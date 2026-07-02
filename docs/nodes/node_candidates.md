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
| 表土を被覆させる | `Soil` | 実装済み | `Soil` (被覆型堆積) |
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
| Crumbling | 崖下の崩落・岩屑・堆積物を追加する | Heightfield + Emission Mask | Heightfield/Mask/Unique Mask | Physics Count、Debris Amount、Debris Size、Rock Style、Gravity、Spread、Seed | 初期実装済み。`Rock` / `Sediment` との差分を見ながら発生密度、停止条件、堆積量を調整する。 |
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
| Glacial Erosion | 氷河侵食で U 字谷・カール・アレート/ホルンを作る | Heightfield + Snow/Ice Mask optional | Heightfield/Mask | Ice Source(積雪・標高)、Flow Iterations、U字化(谷底平滑・拡幅)、Overdeepening、Cirque Headward、Talus | アルプス的地形の差別化要因。Droplet/Fluvial(水=V字谷・樹枝状)では原理的に作れない形。物理完全解ではなく「氷厚場→厚み依存侵食→谷を U字へ整形」の近似で実装する想定。`Snow`(氷の供給源)/`Mask Height`(雪線)/`Multi-Scale Erosion` の thermal(フロスト破砕)と組み合わせる。 |
| Roches Moutonnées (羊背岩) | 氷流方向に対し非対称な岩盤の瘤(上流側=滑らかな磨食面 / 下流側=急な剥離面)を作る | Heightfield + Flow Direction/Mask optional | Heightfield/Mask | Flow Direction、Density/Scatter、Size、Stoss Smoothness、Lee Steepness、Plucking Roughness、Seed | 氷河侵食のメソ〜小スケールの代表的微地形。`Glacial Erosion` の氷流方向を引き継ぐと自然。単独ノードにするか `Glacial Erosion` の出力ディテールとして組み込むかは設計時に判断する。U 字谷底や圏谷の岩床に散布して氷河地形らしさを補強する。 |
| Moraine (モレーン/堆石) | 氷河が運んだ岩屑の堆積を筋状の高まりとして盛る(側・中央・末端モレーン) | Heightfield + Ice Flow Direction/Tributary Mask optional (`Glacial Erosion` 由来) | Heightfield/Mask | Moraine Type(Lateral/Medial/Terminal)、Flow Direction、Debris Amount、Ridge Width/Height、Confluence(支流)構造、Seed | 氷河侵食が「削る」のに対しモレーンは「盛る」堆積地形。岩屑は氷の流線に沿って運ばれるため、`Glacial Erosion` の氷流方向に沿って advect すると自然な縞になる。**中央モレーン**は 2 本の支流が合流するたびに内側どうしの側モレーンが 1 本に合体して筋が増える(N 本合流→おおむね N−1 本)ので、合流(支流)トポロジーを参照して生成する。`Roches Moutonnées` と同様、単独ノードにするか `Glacial Erosion` の出力ディテールに組み込むかは設計時に判断する(下記設計メモ参照)。 |

## 優先度 B: ディテール生成

| ノード候補 | 目的 | 入力 | 出力 | 最初に実装する設定 | 補足 |
| --- | --- | --- | --- | --- | --- |
| Displacement | 画像や別フィールドで高さを変位させる | Heightfield + Texture/Heightfield | Heightfield | File、Mapping、Amount、Triplanar Blend | ビューポートの GPU Displacement とは別に、データとして Heightfield へ反映するノード。 |
| Layered Displacement | 複数層の変位を重ねる | Heightfield | Heightfield | Layers、Scale、Amount、Blend | 岩肌や細部をまとめて追加する。 |
| Detail Transfer | 別ハイトフィールドから細部だけ転写する | Heightfield A/B | Heightfield | Detail Scale、Strength、Mask | 大形状は維持し、細部だけ借りる。 |
| Strata Noise | 層状の岩肌ノイズを作る | Heightfield | Heightfield/Mask | Layer Scale、Warp、Strength | 地層、崖、浸食縞向け。 |
| Cracks | ひび割れや割れ目を追加する | Heightfield + Mask optional | Heightfield/Mask | Seed、Density、Width、Depth、Pattern(亀甲/直線/節理)、Mask | 乾燥地形(泥のひび)や岩盤(節理・割れ目)向け。単独の汎用ひび割れノードにするか、`Rock` 側にクラック設定を持たせるかは要検討(下記設計メモ参照)。Mask 入力で岩・露頭にだけ適用できるようにする想定。 |
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
- 岩のクラック(割れ目)の持たせ方は未決。以下を検討事項として残す。
  - クラックを **単体ノード**(汎用 `Cracks`)として作成するか。Mask で岩・露頭に限定適用でき、
    乾燥地形の泥ひびや岩盤の節理など岩以外にも再利用できる。
  - 既存 `Rock` ノードにクラック設定(Density / Width / Depth / Pattern)を追加し、岩そのものに割れを刻むか。
    岩の見た目を 1 ノードで完結できる。
  - 両立案(単体ノードを基本にしつつ `Rock` 側は簡易プリセット呼び出しに留める)も含めて比較する。
  どれを採るかは実装時に判断する。
