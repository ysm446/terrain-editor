# Multi-Scale Erosion ノード設計メモ

このメモは Terrain Editor の `Multi-Scale Erosion` ノードの実装方針と、出典となる論文・参照実装を整理したものです。

## 出典

| 区分 | 内容 |
| --- | --- |
| 論文 | Schott, Galin, Guérin, Peytavie, Paris. *Terrain Amplification using Multi-scale Erosion*. ACM Transactions on Graphics (SIGGRAPH 2024). |
| 著者ページ | <https://h-schott.github.io/p/mserosion/> |
| 参照実装 | <https://github.com/H-Schott/MultiScaleErosion> (MIT License, OpenGL 4.3 + GLSL コンピュートシェーダー) |

参照実装の `data/shaders/erosion.glsl` (Stream Power Erosion)、`thermal.glsl` (タラス崩壊)、`deposition.glsl` (堆積) の 3 本のコンピュートシェーダーを CPU C++ へ移植したものが本ノードです。OpenGL の SSBO (`std430`) を `std::vector<float>` のピンポンバッファへ置き換え、`gl_GlobalInvocationID` を 2 重ループへ展開しています。

KTT 由来の `Fluvial Erosion` ノードと同じ「水流による河川浸食」カテゴリですが、離散化方式が異なります。

| 観点 | KTT (`Fluvial Erosion`) | Schott (`Multi-Scale Erosion`) |
| --- | --- | --- |
| 離散化 | ラグランジュ法 (粒子輸送) | オイラー法 (グリッド全体での流量解析) |
| 得意な見た目 | 蛇行・編組流路、谷底のディテール | 樹枝状の谷ネットワーク、稜線シャープ化 |
| 出力 | Heights / Flows / Deposits / Age | Heights / Flows / Deposits |
| 並列化 | 粒子ごとに散布する都合で工夫が必要 | セルごとに独立、コンピュートシェーダー向き |

両者は補完関係にあるため、KTT を残したまま追加します。

## 目的

ノイズや `Shape` ノードで作った大雑把なベース地形に対して、河川浸食 (SPE)・タラス崩壊 (Thermal)・堆積 (Deposition) の 3 種類のグリッドベース処理を組み合わせて掛けることで、

- 樹枝状の谷ネットワークと稜線
- 急斜面の崩落と緩斜面化
- 谷底や合流部への土砂堆積

を出すアンプリフィケーション (terrain amplification) ノードです。`HeightmapLoad` または `Shape` の下流に挿し、必要に応じて `HeightmapBlur` などと組み合わせて使います。

## ノードのピン

| ピン | 種別 | 内容 |
| --- | --- | --- |
| `Heightmap` (入力) | Heightmap | 元地形 |
| `Heightmap` (出力) | Heightmap | 浸食後の高さ |
| `Flows` (出力) | Mask | 流量累積 (D8 重み付きフロー) |
| `Deposits` (出力) | Mask | 堆積した土砂量 |

`Flows` と `Deposits` のプレビューは、出力ピンをクリックして 2D / 3D ビューで確認できます。

## 3 つのパスの数式

参照実装の各シェーダーをほぼそのまま CPU 化しています。本数式は CPU 移植版 (`ApplyMultiScaleErosion` in `src/node_graph.cpp`) に対応します。

すべてのパスは反復ごとにピンポンバッファ間で更新します。各セル `p` での近傍は D8 (8 近傍)、近傍方向への勾配は

```
slope(p, q) = (h(q) - h(p)) / |q - p|
```

で定義します。

### Stream Power Erosion (SPE) — 河川浸食

`erosion.glsl` 相当。各セル `p` の流量 `stream(p)` を D8 重み付きフローで累積し、最大勾配方向の receiver より下に行かないように高さを削ります。

1. **D8 重み付きフロー**: 下り近傍 `q` に対し、`weight(q) = pow(slope(q, p), flow_p)` を正規化したものを「どの近傍に流れるか」の重みにします (`flow_p` は 1.3 が既定)。
2. **流量累積**: 各セルの流量は基準量 `1 * |cellDiag|` に上流からの寄与を足したもの:
   ```
   stream(p) = |cellDiag| + Σ_q weight(q→p) * stream(q)
   ```
   (前イテレーションの `stream` を読み、今イテレーションの `stream` に書く前進反復)
3. **Stream Power**: 最大勾配 `s_max` と流量から削り量 `spe` を作ります:
   ```
   spe(p) = clamp(stream(p)^p_sa * clamp(s_max^p_sl, 0, 1), 0, max_spe) * k
   ```
4. **削る**: `h(p) -= dt * spe(p)`、ただし receiver の高さより下にはしない。

### Thermal — タラス崩壊

