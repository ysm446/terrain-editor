# ノード候補一覧

`ref/ktt` の Kruger Terrain Tools を参考にした、Terrain Editor 向けのノード候補です。
現在の方針はハイトフィールド中心、1 unit = 1 m、1024 m 四方を標準スケールとして考えます。

この一覧は「これから作る候補」と「すでに近い役割を持つ実装済みノード」を分けて管理します。
実装済みノードと重なる候補は、実際のノード名へ差し替えるか、未実装の差分だけを残します。

## 実装済み / 近い役割を持つノード

| 役割 | 現在のノード | 状況 | 置き換えた候補 |
| --- | --- | --- | --- |
| 画像から高さを読み込む | `Import Heightmap` | 実装済み | `Import Heightmap` |
| 基本形状を作る | `Shape` | 実装済み | `Mountain` / `Gradient` の一部 |
| 高さをぼかす | `Heightmap Blur` | 実装済み | `Smooth` 系の一部 |
| 水・熱侵食系のベース | `Multi-Scale Erosion` | 実装済み | `Fluvial Erosion` / `Thermal Erosion` の一部 |
| 岩・転石を散布する | `Rock` | 実装済み | `Scatter` / `Cliff Synthesizer` の一部 |
| 堆積物を追加する | `Sediment` | 実装済み | `Scree` / `Deposition` の一部 |
| 雪を積もらせる | `Snow` | 実装済み | `Snow Base` |
| ノイズマスク | `Mask Noise` | 実装済み | 汎用 `Noise Mask` |
| マスク合成 | `Mask Blend` | 実装済み | `Combine` の Mask 版 |
| マスク値補正 | `Mask Levels` | 実装済み | `Adjust` の Mask 版 / `Mask Invert` の一部 |
| 標高マスク | `Mask Height` | 実装済み | `Mask By Height` / `Color By Gradient` の高さ参照部 |
| 傾斜マスク | `Mask Slope` | 実装済み | `Mask By Slope` / `Color By Gradient` の傾斜参照部 |
| 曲率マスク | `Mask Curvature` | 実装済み | 尾根・谷マスク |
| 流域/河川筋マスク | `Mask Fluvial` | 実装済み | `Flowlines` / `River Network` のマスク版 |
| 色付け | `Colorize` | 実装済み、改善余地あり | `Color By Gradient` / `Tint` の一部 |

## 優先度 A: 次に欲しい基本ノード

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Heightfield Levels | 高さを Gain / Bias / Clamp / Invert で補正する | Heightfield | Heightfield | Gain、Bias、Min/Max Clamp、Invert | `Mask Levels` の Heightfield 版。旧 `Adjust` 候補を差し替え。 |
| Heightfield Blend | 2 つの地形を合成する | Heightfield A/B + Mask optional | Heightfield | Add、Subtract、Multiply、Max、Min、Blend、Opacity | `Mask Blend` の Heightfield 版。旧 `Combine` 候補を差し替え。 |
| Transform | 地形フィールドを移動・回転・スケールする | Heightfield | Heightfield | Translate、Rotate Y、Scale、Wrap/Clamp | ノードグラフ上で形状配置を調整するために必要。 |
| Output | 最終出力を明示する | Heightfield / Color Texture | Mesh/Export | Export Target、Mesh Resolution、Include Color | 現状はアプリ側の出力機能として存在。ノード化は未実装。 |
| Color Ramp | マスク値から色を割り当てる | Mask / Color Texture optional | Color Texture | Gradient Keys、Blend Mode、Opacity | `Colorize` の改善候補。高さ/傾斜参照は `Mask Height` / `Mask Slope` に分離済み。 |

## 優先度 A: マスク加工

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Mask Invert | 入力マスクを反転する | Mask | Mask | Invert、Floor/Ceiling optional | `Mask Levels` の `Invert` で代用可能。単独ノードが必要かは保留。 |
| Mask Blur | マスク境界をぼかす | Mask | Mask | Radius、Iterations、Edge Mode | Heightfield Blur ではなく Mask 用。 |
| Mask Erode/Dilate | マスク領域を縮小・拡大する | Mask | Mask | Radius、Mode、Iterations | 雪線や水域の縁調整に使う。 |
| Mask Remap Curve | カーブでマスクを再マップする | Mask | Mask | Curve、Clamp、Invert | `Mask Levels` より細かいカーブ調整。 |
| Mask Distance | マスク境界から距離場を作る | Mask | Mask | Inside/Outside、Max Distance、Normalize | 岸辺、雪縁、道路脇などの幅制御に使う。 |

