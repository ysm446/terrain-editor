# Terrain Editor

ハイトフィールドを中心にした、ノードベースの地形エディタの Windows デスクトッププロトタイプです。

このプロジェクトはもともと岩生成ツールとして始まりましたが、現在はハイトマップの読み込み、地形向けのプロシージャル処理、地形確認用の表示機能を中心に作り替えています。

## 現在のプロトタイプ

- C++20 / CMake アプリケーション
- Win32 + DirectX 12 レンダラ
- Dear ImGui ベースの UI
- imgui-node-editor を使ったノードグラフ
- ノード、ピン、リンク、パラメータ、評価キャッシュ、プレビュー状態を持つ内部 `NodeGraph` モデル
- `ファイル > 保存` / `ファイル > 開く` による `.terrainproj` の JSON プロジェクト保存・読み込み
- 古いサンプルデータ用に `.rockproj` の読み込みも維持
- `data/ui_themes` の JSON UI テーマを `設定 > UIテーマ` から切り替え可能
- `data/app_settings.json` に UI テーマ、3D カメラ、2D マップ表示、プレビュー表示、ライティング設定、最近使ったプロジェクトを保存

## ノード

ノードは `ハイトフィールド系` と `マスク系` の 2 カテゴリに分かれており、ノードグラフ上の右クリックメニューもこの分類で整理されています。詳細は [docs/nodes/README.md](docs/nodes/README.md) の索引を参照してください。

### ハイトフィールド系

- **`Import Heightmap`** — 画像ハイトマップを読み込みます。地形スケール (m)、Relative Vertical Scale、縦方向オフセット、シミュレーション解像度を指定可能。
- **`Shape`** — 半球やピラミッドなどのプロシージャル形状をハイトフィールドとして生成。
- **`Heightmap Blur`** — 分離可能ガウシアンによる滑化。半径・強度・反復回数を調整可能。
- **`Multi-Scale Erosion`** — Schott et al. SIGGRAPH 2024 の Stream Power + Thermal + Deposition の CPU 実装。マルチグリッドピラミッドで解像度依存性を抑え、`Heightmap` / `Flows` / `Deposits` を出力。

### マスク系

- **`Mask Noise`** — Perlin / fBM ベースのマスク発生源。GPU compute 経路と CPU 並列フォールバックを持ちます。
- **`Mask Blend`** — 2 つのマスクを `Add` / `Multiply` / `Min` / `Max` で合成。`Mask Fluvial` も上流に置けます。
- **`Mask Fluvial`** — ハイトフィールドから D8 / MFD フロー累積を計算して川筋マスクを抽出。`Log` / `Threshold` / `Linear` の 3 つの出力カーブから選べ、既定の Log カーブは GIS 標準の連続的な樹枝状ドレナージマップを出します。

まずは汎用的な浸食ワークフローを作ることを優先しています。アルプスの山と日本の山の違いのような地質・地域性のプリセットは、コアの浸食モデルの上に重ねる後工程として扱う想定です。

## ビューとプレビュー

- 1 unit = 1 m の 3D 地形ビューポート
- 選択中のハイトマップまたはマスク出力を確認する 2D マップビュー
- 2D ビューはズーム、パン、リセットに対応
- ノードや出力ピンを選択すると、その段階の結果をプレビュー表示
- 出力ピンは型ごとに色分け
  - ハイトフィールド出力は緑
  - マスクテクスチャ出力はオレンジ
- アクティブな出力ラベルをノード上で強調表示
- プレビュー評価は非同期で実行し、ノード選択やパラメータ変更時に UI が固まりにくいようにしています。
- ステータスバーに評価状態と計算時間を表示
- 評価中のノードには `計算中` / `計算待ち` バッジを表示

## 表示モード

ビューポート左上の `表示` メニューから切り替えできます。

- **`シンプル`** — 軽量な地形・マスク確認用のフラットシェーディング表示。
- **`PBR`** — 太陽方向、光量、環境光、影の強さ、シャドウマップ解像度、バイアス、Albedo を調整できる地形プレビュー用ライティング(完全な PBR マテリアルではなく、凹凸と影を読みやすくする想定)。
- **`天球`** — Nishita 単散乱 + Hillaire 多重散乱 LUT による物理ベース大気と、レイマーチ式ボリューム雲を有効化。太陽の高度を変えるだけで青空 → 黄昏 → 夕焼け → 夜が連動し、雲は太陽方向ライトマーチによる自己遮蔽と Henyey-Greenstein 位相関数でボリューム感のある陰影を出します。詳細は [docs/sky_and_clouds/sky_and_clouds.md](docs/sky_and_clouds/sky_and_clouds.md) を参照。

## エクスポートと補助機能

- 評価済み地形メッシュの OBJ エクスポート
- 最終メッシュ評価では、共有頂点、三角形インデックス、ユニークエッジ、頂点法線を持つインデックス付きトポロジを生成
- F12 キーでアプリケーションウィンドウ全体を PNG スクリーンショットとして保存

## ドキュメント

- 変更履歴: [docs/changelog.md](docs/changelog.md)
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