`thermal.glsl` 相当。3×3 ステンシルで角度しきい値 `θ` を超える近傍がいれば、その分だけ matter を受け取り / 配ります。境界は **wrap-around** (シェーダー実装と同じ)。

```
distributeMul(p) = #{q ∈ 3x3(p) : (h(p) - h(q)) / |p - q| > tan(θ)}
receiveMul(p)    = #{q ∈ 3x3(p) : (h(q) - h(p)) / |p - q| > tan(θ)}
h'(p) = h(p) + ε * cellArea * (receiveMul - distributeMul)
```

オプションで `θ` を簡易ノイズで `[noise_min, noise_max] * tan(θ)` の範囲に空間的に揺らし、岩質の不均一を表現します。

### Deposition — 堆積

`deposition.glsl` 相当。`stream` の他に土砂量フィールド `sed` をピンポンします。

1. **流量と土砂の前進**: 上流から重み付き D8 で `stream` と `sed` を集めます。雨量 `rain * cellArea` を `stream` に足します。pit セルでは `sed = 0`。
2. **堆積条件**: 流速由来の搬送能 `streamPower = stream^0.3 * clamp(s_max^2, 0, 1)` と比較し、土砂が運べる量を超えたら 10 % を堆積:
   ```
   if (deposition_strength * sed > streamPower) {
       deposit = min(sed, (deposition_strength * sed - streamPower) * 0.1)
       h += deposit
       sed -= deposit
   }
   sed += 0.1 * streamPower   // 浮遊土砂を増やす
   ```

## 反復構成

1 ノード内で `iterations` 回、`SPE → Thermal → Deposition` の順に各パスを回します。各パスは個別に ON/OFF できます。

```
for i in 1..iterations:
    if enableStreamPower then  step_SPE
    if enableThermal     then  step_Thermal
    if enableDeposition  then  step_Deposition
```

参照実装では「erosion → thermal → deposition」の順を推奨しています。

## マルチグリッド (Use Multigrid)

`useMultigrid` (既定 ON) で論文本来のマルチスケール処理が有効になります。粗い解像度から目標解像度へ x2 でアップサンプリングしながら、各段で `Iterations` 回ずつ上記の三組を反復します:

```
levels = [64, 128, ..., target_resolution]   // x2 で増やす
heights = bilinear_downsample(input, levels[0])
for level in levels:
    if level > prev: heights = bilinear_upsample(heights, level)
    for i in 1..iterations:
        step_SPE / step_Thermal / step_Deposition
```

**効果:**
- 粗いレベルで `path_length / cellSize` が小さく、`stream` 累積が少ない反復で steady state に達するため、大局の谷ネットワークが安定して決まります。
- 細かいレベルは大局構造を引き継ぎつつ細部だけを追加するので、**解像度を変えても大局の谷の位置と本数がほぼ変わらない**結果になります。
- 各段の cell 数は粗いほど少なく (1/4, 1/16, 1/64...)、並列化と組み合わせて全体の評価時間も控えめです (典型的に単一段階の 1.3 倍程度)。

**OFF (単一段階モード):** 入力解像度に対して 1 段階だけ反復します。`Iterations` の意味が「総反復数」になります。実験的にチューニングしたい場合や、プレビューを高速化したい場合に有用です。

## パラメータ一覧

すべて参照実装の uniform に対応します。括弧内が対応シェーダー名 / uniform 名。

### 反復

| パラメータ | 既定 | 範囲 | 役割 |
| --- | --- | --- | --- |
| `Iterations` | 50 | 0–500 | 1 ノード内で SPE→Thermal→Deposition を繰り返す回数。Multigrid 有効時は各レベルでの反復数 |
| `Use Multigrid` | true | – | マルチグリッドピラミッド処理 ON/OFF (上記参照) |
| `Enable Stream Power` | true | – | SPE パス ON/OFF |
| `Enable Thermal` | true | – | Thermal パス ON/OFF |
| `Enable Deposition` | true | – | Deposition パス ON/OFF |

### Stream Power Erosion (`erosion.glsl`)

| パラメータ | 既定 | 範囲 | uniform | 役割 |
| --- | --- | --- | --- | --- |
| `SPE Strength` | 0.004 | 0–0.01 | `k` | 削り量の倍率 |
| `Stream Exponent` | 0.9 | 0–2 | `p_sa` | 流量に対する非線形性 |
| `Slope Exponent` | 2.0 | 0–4 | `p_sl` | 勾配に対する非線形性 |
| `Max Stream Power` | 10000 | 1–1e6 | `max_spe` | 削り量の上限 |
| `Flow Exponent` | 1.3 | 0.5–4 | `flow_p` | D8 重み付きフローの集中度 |
| `Time Step` | 1.0 | 0–4 | `dt` | 反復あたりの時間刻み |

### Thermal (`thermal.glsl`)