## 優先度 B: 形状生成

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Mountain | ベース山地を生成する | Heightfield optional | Heightfield | Seed、Size、Height、Position、Stretch、Noise Strength | `Shape` より山地専用の高レベルノード。 |
| Ridge / Valley | 尾根や谷の大きな構造を作る | Heightfield optional | Heightfield/Mask | Direction、Length、Height/Depth、Falloff | `Mask Curvature` は検出用。これは生成用。 |
| Gabor Noise | 方向性のある細かい筋状ノイズを作る | Heightfield optional | Heightfield/Mask | Seed、Scale、Direction、Anisotropy、Amplitude | 岩層や砂丘の方向性ディテールに使う。 |
| Crater | クレーター形状を追加する | Heightfield optional | Heightfield | Position、Radius、Depth、Rim Height、Falloff | 惑星/火山地形向け。 |
| Stamp | ブラシや外部ハイトマップを押し付ける | Heightfield + Stamp | Heightfield | File、Position、Rotation、Scale、Blend | 外部地形パーツの配置用。 |

## 優先度 B: 地形加工

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Terrace | 段丘状に高さを量子化・整形する | Heightfield | Heightfield | Step Count、Strength、Smoothness、Mask | 高さの段を作る。 |
| Height Curve | 高さカーブで地形の輪郭を再形成する | Heightfield | Heightfield | Curve、Blend、Mask | 旧 `Recurve` 候補を分かりやすく改名。 |
| Smooth Talus | 不安定な斜面をならす | Heightfield | Heightfield/Mask | Talus Angle、Iterations、Strength | `Multi-Scale Erosion` より単純で制御しやすい斜面緩和。 |
| Crumbling | 崖下の崩落・岩屑・堆積物を追加する | Heightfield + Emission Mask | Heightfield/Mask/Unique Mask | Physics Count、Debris Amount、Debris Size、Rock Style、Gravity、Seed | 初期実装済み。`Rock` / `Sediment` との差分を見ながら発生密度、停止条件、堆積量を調整する。 |
| Flatten By Proximity | カーブや点の近くを平坦化する | Heightfield + Curve/Points | Heightfield | Radius、Falloff、Target Height | 道路、建物、川岸などの整地に使う。 |
| Flatten Borders | 外周を平坦化する | Heightfield | Heightfield | Border Width、Falloff、Target Height | タイル端や展示用地形の外周処理。 |

## 優先度 B: 侵食・水系

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| River / Lake | 川筋、湖、湿地のような水域を作る | Heightfield + Mask/Guide optional | Heightfield/Mask/Water Surface | Water Level、River Width、Erosion、Bank Softness、Water Material | 予定タスク。地形生成とビューポート水表現を分けて設計する。 |
| Lakes | 窪地の水面や湖マスクを作る | Heightfield | Mask/Water Surface | Water Level、Fill Depressions、Depth Mask | `River / Lake` のうち湖だけを単独化する案。 |
| River Network | 河川ネットワークを生成する | Heightfield/Water Sources | Curve/Mask | Source Count、Drainage Threshold、Width | `Mask Fluvial` はマスク生成済み。Curve/水路生成は未実装。 |
| Hydrologic Conditioning | 水が流れるように地形を補正する | Heightfield | Heightfield | Fill Sinks、Breach、Flow Direction | 河川生成前の前処理。 |
| Meandering Rivers | 河川カーブを蛇行させる | Curve + Heightfield | Curve/Heightfield | Meander Strength、Iterations、Width | River Network 後の形状改善。 |

## 優先度 B: ディテール生成

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Displacement | 画像や別フィールドで高さを変位させる | Heightfield + Texture/Heightfield | Heightfield | File、Mapping、Amount、Triplanar Blend | ビューポートの GPU Displacement とは別に、データとして Heightfield へ反映するノード。 |
| Layered Displacement | 複数層の変位を重ねる | Heightfield | Heightfield | Layers、Scale、Amount、Blend | 岩肌や細部をまとめて追加する。 |
| Detail Transfer | 別ハイトフィールドから細部だけ転写する | Heightfield A/B | Heightfield | Detail Scale、Strength、Mask | 大形状は維持し、細部だけ借りる。 |
| Strata Noise | 層状の岩肌ノイズを作る | Heightfield | Heightfield/Mask | Layer Scale、Warp、Strength | 地層、崖、浸食縞向け。 |
| Cracks | ひび割れや割れ目を追加する | Heightfield | Heightfield/Mask | Seed、Density、Width、Depth | 乾燥地形や岩盤向け。 |
| Crevasses | 氷河や雪面向けの深い割れ目を作る | Heightfield | Heightfield/Mask | Direction、Depth、Spacing、Noise | Snow と組み合わせる。 |

