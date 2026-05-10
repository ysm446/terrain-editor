# Sediment ノード

入力ハイトフィールドの上 (もしくは入力地形そのもの) に **可動な堆積物レイヤー** を置き、重力でマルチスケールに再分配する GeoGen 互換の堆積シミュレーションノードです。安息角を超えた斜面から土砂が低い隣接セルへスライドし、谷底に厚く堆積、尾根は剥き出しになる **dendritic な堆積パターン** が形成されます。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `HeightField` (基盤として固定 — `Convert Terrain to Sediment` が ON のときは入力高さ全体が可動堆積物として扱われ、基盤は平坦 = 0 になります) |
| 出力 | `Heightmap` (基盤 + 再分配後の堆積物)、`Mask` (堆積厚みを max で 0..1 正規化 → 谷底が明、尾根が暗) |

## アルゴリズム

GeoGen 風の **マルチグリッド・サーマル (talus) スライディング**:

1. **初期化** — `Convert Terrain to Sediment` ON: 基盤 = 0, 堆積物 = 入力高さ。OFF: 基盤 = 入力高さ, 堆積物 = 0。
2. **スケール階層生成** — `Largest Detail Level (m)` をセル単位に変換した stride を最大値とし、1 セルまで毎回半分にします (例: 8m / 4m / 2m / 1m)。
3. **外側反復ループ** (`Iterations Count` 回):
   - **エミッション** — `Emission Amount (m)` を `emissionEnd = ceil(iterations × Emission Time)` 反復で均等加算 (Emission Time = 0% なら最初の 1 反復で全量)。
   - **粗→細スケール走査** — 各 stride で `Stabilization Iterations` 回スライドパスを実行。各パスは:
     - 各セルから 4 近傍 (距離 = stride) への高さ差を見て、`talusH = tan(角度) × cellSize × stride` を超える落差ぶんだけ可動量とみなす。
     - その合計の半分 (`flowRate = 0.5`) を、各方向の落差比で按分して隣接セルへ送る。
     - スレッドセーフのため、第 1 sweep で各セルの「方向別流出量」をスナップショットへ書き、第 2 sweep でそれを `自分の流出 − 4 近傍からの流入 (= 隣接の対方向スロット)` として適用します。
4. **マスク生成** — `mask[i] = sediment[i] / max(sediment)` を `Mask Contrast` の S カーブで補正。

## 主な設定

| 設定 | 既定値 | 役割 |
| --- | --- | --- |
| `Emission Time (%)` | 0 | `Emission Amount` を最初の何割の反復にかけて徐々に積むか。0% = 最初に全量を一度に積む (緩い層が自由に流れて落ち着く)、100% = 毎反復に均等 (前反復が彫った河道に新層が流れ込み、河道がよりシャープに刻まれる) |
| `Largest Detail Level (m)` | 8.0 | マルチグリッドの最も粗いスケール。大きいほど大規模盆地が早く埋まり、小さいほど細部優先。最大は 1/4 グリッドまでクランプ |
| `Iterations Count` | 40 | 外側の緩和反復回数。各反復で全スケールを粗→細で 1 周します |
| `Stabilization Iterations` | 2 | 1 反復・1 スケール内で何回連続でスライドパスを走らせるか。多いほど各スケールがそのスケール内で完全に静定します |
| `Sediment Viscosity (%)` | 20 | 流動性 / 安息角を制御 (二乗カーブ)。0% = 0° (完全流体、谷底で水平面に均される)、20% (既定) ≈ 3° (ほぼ平らな堆積、GeoGen 相当)、50% = 20°、100% = 80° (粘り強く中腹に厚く積もる) |
| `Emission Amount (m)` | 0.5 | 全セルに上乗せする堆積物の総厚 (m)。`Convert Terrain to Sediment` が ON のときは元地形に対する追加分 |
| `Convert Terrain to Sediment` | true | ON: 入力地形全体を可動堆積物として扱い、山頂が崩れて谷を埋める典型 GeoGen 風出力。OFF: 入力は固定基盤、Emission Amount で追加した分だけが流れます |
| `Mask Contrast (%)` | 0 | Mask 出力のコントラスト。0 で線形、1 でほぼバイナリ。dendritic を強調するなら 0.5 以上 |
| `Backend` | GPU Compute | 実行バックエンド (`CPU` / `GPU Compute`)。GPU は D3D12 compute shader で 10-30 倍高速。シェーダーコンパイル / ディスパッチ失敗時は自動的に CPU にフォールバック |

## バックエンド (CPU / GPU)

`Backend` プロパティで実行経路を切り替えられます。

**CPU パス** — 各スライドパスは 2 つの sweep からなり、両方とも `ParallelForRows` で行並列化。作業バッファ `outgoing[4 × n²]` (4 方向 × 全セル) はノード呼び出し全体で 1 回だけ確保し、すべてのパスで再利用。1024² グリッド × `iterations 40` × stab 2 × macro 8 = 640 パスで数秒。

**GPU パス (既定)** — [shaders/sediment_compute.hlsl](../../../../shaders/sediment_compute.hlsl) の D3D12 compute shader で同じアルゴリズムを並列実行。エントリは:
- `CSSetup`: 入力 height から bedrock + sediment を初期化
- `CSEmit`: sediment に emissionPerIter を加算
- `CSSlideSweep1`: 流出シェアを `outgoing[i*4..i*4+3]` に書き出し
- `CSSlideSweep2`: `自身の流出 - 4 近傍の対方向流出` で sediment を更新

CPU 比 10-30 倍高速 (1024² グリッドで 100ms 程度)。シェーダーコンパイル / ディスパッチ失敗時は CPU パスに自動フォールバック。Multi-Scale Erosion / Mask Noise と同じワーカー → メインスレッドキューパターンで非同期評価でも安全に動作します。

## 用途の使い分け

| 目的 | 推奨パラメータ |
| --- | --- |
| GeoGen 風の樹枝状堆積 (参考画像) | 既定値 (Convert Terrain to Sediment = ON, Viscosity 20%, Emission Amount 0.5m) |
| 山頂を強く削り谷を厚く埋める | `Iterations Count 80-150` / `Largest Detail Level 16m` / `Viscosity 10%` |
| 中腹に粘り強く積もらせる | `Viscosity 50-70%` / `Emission Amount 1-2m` |
| 細部の樹枝状を強調 | `Largest Detail Level 4m` / `Stabilization Iterations 4` |
| 既存地形を残し追加層だけ動かす | `Convert Terrain to Sediment OFF` / `Emission Amount 1-3m` |

## メモ

- Heights 出力は **基盤 + 再分配後の堆積物**。`Convert Terrain to Sediment` が ON だと基盤 = 0 なので、出力高さ = 再分配後の堆積物そのもの。
- Mask 出力は **堆積厚みを max で正規化** した 0..1 値。最大堆積セルが必ず 1 になるので、堆積量の絶対値ではなく相対分布を見る用途。
- 流れの安定性は `Sediment Viscosity` × `Stabilization Iterations` でほぼ決まります。低粘性 + 少ない安定化反復だと振動気味になることがあるので、低粘性なら Stab を 4-8 に上げるのが安全。
- キャッシュは入力ハッシュ + パラメータハッシュで他ノードと同様。
