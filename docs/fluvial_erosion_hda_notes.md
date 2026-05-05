# KTT Fluvial Erosion 調査メモ

`ref/ktt/Kruger.KTT_Fluvial_Erosion.1.0.hda` を読んだ内容を、Terrain Editor へ `Fluvial Erosion` ノードを実装するための参考として整理します。

## 概要

KTT の `Fluvial Erosion` は、水による侵食を扱うハイトフィールドノードです。
HDA 内の handbook では、地形に大きな水路や細かいチャンネルを作ることを主目的として説明されています。

特徴は、一般的な水量グリッド主体の侵食よりも、以下の要素を強く使っている点です。

- 高さ勾配から流れ方向の力場を作る
- 粒子を地表に散布して、力場に沿って移動させる
- 移動中の粒子が高さを削り、別地点へ堆積させる
- `Channeling` によって水路状の削り込みを強調する
- 解像度差を扱うためにマルチグリッド処理を使う
- 大きい地形ではタイル分割して処理する

HDA の内部ネットワークは gzip 圧縮された `Contents.gz` として格納されていますが、展開すると Houdini ノード構成と OpenCL カーネルを確認できます。

## 入出力

| 種類 | 内容 |
| --- | --- |
| Input 1 | Heightfield Input |
| Input 2 | Flow Lines Input |
| Output 1 | Heightfield Output |

Terrain Editor では、まず Input 1 の `Heightfield` を主入力として扱います。
ただし実装方針は「簡易的な見た目再現」ではなく、KTT の粒子輸送、補助フィールド、マルチスケール処理にできるだけ近づけることを目標にします。
Flow Lines 入力は初期段階では未接続でも、将来の River / Flowline 系ノードと接続できる前提で設計します。

## HDA の主なパラメータ

### Basic Simulation Properties

| KTT パラメータ | 初期値 | 範囲 | 意味 |
| --- | ---: | ---: | --- |
| Feature Size | 8 | 0-32 | 侵食で扱う特徴サイズ |
| Geological Age | 20 | 0-20 | 地形に追加する時間的な古さ |
| Simulation Iterations | 25 | 0-100 | シミュレーション反復数 |
| Channel Length | 128 | 0-512 | 粒子が流れる距離、または水路の伸び |

### Sedimentation Properties

| KTT パラメータ | 初期値 | 範囲 | 意味 |
| --- | ---: | ---: | --- |
| Erosion Strength | 1 | 0-1 | 侵食処理全体の強さ |
| Channeling | 0.25 | 0-1 | 水路状の削り込みをどれだけ強めるか |

### Sediment Transport

| KTT パラメータ | 初期値 | 範囲 | 意味 |
| --- | ---: | ---: | --- |
| Friction | 0.1 | 0-1 | 粒子速度の減衰 |
| Wear Angle | 15 | 0-90 | 侵食を開始する最小斜面角 |
| Deposit Angle | 0 | 0-90 | これ未満の斜面では侵食を止める角度 |
| Max Erosion Angle | 30 | 0-90 | 侵食を有効にする最大斜面角 |

### Sediment Shaping

| KTT パラメータ | 初期値 | 範囲 | 意味 |
| --- | ---: | ---: | --- |
| Erosion Granularity | 10 | 0-100 | 侵食粒子の密度。高いほど粒子が疎になり柔らかい形になる |
| Flow Volume | 0 | 0-1 | sediment flow が粒子を引き寄せる/反発する度合い |
| Small Channel Influence | 0 | 0-1 | 小さい水路の影響度 |
| Sediment Velocity | 1 | 0-2 | 粒子の移動速度 |

### Directionality

| KTT パラメータ | 初期値 | 範囲 | 意味 |
| --- | ---: | ---: | --- |
| Force Vector | 0,0,0 | 0-1 | 追加で与える外力方向 |
| Force Strength | 1 | 0-1 | 外力の強さ |
| Shear X | 0 | -0.1-0.1 | 流れ方向の横ずらし |
| Shear Y | 0 | -0.1-0.1 | 流れ方向の縦ずらし |

### Mask

| KTT パラメータ | 初期値 | 意味 |
| --- | ---: | --- |
| Use Erosion Mask | 0 | 侵食を適用する範囲をマスクで制限 |
| Erosion Mask | mask | 侵食マスクに使うフィールド |
| Invert Erosion Mask | 0 | 侵食マスクを反転 |
| Use Hardness Mask | 0 | 削れにくさをマスクで制御 |
| Erosion Hardness | mask | 硬さマスクに使うフィールド |
| Invert Hardness Mask | 0 | 硬さマスクを反転 |

