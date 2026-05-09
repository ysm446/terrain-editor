# Sediment ノード

入力ハイトフィールドを岩盤(bedrock)とみなし、上に載せた **均一な土砂レイヤー** を確率的な水力侵食パーティクルで再分配します。GeoGen の Sediment ノード相当で、土砂が斜面を流れて谷や窪みに堆積し、**dendritic な流路に沿った筋状の堆積パターン**が出ます。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `HeightField`(bedrock として固定、岩盤とみなす) |
| 出力 | `Heightmap`(bedrock + 残った土砂)、`Mask`(土砂厚みを 0..1 に正規化) |

## アルゴリズム

各粒子は `(pos, dir, velocity, water, carried)` を持ち、以下のループで動きます:
1. 現在位置で双線形勾配を計算
2. `dir = inertia × dir + (1 - inertia) × (-gradient)`
3. 単位ベクトル化して 1 セル進む
4. 新位置の高さ差 `dh = oldH - newH` を計算
5. 容量 `c = max(dh, 0.01) × |v| × water × Kc`
6. `carried < c` なら `(c - carried) × Ke` を侵食。岩盤を掘らない sediment-only モデルなので、**`dh` による侵食キャップは無し**(古典的な hydraulic erosion のキャップを撤廃)、各セルの利用可能 sediment 量だけで cap。
7. `carried > c` なら `(carried - c) × Kd` を旧位置に堆積
8. `velocity = sqrt(v² + dh × 4)`、`water *= 1 - Kev`
9. 寿命到達 or 水切れ で残り全堆積

## 主な設定

| 設定 | 既定値 | 役割 |
| --- | --- | --- |
| `Initial Sediment (m)` | 2.0 | 全セルに最初に積む土砂の厚み。再分配で動かせる絶対量を決める(薄すぎると地形に埋もれる) |
| `Mask Contrast (%)` | 70 | Mask 出力のコントラスト。`smoothstep((initial×2) 周辺で遷移)`。0 で連続グラデーション、1 でほぼバイナリ。GeoGen 風くっきり dendritic は 0.7+ |
| `Iterations` | 30 | 粒子を投入する回数(波数)。各 wave 後に sediment が更新され、次 wave は侵食済み地形を見る。GeoGen の Iter X 相当 |
| `Particle Count` | 5,000 | 1 wave で発射する粒子数(総粒子数 = `Iterations × Particle Count`) |
| `Particle Lifetime` | 128 | 1 粒子の最大ステップ数 |
| `Gradient Distance (m)` | 8 | 勾配サンプリングの距離(メートル、解像度非依存)。GeoGen の "Largest Detail Level" 相当。4m で局所地形に追従(細かい筋状)、8m がほどよい集約、16m+ で主要谷強調(ただし収束が強くなりスパイク出やすい) |
| `Inertia (%)` | 40 | 前ステップ方向への引き継ぎ率。高めにして小さな pit / ノイズに引っ掛からず長く流れるように |
| `Friction (%)` | 5 | 1 ステップごとに失われる速度の割合。長い斜面で velocity が無制限に増えるのを防ぐ。低いほど重力加速感が強い、上げるとスパイク防止に有効 |
| `Capacity` | 4.0 | 容量係数 Kc |
| `Erosion (%)` | 30 | 1 ステップで容量差から侵食する割合(available sediment による cap のみ) |
| `Deposition (%)` | 30 | 1 ステップで過剰分から堆積する割合(低いほど粒子が長く運んでから徐々に吐く) |
| `Evaporation (%)` | 2 | 1 ステップでの水の喪失率 |
| `Emission Time (%)` | 0 | 総粒子予算 (`Iterations × Particle Count`) を先頭何割の wave に集中させるか。0% は **全粒子を 1 wave に集中**(初期地形だけを侵食、wave 間 merge なし → 樹枝状がシャープ)、100% は従来どおり毎 wave に `Particle Count` ずつ均等(progressive な彫り込みと平滑化)。総仕事量は不変で、wave 間 merge の粒度のみ変わる |
| `Seed` | 0 | 粒子位置の乱数シード |

## CPU 並列化

粒子をスレッドに均等分割し、各スレッドが **自分専用の delta マップ**(`std::vector<float>` × N スレッド)に書き込む。同期は最終マージのみ。スレッド間の粒子相互作用は弱まる(他スレッドの侵食/堆積が走査中に見えない)が、レース回避のために標準的な選択。1024² × 100k 粒子 × 64 step で ~500ms 程度。

## 用途の使い分け

| 目的 | 推奨パラメータ |
| --- | --- |
| 谷の樹枝状堆積(GeoGen 参考画像) | `Particle Count 200000` / `Lifetime 64` / `Initial Sediment 2-5m` |
| 軽い土砂層を斜面から谷へ | `Initial Sediment 0.5m` / `Lifetime 30` |
| シャープな初期侵食を強調 | `Emission Time 0%`(全粒子を 1 wave で初期地形に投入) |
| Progressive な彫り込み・平滑化 | `Emission Time 100%`(波間 merge を毎回挟む) |

## メモ

- Heights 出力は **bedrock + 残った土砂**(加算ではなく、bedrock の上に sediment レイヤを重ねた最終地形)。
- Mask 出力は **土砂厚みを max 値で正規化** した 0..1 マスク。GeoGen の白い堆積マスクに対応。
- `Seed` を変えると粒子配置が変わるので、決定論を求めるならシード固定。
- パフォーマンスは `Iterations × Particle Count × Lifetime` に線形依存。プレビュー解像度を下げると高速化。
- キャッシュは入力ハッシュ + パラメータハッシュで他ノードと同様。粒子数変更でも個別に再評価。
