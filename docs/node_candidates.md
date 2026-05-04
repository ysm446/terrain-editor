# ノード候補一覧

`ref/ktt` の Kruger Terrain Tools を参考にした、Terrain Editor 向けのノード候補です。
現在の方針はハイトフィールド中心、1 unit = 1 m、1024 m 四方を標準スケールとして考えます。

## 優先度 A: 最初に欲しい基本フロー

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Import Heightmap | 画像から高さを読み込む | なし | Heightfield | File、Scale、Relative Vertical、Offset |
| Adjust | 高さやマスク値を補正する | Heightfield | Heightfield | Field、Gain、Bias、Clamp、Invert |
| Combine | 2 つの地形やマスクを合成する | Heightfield A/B | Heightfield | Add、Subtract、Multiply、Max、Min、Blend |
| Transform | 地形フィールドを移動・回転・スケールする | Heightfield | Heightfield | Translate、Rotate Y、Scale |
| Output | 最終出力ノード | Heightfield | Mesh/Export | Mesh Resolution、Export Path |
| Mesher | ハイトフィールドをメッシュ化する | Heightfield | Mesh | Resolution、Normal、UV、Bounds |

## 優先度 A: 形状生成

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Mountain | ベース山地を生成する | Heightfield optional | Heightfield | Seed、Size、Height、Position、Stretch、Noise Strength |
| Gradient | 傾斜や方向性のある高さを作る | Heightfield optional | Heightfield/Mask | Direction、Amplitude、Falloff |
| Gabor | 方向性のある細かい筋状ノイズを作る | Heightfield optional | Heightfield/Mask | Seed、Scale、Direction、Anisotropy、Amplitude |
| Crater | クレーター形状を追加する | Heightfield optional | Heightfield | Position、Radius、Depth、Rim Height、Falloff |
| Stamp | ブラシや外部ハイトマップを押し付ける | Heightfield + Stamp | Heightfield | File、Position、Rotation、Scale、Blend |

## 優先度 B: 地形加工

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Terrace | 段丘状に高さを量子化・整形する | Heightfield | Heightfield | Step Count、Strength、Smoothness |
| Recurve | 高さカーブを使って地形の輪郭を再形成する | Heightfield | Heightfield | Ramp、Blend、Mask |
| Smooth Talus | 不安定な斜面をならす | Heightfield | Heightfield/Mask | Talus Angle、Iterations、Strength |
| Thermal Erosion | 熱侵食、崖下の堆積を作る | Heightfield | Heightfield | Iterations、Talus Angle、Sediment Amount |
| Scree | 崖下の崩落・堆積物を追加する | Heightfield | Heightfield/Mask | Slope Range、Amount、Noise、Mask |
| Flatten By Proximity | カーブや点の近くを平坦化する | Heightfield + Curve/Points | Heightfield | Radius、Falloff、Target Height |
| Flatten Borders | 外周を平坦化する | Heightfield | Heightfield | Border Width、Falloff、Target Height |

## 優先度 B: 侵食・水系

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Flowlines | 斜面に沿った流線やフローマップを生成する | Heightfield | Mask/Vector Field | Particle Count、Step Length、Seed |
| Lakes | 窪地の水面や湖マスクを作る | Heightfield | Heightfield/Mask | Water Level、Fill Depressions、Mask Output |
| River Network | 河川ネットワークを生成する | Heightfield/Water Sources | Curve/Mask | Source Count、Drainage Threshold、Width |
| Fluvial Erosion | 水侵食でチャンネルを作る | Heightfield | Heightfield/Mask | Iterations、Rain Amount、Erosion、Deposition |
| Smooth Fluvial Erosion | 制御しやすい水侵食と堆積 | Heightfield | Heightfield/Mask | Iterations、Sediment Capacity、Bedrock、Uplift |
| Meandering Rivers | 河川カーブを蛇行させる | Curve + Heightfield | Curve/Heightfield | Meander Strength、Iterations、Width |
| Hydrologic Conditioning | 水が流れるように地形を補正する | Heightfield | Heightfield | Fill Sinks、Breach、Flow Direction |

## 優先度 B: ディテール生成

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Displacement | 画像や別フィールドで法線方向にディスプレイスする | Heightfield + Texture/Heightfield | Heightfield | File、Mapping、Amount、Triplanar Blend |
| Layered Displacement | 複数層の変位を重ねる | Heightfield | Heightfield | Layers、Scale、Amount、Blend |
| Detail Transfer | 別ハイトフィールドから細部だけ転写する | Heightfield A/B | Heightfield | Detail Scale、Strength、Mask |
| Strata Noise | 層状の岩肌ノイズを作る | Heightfield | Heightfield/Mask | Layer Scale、Warp、Strength |
| Cracks | ひび割れや割れ目を追加する | Heightfield | Heightfield/Mask | Seed、Density、Width、Depth |
| Crevasses | 氷河や雪面向けの深い割れ目を作る | Heightfield | Heightfield/Mask | Direction、Depth、Spacing、Noise |
| Cliff Synthesizer | 崖や岩場の詳細を成長させる | Heightfield | Mesh/Heightfield | Region Mask、Iterations、Height、Roughness |