### Performance

| KTT パラメータ | 初期値 | 範囲 | 意味 |
| --- | ---: | ---: | --- |
| Use Tiling | 1 | 0-1 | 大きい地形をタイル分割して処理 |
| Tile Resolution | 4096 | 0-8192 | タイル化を開始する解像度 |
| Tile Padding | 256 | 0-1024 | タイル境界の余白 |
| Use Multigrid Acceleration | 1 | 0-1 | 複数解像度で処理して大きな特徴を作る |

## 出力フィールド

KTT は高さだけでなく、侵食結果の補助フィールドを出力できます。

| フィールド | 意味 |
| --- | --- |
| deposits | 堆積量 |
| age | 侵食による年齢/摩耗表現 |
| flows | 流れの強さや流線情報 |

Terrain Editor でも、KTT に近い調整を行うには補助フィールドが必要です。
そのため Heightfield の更新だけでなく、少なくとも `deposits` と `flows` はノード出力として扱います。
`age` や `wear/erosion` は、見た目の調整とデバッグに必要になった段階で追加します。

## 内部ネットワークの構成

展開した内部ネットワークには、次のような主要ノードが含まれています。

| 内部ノード | 種類 | 役割 |
| --- | --- | --- |
| Compute_Padding | attribwrangle | マルチグリッド処理に必要なパディング計算 |
| Update_Resolution | attribvop | 処理解像度の更新 |
| heightfield_resample1/2 | heightfield_resample | マルチグリッド用のリサンプル |
| heightfield_tilesplit1 | heightfield_tilesplit | タイル分割 |
| heightfield_tilesplice1 | volumesplice | タイル結果の結合 |
| Update_Forces | opencl | 高さ勾配から流れ方向を更新 |
| Transport_Particles | opencl | 粒子を動かして侵食・堆積を行う |
| Smooth_Flows | volumevop | flow フィールドの平滑化 |
| Add_Detail_Pass | volumevop | 細部を戻す/追加する処理 |
| Set_Export_fields | subnet | deposits、age、flows などの出力整形 |

主な処理順は次のように見えます。

1. 入力 Heightfield を受け取る
2. 必要に応じて硬さフィールドや age フィールドを作る
3. Feature Size と解像度からマルチグリッド段数を計算する
4. 地形をタイル分割する
5. 各タイルで反復処理を行う
6. `Update_Forces` で `fx/fy` を更新する
7. `Transport_Particles` で粒子を流し、高さ、wear、deposit を更新する
8. タイルを結合する
9. 元解像度へ戻して補助フィールドを整理する

## OpenCL カーネルの要点

### Update_Forces

`Update_Forces` は高さの近傍サンプルから勾配を計算し、流れの力場を `fx/fy` に書き込みます。

要点:

- 3 x 3 近傍を使って高さ差を取る
- 勾配を `grad_x`、`grad_y` として計算する
- `fx = -grad_x`、`fy = -grad_y`
- 外力 `Force Vector` を加算する
- `mask` をリセットする
- 境界の高さを隣接セルに合わせて補正する

### Transport_Particles

`Transport_Particles` が侵食の中心です。
地表に散布された粒子が `fx/fy` の力場に沿って移動しながら、高さを削る/堆積させます。

要点:

- 各セル付近に乱数で粒子を散布する
- 勾配、曲率、Wear Angle から侵食するか判定する
- `Erosion Granularity` で粒子密度を調整する
- `Erode Mask` と `Hardness` を参照する
- `Sediment Velocity` と `Friction` で粒子速度を更新する
- 進行方向の前後サンプルから高さ差を見て、現在セルの高さを更新する
- `Channeling` で水路状の削り込みを強調する
- `wear` と `deposit` に削り量/堆積量を蓄積する
- 高さは前後サンプルの範囲内にクランプして破綻を抑える

KTT の核はこの粒子輸送処理です。
Terrain Editor でも、単なる斜面方向の削り込みではなく、力場、粒子輸送、堆積、flow/deposit 補助フィールド、マルチスケール処理を組み合わせて、かなり KTT に似せた構成を目指します。

## Terrain Editor 向け実装方針

`Fluvial Erosion` ノードは、KTT の完全なバイナリ互換や Houdini 内部ノードの完全移植ではなくても、挙動と調整感はできるだけ KTT に近づけます。
特に、KTT らしさに効く次の要素は初期から中核として扱います。

