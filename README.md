# Terrain Editor

ハイトフィールドを中心にした、ノードベースの地形エディタの Windows デスクトッププロトタイプです。

もともとは岩生成ツールとして始まりましたが、現在は `Terrain Size (m)` を基準にした地形キャンバス、プロシージャルな地形生成ノード、マスク/カラー編集、3D プレビュー、プロジェクト保存を組み合わせて、地形制作の基本フローを作れるツールへ育てています。

## 現在のプロトタイプ

- C++20 / CMake アプリケーション
- Win32 + DirectX 12 レンダラ
- Dear ImGui ベースの UI
- imgui-node-editor を使ったノードグラフ
- 1 unit = 1 m の地形プレビュー
- `Simulation Resolution` と `Viewport Mesh Resolution` を分けた評価/表示設定
- `Terrain Size (m)` による 512 / 1024 / 2048 / 4096m の地形キャンバス
- `.terrainproj` の JSON プロジェクト保存・読み込み
- 古いサンプルデータ用の `.rockproj` 読み込み
- 未保存変更の `*` 表示と、新規作成・読み込み・終了時の保存確認
- `data/ui_themes` の JSON UI テーマ切り替え
- `data/app_settings.json` への UI テーマ、カメラ、2D マップ、表示設定、ライティング、最近使ったプロジェクトの保存

## 地形制作の流れ

基本的には、起点になる `Import Heightmap` や `Shape` から地形を作り、浸食、岩、土砂、雪、散布、マスク、カラーをノードグラフ上で組み合わせます。

`Terrain Size (m)` はノードグラフ全体の制作スケールです。`Import Heightmap` の `Scale (m)` や、各ノードのメートル単位パラメータは、この地形キャンバスに対して解釈されます。地形制作の考え方は [docs/plan/terrain_workflow.md](docs/plan/terrain_workflow.md) に整理しています。

## ノード

ノードの詳細は [docs/nodes/README.md](docs/nodes/README.md) の索引を参照してください。

### ハイトフィールド系

- **`Import Heightmap`**: 画像ハイトマップを読み込みます。`Scale (m)` はグローバルな `Terrain Size` 内で画像が占める実サイズとして扱います。プロジェクト保存時はパスを相対化します。
- **`Shape`**: 半球やピラミッドなどのプロシージャル形状を生成します。
- **`Heightmap Blur`**: 分離可能ガウシアンでハイトマップを滑らかにします。
- **`Multi-Scale Erosion`**: Stream Power + Thermal + Deposition 系の浸食ノードです。
- **`Rock`**: 岩を散布します。`Mask` 入力、`Unique Mask` 出力、`Rock Style`、`Orientation Rule`、`Ground Detail Level` に対応しています。
- **`Scatter`**: `Hemisphere` / `Cone` の汎用散布ノードです。`Mask` と `Unique Mask` を出力できるため、植生分布用のプロキシとしても使えます。
- **`Crumbling`**: `Emission Mask` から崩落粒子を発生させ、低い方向へ流して岩屑を堆積します。
- **`Sediment`**: GeoGen 互換寄りの土砂スライド/堆積ノードです。
- **`Snow`**: 地形の形状に沿って積雪面を生成します。

### マスク系

- **`Mask Noise`**: Perlin / fBM ベースのマスク発生源です。
- **`Mask Blend`**: `Foreground` / `Background` の 2 つのマスクを合成します。
- **`Mask Levels`**: Black Point / White Point / Gamma / Invert でマスクを整えます。
- **`Mask Height`**: 標高範囲または入力地形の全高レンジからマスクを作ります。
- **`Mask Slope`**: 傾斜角から急斜面/平地マスクを作ります。
- **`Mask Curvature`**: 尾根、谷、絶対曲率のマスクを作ります。
- **`Mask Fluvial`**: 流量累積または粒子通過密度から川筋向けのマスクを作ります。

### カラー系

- **`Colorize`**: `Gradient Mask`、`Mask`、任意の `Base Color` から `Color Texture` を生成します。岩ごとの `Unique Mask` や標高/斜面/曲率マスクと組み合わせて、地形の色分けに使えます。

## GPU backend

重いノードの一部は D3D12 GPU compute に対応しています。GPU 経路が使えない条件やシェーダー実行失敗時は CPU にフォールバックします。

GPU backend 対応済みの主なノード:

- `Mask Noise`
- `Mask Fluvial`
- `Multi-Scale Erosion`
- `Rock`
- `Scatter`
- `Sediment`
- `Snow`
- `Colorize`

`Scatter` の GPU 経路は、現在 `Mask` 入力なし、`Ground Detail Level = Max` の場合に使われます。

## ビューとプレビュー

- 選択中のノードまたは出力ピンを 3D ビューポートと 2D マップでプレビュー
- ノード評価は非同期で実行
- ステータスバーに評価状態と計算時間を表示
- 評価中ノードに `計算中` / `計算待ち` バッジを表示
- `A` キーまたは `Reset View` で現在の `Terrain Size (m)` に合う初期視点へリセット
- `O` キーでカメラの自動回転をオン / オフ
- `D` キーで Depth of Field をオン / オフ
- マスク出力を近い上流地形に載せて確認する `近い地形でマスク表示`
- 地形境界を `なし` / `断面ポリゴン` / `ライン` から切り替え
- Depth of Field と `Miniature` 表現
- GPU Displacement / Tessellation 表示
- `Debug` タブでドローコール、三角形数、表示メッシュ、テセレーション状態を確認

## 表示モード

ビューポート左上の `表示` メニューから切り替えできます。

- **`シンプル`**: 軽量な地形・マスク確認用の表示。
- **`PBR`**: 太陽方向、光量、環境光、影、Albedo を調整できる地形プレビュー用ライティング。
- **`天球`**: Nishita 単散乱 + Hillaire 多重散乱 LUT による物理ベース大気と、レイマーチ式ボリューム雲を使う表示。詳細は [docs/sky_and_clouds/sky_and_clouds.md](docs/sky_and_clouds/sky_and_clouds.md) を参照してください。

## エクスポートと補助機能

- 評価済み地形メッシュの OBJ エクスポート
- OBJ エクスポートでは表示用の断面壁と底面を除外し、上面ポリゴンだけを書き出し
- F12 キーでアプリケーションウィンドウ全体を PNG スクリーンショットとして保存

## ドキュメント

- 変更履歴: [docs/changelog.md](docs/changelog.md)
- 計画: [docs/plan/plan.md](docs/plan/plan.md)
- 進捗: [docs/plan/progress.md](docs/plan/progress.md)
- 地形制作フロー: [docs/plan/terrain_workflow.md](docs/plan/terrain_workflow.md)
- ノードドキュメント索引: [docs/nodes/README.md](docs/nodes/README.md)
- Multi-Scale Erosion アルゴリズム解説: [docs/nodes/heightfield/multi_scale_erosion/multi_scale_erosion_algorithm_guide.md](docs/nodes/heightfield/multi_scale_erosion/multi_scale_erosion_algorithm_guide.md)
- 大気・雲システム解説: [docs/sky_and_clouds/sky_and_clouds.md](docs/sky_and_clouds/sky_and_clouds.md)
- 今後追加したいノード候補: [docs/nodes/node_candidates.md](docs/nodes/node_candidates.md)

## ビルド

vcpkg で依存ライブラリを用意し、vcpkg toolchain を指定して CMake を構成します。

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

実行:

```powershell
./build/Debug/terrain_editor.exe
```
