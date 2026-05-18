# 計画

このファイルは、次に何をするかを相談しながら整理するための場所です。

## 現在の方針

- Terrain Editor の中核体験であるノード、プレビュー、保存形式を安定させる。
- 設定タブの `Terrain Size (m)` を地形キャンバスのグローバルな縦横サイズとして扱い、各ノードのメートル単位パラメータはこの制作スケールに対して解釈する。
- 追加したノードや表示機能は、`docs/nodes/`、`docs/changelog.md`、必要に応じて `progress.md` に記録する。
- 重いノードは CPU 実装を基準にしつつ、D3D12 GPU compute 化できるものから段階的に高速化する。
- 地形制作の基本的な流れは `docs/plan/terrain_workflow.md` を参照し、岩、土、植生、砂礫、水域の役割を分けて考える。

## 次の優先候補

1. `Crumbling` の見た目調整
   - 初期実装は完了。
   - 次は、低い方向へ流れるまとまりを残しつつ、岩屑がもう少しランダムに散るようにする。
   - 優先案は `Spread (%)` の追加。`Gravity` は低い方へ流れる強さ、`Spread` は進行方向から横へ逸れる強さとして扱う。
   - 追加候補として、サイズ依存移動、`Bounce` / `Deflection`、`Path Noise`、`Emission Jitter` を検討する。

2. `Scatter` の検証と植生分布ワークフロー
   - `Scatter` は追加済みで、`Hemisphere` / `Cone`、`Mask`、`Unique Mask`、GPU backend に対応済み。
   - 当面は植生分布用のプロキシとして使う。
   - GPU 経路は `Mask` 入力なし、`Ground Detail Level = Max` の場合に使うため、マスク入力ありの実用ケースで速度や結果を確認する。
   - 将来の専用 `Vegetation` ノードに向けて、Scatter で不足する制御を観察する。

3. 水系ノードの仕様整理
   - `River` / `Lake` は未実装。
   - まず地形データ側の水域生成と、ビューポート表示側の水表現を分けて設計する。
   - 出力候補は、掘り込み済み `Heightmap`、水域 `Mask`、水深 `Mask`、表示用の水面情報。
   - `Spline` や `Mask Fluvial` を水路ガイドとして使えるかを検討する。
   - 最初の実装候補として、始点と終点を置くと、その間の川筋を探索して `River Mask` を作る `River Path Mask` 的なノードを検討する。
   - もう一つの候補として、パス指定なしで入力地形から自然な川網の `Mask` を生成するモードも検討する。これは地形全体の drainage / flow accumulation を使い、主要な谷筋を自動抽出する。

4. `Spline` ノードの設計
   - 未実装。
   - まずパス編集 UI、保存形式、ノードグラフ上の入力/出力、既存 Heightmap へ加算するか単独生成にするかを整理する。
   - 山脈や尾根の土台作りを主目的にしつつ、将来の `River` / `Lake` の水路ガイドとしても使える設計を検討する。

5. マスク・カラー系の仕上げ
   - `Mask Height`、`Mask Slope`、`Mask Curvature`、`Mask Levels` は実装済み。
   - `Mask Invert` は `Mask Levels` の `Invert` で代用できるため、単独ノードとして本当に必要か保留する。
   - `Colorize` は実装済み。今後は色ピック対象、サンプル方法、グラデーションキー化を必要に応じて調整する。

## 実装済みの大きな基盤

- グローバル `Terrain Size (m)`。
- `Import Heightmap` の `Scale (m)` とプロジェクト相対パス保存。
- ハイトマップ参照を持たないマスクプレビューで、近い地形を表示に使うオプション。
- `Crumbling`、`Rock`、`Scatter`、`Sediment`、`Snow`、`Mask Fluvial` などの主要な地形ノード。
- `Rock` / `Scatter` / `Sediment` / `Snow` / `Mask Fluvial` / `Mask Noise` / `Multi-Scale Erosion` などの GPU backend。
- カメラ初期化、`F` キーリセット、Depth of Field、GPU Displacement / Tessellation、地形境界表示。

