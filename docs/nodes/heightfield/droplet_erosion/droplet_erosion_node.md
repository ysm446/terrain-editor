# Droplet Erosion ノード

粒子（液滴）ベースの河川浸食ノードです。入力ハイトフィールドに対して、水が流れた
谷・細い水路・削れた場所・堆積した場所を加えます。入力 `Heightmap`、出力
`Heightmap` / `Flows` / `Deposits`。

> このノードは 0.20.0 で、現行アーキテクチャ（`src/evaluation/` 分離・Multi-Scale
> Erosion と同じ配線）に合わせて**クリーンに作り直したもの**です。かつて存在した
> KTT OpenCL カーネルの完全移植版（削除済み）とは別実装です。力場粒子輸送方式の
> 姉妹ノード `Fluvial Erosion` は
> [../fluvial_erosion/fluvial_erosion_node.md](../fluvial_erosion/fluvial_erosion_node.md) を参照してください。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 出力 | `Heightmap`（侵食後の高さ） |
| 出力 | `Flows`（流れの通過量。マスク。log 圧縮して正規化） |
| 出力 | `Deposits`（堆積量。マスク） |

`Flows` / `Deposits` 出力ピンをクリックすると 2D / 3D プレビューの可視化チャンネルが
切り替わります。両出力はマスクとして下流の `Mask Blend` などへ接続できます。

## 概要

業界標準の液滴（droplet）水力浸食です。水滴を勾配に沿って `Inertia` 付きで流し、
勾配・速度・水量から決まる運搬容量（capacity）まで土砂を拾い、過飽和になったり
登り坂になったら落とします。樹状の水路を刻み、谷底へ土砂を堆積させます。
段階的な解説は [droplet_erosion_algorithm_guide.md](droplet_erosion_algorithm_guide.md) を参照してください。

## パラメータ

| パラメータ | 役割 |
| --- | --- |
| Backend | 実行バックエンド (CPU / GPU)。GPU は逐次依存をスナップショット方式に置換し決定的・高速だが、CPU 版とビット一致はしない（視覚的に同等）。失敗時は CPU に自動フォールバック |
| Droplet Density | 1 セルあたりに流す水滴数（絶対数ではないので解像度を変えても結果が一貫。多いほど密で滑らか、計算は重い） |
| Travel Distance (m) | 1 水滴が消えるまでに進む距離（メートル。セルサイズでステップ数に換算。既定 512 m。短いと水路が中腹で途切れる） |
| Erosion Strength | 1 ステップあたりの削り率 |
| Inertia | 0 = 勾配に正確に従う、大 = 直前の方向を保つ |
| Min Slope | ほぼ平坦でも土砂を運べるようにする勾配の下限 |
| Use Multigrid | 粗→細のピラミッド処理（大きな谷を先に形成し解像度に安定） |
| Seed | 水滴散布の乱数シード（再現性） |
| Sediment Capacity | 1 水滴が運べる土砂量（勾配・速度・水量でスケール） |
| Deposition Strength | 過飽和の水滴が土砂を落とす率 |
| Evaporation (/m) | 1 メートル進むごとの水の損失 (セルサイズで複利圧縮。既定 0.002 /m。容量 ∝ 水量なので大きいと末端で堆積が集中する) |
| Gravity | 下り坂での加速度（速いほど運搬量が増える） |
| Erosion Radius (m) | 削りと容量超過堆積を広げるブラシ半径（メートル。内部でセル数に換算）。単セルの穴・瘤を防ぎ、幅が解像度によらず一定になる |

## アルゴリズムの要点

- CPU 版の粒子ループは高さを直接書き換えるため逐次実行（決定論的）。粒子の散布は `Seed`
  から生成した疑似乱数で地形全体へ均等に配置します。GPU 版はこの逐次依存をスナップショット
  方式に置き換え（反復ごとに凍結した高さへ全水滴を並列トレースし固定小数点で集積）、
  決定的かつ高速ですが結果は CPU 版とビット一致しません。詳細は
  [droplet_erosion_algorithm_guide.md](droplet_erosion_algorithm_guide.md) の「バックエンド」節を参照。
- `Use Multigrid` 有効時は `kCoarsestPyramidLevel`(64) から目標解像度まで bilinear
  アップサンプルしながら各レベルで浸食します。粗いレベルほど粒子数を面積比で減らし、
  密度を一定に保ちます。Multi-Scale Erosion と同じ思想です。
- `Flows` は流量集積が長い裾を持つため `log(1 + flow)` で圧縮してから [0,1] 正規化し、
  樹状ドレナージが見やすくなるようにしています。
- 実装は `src/evaluation/DropletErosion.{h,cpp}`。姉妹ノード `Fluvial Erosion` との
  共有ヘルパ（bilinear サンプリング・splat・マルチグリッド駆動）は
  `src/evaluation/ParticleErosionCommon.h` に切り出しています。`node_graph.cpp` は
  dispatch のみ。

## 今後の候補

- Droplet Erosion の GPU バックエンド（`Fluvial Erosion` と同じ基盤を流用可能）。
- Erosion Mask / Hardness Mask 入力、`age` / `wear` 出力。