## 優先度 C: 砂・雪・特殊地形

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Dunes | 風向きに沿った砂丘を生成する | Heightfield optional | Heightfield | Wind Direction、Iterations、Sand Amount | 砂丘専用ノード。 |
| Dunes V2 | より制御しやすい砂丘生成 | Heightfield optional | Heightfield | Wind Direction、Spacing、Migration、Seed | Dunes の高機能版候補。 |
| Snow Refine | 既存 `Snow` の品質改善 | Heightfield + Snow Mask optional | Heightfield/Mask | Surface Smooth Strength、Thickness Blur、Wind Drift | `Snow Base` は `Snow` に差し替え済み。今後は改善項目として扱う。 |
| Shortest Path | 地形上の最短経路を計算する | Heightfield + Points | Curve/Mask | Start、End、Cost Field | 道路、川、稜線ガイドに使える。 |

## 優先度 C: カラー・テクスチャ

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Color Ramp | マスク値から色を割り当てる | Mask / Color Texture optional | Color Texture | Gradient、Blend、Opacity | `Colorize` の発展候補。 |
| Color Curves | 色フィールドをカーブ補正する | Color Texture | Color Texture | RGB Curves、HSV Adjust | 色の後処理。 |
| Color Distort | 色フィールドをノイズで歪ませる | Color Texture | Color Texture | Noise Scale、Amount、Seed | テクスチャの自然な崩し。 |
| Clear Color | 色フィールドを初期化する | Heightfield/Color Texture | Color Texture | Color、Alpha | 色作業の起点。 |
| Apply Texture | 画像テクスチャを地形に投影する | Heightfield + Texture | Color Texture/Mask | File、Tiled/Triplanar/UV、Scale、Rotation | 外部素材を貼る。 |
| Texture Detail Transfer | テクスチャの細部を転写する | Color Texture A/B | Color Texture | Detail Scale、Strength | 色/粗さのディテール転写。 |
| Bake Normals | ハイトフィールドから法線を焼く | Heightfield | Normal Map | Strength、Resolution | 外部利用向け。 |

## 優先度 C: タイル・キャッシュ・補助

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Tiled Heightfield | 大きな地形をタイルとして扱う | Heightfield/Tiles | Tiled Heightfield | Tile Size、Tile Count、Overlap | タイル LOD 可視化は削除済み。必要になったらデータ設計から再検討する。 |
| Tile Split | 1 枚の地形をタイルへ分割する | Heightfield | Tiles | Tile Size、Overlap、Naming | エクスポートや大規模地形向け。 |
| Cache Tiles | タイル単位で結果をキャッシュする | Tiled Heightfield | Tiled Heightfield | Cache Directory、Dirty Tiles Only | 大規模地形の再計算抑制。 |
| Cacher | ノード結果をファイルに保存して再利用する | Any | Any | Cache Path、Bake、Reload | 重いノード用。 |
| Make Seamless | タイル端をシームレスにする | Heightfield | Heightfield | Border Width、Blend | タイル境界処理。 |
| Scatter Points | 地形条件からポイントを散布する | Heightfield/Mask | Points | Density、Slope Range、Height Range、Seed | `Rock` は散布先が岩に固定。これは汎用 Points 出力。 |
| Shadow Mask | 簡易シャドウや日照マスクを作る | Heightfield | Mask | Sun Direction、Samples、Softness | 表示用シャドウとは別に、生成制御用のマスクを作る。 |
| Path Traced Lightmap | 高品質ライトマップを作る | Heightfield/Mesh | Texture | Samples、Sun/Sky、Resolution | 最終見た目用。 |

## 実装順の提案

1. `Heightfield Levels`、`Heightfield Blend`、`Transform`
2. `Color Ramp`、`Mask Blur`、`Mask Erode/Dilate`
3. `Crumbling` の調整
4. `River / Lake`、`Lakes`、`River Network`
5. `Terrace`、`Height Curve`、`Smooth Talus`
6. `Displacement`、`Stamp`、`Strata Noise`
7. `Tiled Heightfield`、`Tile Split`、`Cache Tiles`

## 設計メモ

- 基本データは `Heightfield`、`Mask`、`Color Texture`、`Curve`、`Points`、`Mesh` に分ける。
- まずは CPU 実装で確定し、重い侵食・タイル処理だけ後から GPU または並列化を検討する。
- `Mask Height`、`Mask Slope`、`Mask Curvature`、`Mask Fluvial` のような条件抽出ノードを先に作り、生成ノードの制御に使う。
- `Colorize` は既にあるが、今後は `Mask` / `Gradient Mask` / `Color Ramp` の関係を整理して、色作りをノードグラフ内で完結しやすくする。
- KTT は 1024 x 1024 m を標準想定しているため、Terrain Editor の初期 Scale 1024 m と整合する。
- 大きな地形では 2048、4096、8192 解像度を想定し、プレビュー解像度と最終解像度を分ける。