- `Feature Size`、`Geological Age`、`Simulation Iterations`、`Channel Length` を基本パラメータとして持つ
- `Erosion Strength` と `Channeling` で水路状の削り込みを制御する
- `Friction`、`Wear Angle`、`Deposit Angle`、`Max Erosion Angle`、`Sediment Velocity` で粒子輸送を制御する
- `Erosion Granularity`、`Flow Volume`、`Small Channel Influence` で細いチャンネルの密度と影響を制御する
- `deposits` と `flows` を補助出力として確認できるようにする
- D8 の単一下流だけでなく、複数下流セルへ分配する multi-flow 風の flow accumulation を使う
- 大きな谷筋と細かいリルを分けて扱うマルチスケール処理を持つ

### 入出力

| 種類 | 内容 |
| --- | --- |
| Input | Heightfield |
| Output | Heightfield |
| Output | Deposits |
| Output | Flows |

将来的には、KTT の Input 2 に相当する Flow Lines 入力、Erosion Mask、Hardness Mask、`age` / `erosion` 出力も追加候補にします。

### KTT 寄せのパラメータ

| パラメータ | 初期値 | 範囲 |
| --- | ---: | ---: |
| Feature Size | 4 | 1-64 |
| Geological Age | 20 | 0-100 |
| Iterations | 25 | 0-200 |
| Channel Length | 128 | 1-1024 |
| Erosion Strength | 0.65 | 0-1 |
| Channeling | 0.2 | 0-1 |
| Friction | 0.1 | 0-1 |
| Wear Angle | 15 | 0-90 |
| Deposit Angle | 0 | 0-90 |
| Max Erosion Angle | 30 | 0-90 |
| Erosion Granularity | 10 | 1-100 |
| Flow Volume | 0 | 0-2 |
| Small Channel Influence | 0 | 0-1 |
| Sediment Velocity | 1 | 0-2 |

Terrain Editor 側の内部係数やマルチスケール強度は、KTT の表示パラメータから自動的に決めます。
KTT の UI に存在しない `Sediment Capacity`、`Deposition Rate`、`Large/Medium/Detail Scale`、`Level Strength`、`Seed` は設定項目として保存せず、内部係数として扱います。

### CPU 実装方針

1. 入力 heightfield を作業バッファへコピーする
2. 高さと `wear` フィードバックから `fx/fy` の力場を計算する
3. 下がっている複数近傍へ flow を分配し、multi-flow 風の drainage area を作る
4. Feature Size と内部スケール係数から、粗い谷筋、中規模支流、細かいリルの複数スケールに分ける。ただし粗いスケールは地形を均しすぎないよう弱めに扱う
5. `VoxelScale` と `Geological Age` に応じて、KTT の反復数スケーリングに近い形でスケール別反復数を決める
6. iteration ごとにKTTの `Transport_Particles` に近い式で粒子開始点を乱数散布する
7. 粒子ごとに `Channel_Length / (dx / Detail_Scale)` に近い距離だけ移動する
8. flow、斜面角、Wear Angle、Deposit Angle、Max Erosion Angle から侵食/堆積を判定する
9. `Channeling` と `Flow Volume` で流路状の削り込みと輸送力を調整する
10. `Small Channel Influence` と micro/detail pass で浅い細リルを追加する
11. 粒子が削った土砂を `sediment` として運び、谷底や緩斜面へ `deposits` として残す
12. 粒子通過量を `flows` として蓄積する
13. 同一反復内の重複侵食をマスクで抑え、削れた流路へ次の力場が集まるようにする
14. 高さを bedrock floor や近傍関係でクランプして破綻を抑える
15. KTT の `Smooth_Flows` / `Add_Detail_Pass` に近い形で、最終段に平滑化した元地形との差分を侵食後へ戻す
16. 一定間隔で `fx/fy` と flow を再計算する

### 追加で近づけたい機能

- `age` / `erosion` / `wear` 出力
- Flow Lines 入力
- Erosion Mask / Hardness Mask
- Directionality の Force Vector / Shear
- タイル処理とタイル境界の padding
- KTT の Add Detail Pass をより正確にするための `Source_Terrain_Detail_Smoothing` 相当パラメータ
- CPU 実装で見た目を固めた後の GPU/D3D12 compute 追従

## 注意点

- HDA の実装は非決定的な挙動を含むが、Terrain Editor の通常UIではKTTにない乱数シードを表示せず、内部値で再現性を保つ。
- 1 unit = 1 m の前提では、`Feature Size` と `Channel Length` はメートル単位として扱うと分かりやすい。
- 侵食は重い処理になるため、プレビュー解像度と最終解像度を分ける必要がある。
- KTT の完全なHoudini互換ではなくても、見た目、パラメータの効き方、補助フィールドの扱いはKTTにかなり近づけることを優先する。