| パラメータ | 既定 | 範囲 | uniform | 役割 |
| --- | --- | --- | --- | --- |
| `Threshold Angle (deg)` | 30 | 0–60 | `tanThresholdAngle` | 安息角。これを超える勾配は崩落 |
| `Thermal Strength` | 0.005 | 0–0.01 | `eps` | 反復あたりの輸送量 |
| `Noisify Angle` | true | – | `noisifiedAngle` | 安息角を空間ノイズで揺らす |
| `Noise Min` | 0.9 | 0–4 | `noise_min` | tan(角度) 倍率の下限 |
| `Noise Max` | 1.4 | 0–4 | `noise_max` | tan(角度) 倍率の上限 |
| `Noise Wavelength` | 0.0023 | 0–0.05 | `noiseWavelength` | 角度ノイズの空間周波数 |

### Deposition (`deposition.glsl`)

| パラメータ | 既定 | 範囲 | uniform | 役割 |
| --- | --- | --- | --- | --- |
| `Deposition Strength` | 0.2 | 0–8 | `deposition_strength` | 搬送能を超えた分の堆積率 |
| `Rain` | 2.6 | 0–10 | `rain` (定数) | セルあたりに降る水量 |

## 境界条件

参照実装と一致させています。

- **SPE / Deposition**: クランプ (範囲外は `0` を返す `Slope` / `Stream` / `Sed`)。
- **Thermal**: ラップアラウンド (3×3 サンプリングが `% (nx, ny)`)。

## 解像度不変性

参照シェーダーは `eps * cellArea` (Thermal の matter) と `rain * cellArea` (Deposition の流量基準) を **実 cellSize の二乗** で計算します。これは解像度を変えると 1 反復あたりの効果が cellSize² で変わってしまい、同じパラメータで解像度だけ変えると見た目が大きくドリフトします (Thermal が高解像度ほど効かなくなり、SPE の V 字が丸まらない)。

本実装ではこれらの `cellArea` を **基準 cellSize = 4 m** (= 解像度 512 / 2048 m terrain) で固定し、解像度に依存しないように補正しています:

```cpp
constexpr float kRefCellSize = 4.0f;
constexpr float kRefCellArea = 16.0f;  // = 4 m × 4 m
matter   = thermalStrength * kRefCellArea;
cellArea = kRefCellArea * 0.00001f;     // for rain
```

これで 1 反復あたりの再配分量と雨量寄与は cellSize に依存しなくなります。基準 4 m での既存チューニングは挙動が変わりません。

ただし **SPE の `stream` 累積** はピンポン式の前進反復で、長距離流路が成熟するのに必要な反復回数が `path_length / cellSize` に比例します。完全な解像度不変は構造上不可能なので、解像度を上げた場合は `Iterations` を比例的に増やすのが推奨です (例: 512 → 1024 で iterations を 50 → 80〜100)。

## 出力フィールドの正規化

評価後、`flows` と `deposits` を最大値で `[0, 1]` に正規化してマスクとして返します (`NormalizeHeightfieldFields`)。これは KTT ノードと同じ仕様です。

## 今後の課題

| 項目 | 内容 |
| --- | --- |
| マルチスケールラッパー | 論文本来の「粗→細を交互に走らせ、x2 アップサンプル」を内部に持たせる。`useMultigrid` 風のフラグ + 段別強度配列で KTT と同じ構成にできる。 |
| GPU Compute 移植 | 参照実装は OpenGL 4.3 のコンピュートシェーダー。Terrain Editor は D3D12 + HLSL なので、SSBO ↔ StructuredBuffer の置換と GLSL ↔ HLSL の翻訳が必要。KTT が辿った CPU → GPU と同じパスを踏める。 |
| Hardness 入力 | 参照実装は `Hardness` SSBO を持つが活用していない。本ノードでも当面ダミー扱い。マスク入力ピンを足せば KTT と同じく erodibility を渡せる。 |
| 角度ノイズの 3D 化 | 参照実装は `snoise(vec3(x, y, height))`。本実装は 2D 簡易ノイズ。標高依存の岩質変化が要るなら差し替え。 |
| 雨量フィールド | 現在 `rain` は定数。空間的に変化する雨量を入力できると流域の偏りが作りやすい。 |

## 関連ファイル

- 実装: `src/node_graph.cpp` の `ApplyMultiScaleErosion`、`HashMultiScaleErosionSettings`
- 設定構造: `src/node_graph.h` の `MultiScaleErosionSettings`
- UI / シリアライズ: `src/main.cpp` の `MultiScaleErosionRows` テーブルと JSON ラウンドトリップ
- 参考シェーダー (出典): `data/shaders/erosion.glsl` / `thermal.glsl` / `deposition.glsl` (Schott et al. リポジトリ内)