- 氷河系ノード(`Glacial Erosion` / `Roches Moutonnées` / `Moraine`)は川系(`River` 系)と
  同じ「侵食・水系」カテゴリに置く。水も氷も「流れ場に沿って削る・運ぶ・盛る」点で原理が共通し、
  流れ方向フィールドなどの基盤を共有できるため。ただしノードとしての役割は分ける:
  `Glacial Erosion` = 削る(U字谷・カール)、`Moraine` = 盛る(堆積)、
  `Roches Moutonnées` = 微地形ディテール。
- そのため `Moraine` は `Glacial Erosion` に統合せず、`Roches Moutonnées` と同様に独立ノードとし、
  `Glacial Erosion` が出力する氷流方向と支流(合流)構造を入力に取る構成を基本とする。削りと盛りを
  別レイヤーで制御・キャッシュでき、中央モレーンの「合流ごとに筋が増える」表現も合流トポロジーを
  参照して作れる。理想的には `Glacial Erosion` が Heightfield/Mask に加えて Flow Direction と
  Tributary ID(支流識別)フィールドを出力できると、`Moraine` / `Roches Moutonnées` の双方が
  同じ流れ場を共有できる。
- 堆積系 3 ノードの役割分担は次のとおり分ける (`Soil` は 0.24.0 で実装済み)。
  - `Sediment` = **谷埋め型**。重力輸送で溝・谷底に厚く溜まり尾根が剥き出しになる(樹枝状)。「崩れた岩屑が溜まる」表現。
  - `Soil` = **被覆型**。緩斜面・上面に一様にかぶさり、急斜面・崖では剥げて岩盤が露出する表土マントル。
  - `Snow` = **雪特化**。アルゴリズムは被覆型だが、雪固有の拡張(風ドリフト・雪線・融雪)へ進化させる。
  かつては `Soil` の代わりに `Snow` を土の堆積に使っていたが、今後の拡張が
  Snow = 風・雪線・融雪、Soil = 傾斜/曲率依存の厚み・erosion deposits 連携・植生マスクと
  確実に分岐するため、兼用ノードにはせず分離した。素材ごとの既定値(安息角、表面平滑化)と
  マスク意味論(雪 = coverage で Colorize の白、土 = 厚み/coverage 切り替えで土色・草)も
  ノード名で意図が残るよう別ノードとして守る。実装は `Snow` の settling コア
  (注入 → 安息角超過分の移動 → 表面平滑化 → マスク化)を共有関数化
  (`GranularSettle`、GPU は `snow_compute.hlsl` を共有)して両者で使っている。
  `Soil` の後続拡張候補: 曲率依存の厚み、`Multi-Scale Erosion` deposits 連携、植生マスク接続。
- KTT は 1024 x 1024 m を標準想定しているため、Terrain Editor の初期 Scale 1024 m と整合する。
- 大きな地形では 2048、4096、8192 解像度を想定し、プレビュー解像度と最終解像度を分ける。
- 山地の地域差(例: 日本の山 vs スイスアルプス)は単一の侵食ノードでは出し切れない。
  Droplet / Fluvial(水力侵食)が担うのは V 字谷・樹枝状ドレナージで「湿潤・河川」レジーム
  (日本寄り)。U 字谷・カール・アレート/ホルンといったアルプス的な形は氷河侵食固有で、
  `Glacial Erosion`(未実装、上記候補)が最大の差別化要因になる。差別化の効きは
  氷河侵食 > thermal/talus の強弱 > ベース形状(褶曲 vs 火山錐)の順。典型ワークフローは
  日本=「Shape → Droplet/Fluvial 強め → Thermal 弱め」、アルプス=「Shape → Glacial →
  Thermal/Talus 強め → Droplet 軽く仕上げ」。