## 確認時の観点

- 既存プロジェクトの読み込み互換性が保たれているか。
- ノードをコピー＆ペーストしたとき、パラメータと上流リンクが期待通り引き継がれるか。
- CPU / GPU backend の切り替えで、失敗時に CPU へフォールバックするか。
- `Terrain Size (m)` を変えたとき、ノードのメートル単位パラメータが直感に合うか。
- `Mask`、`Unique Mask`、`Color Texture` の意味が下流ノードから見て分かりやすいか。

## River Path Mask 案

- 目的は、ユーザーが始点と終点を置くだけで、川の中心線と川幅フェードを持つ `Mask` を生成できるようにすること。
- 入力はまず `Heightmap`。UI では Start Point / End Point を 2D ビューまたは 3D ビューポート上で配置する。
- 初期出力は `Mask` を優先する。後から `Heightmap`、`Depth Mask`、`Bank Mask`、水面表示情報を追加する。
- 最初のアルゴリズム候補は、簡易 A* またはコスト探索で、距離、上り坂ペナルティ、谷筋への寄りやすさ、曲がりすぎペナルティを組み合わせる。
- シンプルな代替案として、2点間の線をノイズで蛇行させ、幅とフェードでマスク化する方式も残す。これは地形追従は弱いが、制御しやすく実装が軽い。
- 主要パラメータ候補は `Width (m)`、`Feather (m)`、`Meander (%)`、`Terrain Follow (%)`、`Uphill Penalty`、`Valley Bias`、`Seed`。
- `River Mask` は中心線からの距離で 1 から 0 へフェードする形にし、後段の `Mask Levels`、`Colorize`、将来の川床掘り込み処理で使いやすくする。
- 実装順としては、まず地形を掘らないマスク生成ノードとして作り、次に `River Carve` 的な川床加工や水面表示へ進める。

## River Auto Mask 案

- 目的は、ユーザーがパスを指定しなくても、入力 `Heightmap` から自然に川ができそうな場所の `Mask` を生成すること。
- 基本方針は、流向、flow accumulation、谷筋、集水面積を見て、地形全体から主要な流路を抽出する。
- `Mask Fluvial` の流路マスク生成と近いが、River 用では川幅、蛇行、岸辺フェード、将来の掘り込みを意識した出力に寄せる。
- 蛇行は重要な見た目要素として扱う。単純に最急降下だけで線を引くと硬く直線的になりやすいため、低地へ向かう制約を守りつつ、谷幅内で左右に揺れる余地を作る。
- 蛇行の作り方候補は、流路中心線への低周波ノイズ、谷底内での横方向オフセット、曲率をなめらかにする後処理、流量が大きいほど蛇行幅を広げる制御。
- 主要パラメータ候補は `River Count`、`Source Density`、`Minimum Flow`、`Width (m)`、`Width by Flow (%)`、`Meander (%)`、`Smoothing (m)`、`Seed`。
- 2点指定モードは「狙った場所に川を通す」ために使い、パスなしモードは「地形から自然な川網を見つける」ために使う、という役割分担にする。

## 保留メモ

- `Mask Invert` を単独ノードとして追加するか。
- `Crumbling` の散らばり改善で、最初にどこまでパラメータを増やすか。
- `Scatter` から専用 `Vegetation` ノードへ分けるタイミング。
- `River Path Mask` を単独ノードにするか、将来の `River` ノードの最初のモードとして入れるか。
- `River Auto Mask` を `Mask Fluvial` の拡張にするか、将来の `River` ノードの自動生成モードとして分けるか。
- 水面メッシュや水域マスクをエクスポート時にどう扱うか。
- OS 全体からの色ピックを `Colorize` に入れるか。まずはアプリ内ピックを優先する。