## 優先度 C: 砂・雪・特殊地形

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Dunes | 風向きに沿った砂丘を生成する | Heightfield optional | Heightfield | Wind Direction、Iterations、Sand Amount |
| Dunes V2 | より制御しやすい砂丘生成 | Heightfield optional | Heightfield | Wind Direction、Spacing、Migration、Seed |
| Snow Base | 雪の堆積ベースを作る | Heightfield | Heightfield/Mask | Snow Amount、Slope Limit、Wind |
| Shortest Path | 地形上の最短経路を計算する | Heightfield + Points | Curve/Mask | Start、End、Cost Field |

## 優先度 C: カラー・マスク・テクスチャ

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Mask By Color | 色からマスクを作る | Color Field | Mask | Target Color、Tolerance、Falloff |
| Color By Gradient | 高さ・傾斜から色を割り当てる | Heightfield | Color Field | Gradient、Height Range、Slope Range |
| Color Curves | 色フィールドをカーブ補正する | Color Field | Color Field | RGB Curves、HSV Adjust |
| Color Distort | 色フィールドをノイズで歪ませる | Color Field | Color Field | Noise Scale、Amount、Seed |
| Clear Color | 色フィールドを初期化する | Heightfield/Color Field | Color Field | Color、Alpha |
| Tint | 複数の色レイヤーをマスクで重ねる | Color Field + Masks | Color Field | Layers、Mask Type、Blend |
| Apply Texture | 画像テクスチャを地形に投影する | Heightfield + Texture | Color Field/Mask | File、Tiled/Triplanar/UV、Scale、Rotation |
| Texture Detail Transfer | テクスチャの細部を転写する | Color Field A/B | Color Field | Detail Scale、Strength |
| Bake Normals | ハイトフィールドから法線を焼く | Heightfield | Normal Map | Strength、Resolution |

## 優先度 C: タイル・キャッシュ・補助

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 |
| --- | --- | --- | --- | --- |
| Tiled Heightfield | 大きな地形をタイルとして扱う | Heightfield/Tiles | Tiled Heightfield | Tile Size、Tile Count、Overlap |
| Tile Split | 1 枚の地形をタイルへ分割する | Heightfield | Tiles | Tile Size、Overlap、Naming |
| Cache Tiles | タイル単位で結果をキャッシュする | Tiled Heightfield | Tiled Heightfield | Cache Directory、Dirty Tiles Only |
| Cacher | ノード結果をファイルに保存して再利用する | Any | Any | Cache Path、Bake、Reload |
| Make Seamless | タイル端をシームレスにする | Heightfield | Heightfield | Border Width、Blend |
| Scatter | 地形条件からポイントを散布する | Heightfield/Mask | Points | Density、Slope Range、Height Range、Seed |
| Shadowmap | 簡易シャドウや日照マスクを作る | Heightfield | Mask | Sun Direction、Samples、Softness |
| Shade Viewport | ビューポート確認用の陰影を作る | Heightfield | Preview Color | Light Direction、Color Mode |
| Path Traced Lightmap | 高品質ライトマップを作る | Heightfield/Mesh | Texture | Samples、Sun/Sky、Resolution |

## 実装順の提案

1. `Import Heightmap`、`Adjust`、`Combine`、`Output/Mesher`
2. `Mountain`、`Terrace`、`Thermal Erosion`
3. `Flowlines`、`Lakes`、`Fluvial Erosion`
4. `Displacement`、`Stamp`、`Strata Noise`
5. `Color By Gradient`、`Apply Texture`、`Tint`
6. `Tiled Heightfield`、`Tile Split`、`Cache Tiles`

## 設計メモ

- 基本データは `Heightfield`、`Mask`、`Color Field`、`Curve`、`Points`、`Mesh` に分ける。
- まずは CPU 実装で確定し、重い侵食・タイル処理だけ後から GPU または並列化を検討する。
- 各ノードは `Field` と `Mask` を共通概念として持てるようにする。
- KTT は 1024 x 1024 m を標準想定しているため、Terrain Editor の初期 Scale 1024 m と整合する。
- 大きな地形では 2048、4096、8192 解像度を想定し、プレビュー解像度と最終解像度を分ける。
