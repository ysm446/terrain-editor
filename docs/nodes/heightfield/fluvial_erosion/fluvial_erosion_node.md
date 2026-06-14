# Droplet Erosion / Fluvial Erosion ノード

粒子ベースの河川浸食ノードです。入力ハイトフィールドに対して、水が流れた谷・細い
水路・削れた場所・堆積した場所を加えます。方式の異なる **2 つの独立ノード**として
提供します。どちらも入力 `Heightmap`、出力 `Heightmap` / `Flows` / `Deposits`。

> これらは 0.20.0 で、現行アーキテクチャ（`src/evaluation/` 分離・Multi-Scale
> Erosion と同じ配線）に合わせて**クリーンに作り直したもの**です。かつて存在した
> KTT OpenCL カーネルの完全移植版（削除済み）とは別実装です。KTT の考え方の参考資料は
> 同フォルダの [ktt_fluvial_erosion_algorithm_guide.md](ktt_fluvial_erosion_algorithm_guide.md) /
> [fluvial_erosion_hda_notes.md](fluvial_erosion_hda_notes.md) に残しています。

## 入出力（両ノード共通）

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 出力 | `Heightmap`（侵食後の高さ） |
| 出力 | `Flows`（流れの通過量。マスク。log 圧縮して正規化） |
| 出力 | `Deposits`（堆積量。マスク） |

`Flows` / `Deposits` 出力ピンをクリックすると 2D / 3D プレビューの可視化チャンネルが
切り替わります。両出力はマスクとして下流の `Mask Blend` などへ接続できます。

## Droplet Erosion

業界標準の液滴（droplet）水力浸食です。水滴を勾配に沿って `Inertia` 付きで流し、
勾配・速度・水量から決まる運搬容量（capacity）まで土砂を拾い、過飽和になったり
登り坂になったら落とします。樹状の水路を刻み、谷底へ土砂を堆積させます。

| パラメータ | 役割 |
| --- | --- |
| Particle Count | 流す水滴の数（多いほど密で滑らか、計算は重い） |
| Max Lifetime | 1 水滴が移動する最大ステップ数 (既定 256。短いと水路が中腹で途切れる) |
| Erosion Strength | 1 ステップあたりの削り率 |
| Inertia | 0 = 勾配に正確に従う、大 = 直前の方向を保つ |
| Min Slope | ほぼ平坦でも土砂を運べるようにする勾配の下限 |
| Use Multigrid | 粗→細のピラミッド処理（大きな谷を先に形成し解像度に安定） |
| Seed | 水滴散布の乱数シード（再現性） |
| Sediment Capacity | 1 水滴が運べる土砂量（勾配・速度・水量でスケール） |
| Deposition Strength | 過飽和の水滴が土砂を落とす率 |
| Evaporation | 1 ステップあたりの水の損失 (既定 0.005。容量 ∝ 水量なので大きいと末端で堆積が集中する) |
| Gravity | 下り坂での加速度（速いほど運搬量が増える） |
| Erosion Radius | 削りと容量超過堆積を広げるブラシ半径（セル）。単セルの穴・瘤を防ぐ |

## Fluvial Erosion（KTT 風）

勾配＋ wear フィードバックの力場で粒子を流し、実効勾配（地形勾配と、粒子の運動量を
等価勾配に換算した値の大きい方）に比例して地形を削って (stream power 方式)、削った
土砂を運搬し、実効勾配が `Deposit Angle` を下回ったところへ堆積させます。運動量を
含めることで、斜面が緩む中腹〜山麓でも勢いのある流れが削り続け、水路が尾根から
谷底まで連続します。削りは粒子が降下方向へ動いている間のみ発生し（窪地で往復する
粒子が点状の穴を掘るのを防止）、勢いを失った粒子は土砂を置いて窪地を埋め戻します。
`Channeling` が堆積の一部を破棄し、川床が埋め戻らず残ります。セルごとの高さ変化は
ソフト飽和付きの合計で適用するため、粒子が集中するセルほど深く掘れて樹枝状の
水路網が自己強化されます（1 反復あたりの変化はセルサイズ比例の上限でクランプし、
スパイクを防止）。パラメータは KTT Fluvial Erosion HDA の体系に合わせています。

| グループ | パラメータ | 既定 | 範囲 | 役割 |
| --- | --- | ---: | ---: | --- |
| Basic | Feature Size (m) | 16 | 1-64 | 扱う最大特徴スケール。マルチグリッドの開始（粗）レベルを決める |
| Basic | Geological Age | 20 | 0-20 | 侵食されてきた長さ。全体的な侵食ゲイン |
| Basic | Simulation Iterations | 25 | 0-100 | レベルごとの 力場+輸送 パス回数 |
| Basic | Channel Length (m) | 512 | 0-1024 | 粒子が流れに沿って進む距離（セルサイズでステップ数に換算）。尾根→谷底の距離が目安 |
| Sedimentation | Erosion Strength | 0.5 | 0-1 | 勾配比例の削り量のスケール |
| Sedimentation | Channeling | 0.25 | 0-1 | 堆積分を一部破棄し、水路を刻んだまま保つ |
| Transport | Friction | 0.1 | 0-1 | 1 ステップあたりの速度減衰 |
| Transport | Wear Angle (deg) | 3 | 0-90 | 粒子が削り始める最小の実効勾配角度 |
| Transport | Deposit Angle (deg) | 2 | 0-90 | 実効勾配がこれ未満になると運搬中の土砂を堆積させる |
| Transport | Max Erosion Angle (deg) | 45 | 0-90 | これを超える急斜面は削らない |
| Shaping | Erosion Granularity | 10 | 0-100 | 粒子密度（1 パスで粒子を置くセルの割合 %） |
| Shaping | Flow Volume | 0.35 | 0-1 | 侵食痕（wear）を力場へ戻し水路を自己強化 |
| Shaping | Small Channel Influence | 0 | 0-1 | 細かいレベルでの粒子密度を上げ小支流を増やす |
| Shaping | Sediment Velocity | 1 | 0-2 | 粒子の速度倍率 |
| Advanced | Use Multigrid | ON | - | 粗→細のピラミッド処理 |
| - | Backend | GPU | CPU/GPU | 実行バックエンド。GPU (D3D12 compute) は固定小数点アトミック集積で完全決定的、通常 10 倍以上高速。失敗時は CPU に自動フォールバック |

> 角度しきい値（Wear / Deposit / Max Erosion Angle）は、力場の勾配をセルサイズで
> 割って rise/run（tan）として比較します。これを割らずに比較すると、実地形では
> 1 セルの高さ差が tan 値を容易に超えて判定が常に外れ、侵食がほぼ発動しません。
> Reference Detail Size / Source Terrain Detail Smoothing / Directionality は KTT UI に
> ありますが、本ノードでは内部係数として扱い表に出していません。

## アルゴリズムの要点

- Droplet の粒子ループは高さを直接書き換えるため逐次実行（決定論的）。Fluvial は
  反復ごとに高さ・wear をスナップショット固定し、全粒子をスナップショットに対して
  並列トレースして原子的に集積、反復末尾でまとめて適用します（KTT の GPU モデルと
  同じ構造で、こちらも決定論的）。粒子の散布は `Seed` から生成した疑似乱数で地形
  全体へ均等に配置します。
- `Use Multigrid` 有効時は `kCoarsestPyramidLevel`(64) から目標解像度まで bilinear
  アップサンプルしながら各レベルで浸食します。粗いレベルほど粒子数を面積比で減らし、
  密度を一定に保ちます。Multi-Scale Erosion と同じ思想です。
- `Flows` は流量集積が長い裾を持つため `log(1 + flow)` で圧縮してから [0,1] 正規化し、
  樹状ドレナージが見やすくなるようにしています。
- 実装は `src/evaluation/DropletErosion.{h,cpp}` と `src/evaluation/FluvialErosion.{h,cpp}`。
  両者の共有ヘルパ（bilinear サンプリング・splat・マルチグリッド駆動）は
  `src/evaluation/ParticleErosionCommon.h` に切り出しています。`node_graph.cpp` は
  dispatch のみ。

## GPU バックエンド (Fluvial)

`shaders/fluvial_erosion_compute.hlsl` + `src/gpu/FluvialErosionCompute.cpp`。
CPU 版と同じ「スナップショット凍結 → 全粒子並列トレース → 集積 → 適用」構造を
そのままディスパッチに写しています (1 スレッド = 1 粒子)。HLSL に float アトミック
がないため、スプラットは固定小数点 (1/4096 m) の `InterlockedAdd` で集積します。
加算順序に依存しないので GPU 版は完全決定的です (CPU 版の float アトミックは
順序非決定のため微小に揺れる)。マルチグリッドのレベル間 bilinear アップサンプルも
GPU 上で実行し、CPU との転送は最初のアップロードと最後のリードバック
(heights / flows / deposits) のみ。flows の log 圧縮と各フィールドの正規化は
リードバック後に CPU の `FinalizeLevel` を共用します。

## 今後の候補

- Droplet Erosion の GPU バックエンド (Fluvial と同じ基盤を流用可能)。
- Erosion Mask / Hardness Mask 入力、`age` / `wear` 出力。
