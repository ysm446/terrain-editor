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
| Channeling | 0.25 | 0-1 | 水路状の削り込みをどれだけ強めるか。実装上は「堆積のキャンセル」として機能する |

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
| Erosion Granularity | 10 | 0-100 | 侵食粒子の密度。高いほど粒子が疎になり柔らかい形になる。kernel には `Erosion_Granularity + 1` が渡される |
| Flow Volume | 0 | 0-1 | `Update_Forces` カーネルの `Flow_Cutting` にバインドされる。`hbase = height + wear * Flow_Cutting` の係数として使われ、`wear`（侵食痕）を勾配計算に戻すフィードバック強度を決める。`dx <= Detail_Scale` のときだけ有効で、細かいスケール専用 |
| Small Channel Influence | 0 | 0-1 | 小さい水路の影響度。粒子密度の解像度依存指数 `2 - 2*Small_Channel_Influence` を変化させる（細かいスケールでのみ） |
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
| Tile Slowdown | 計算式 | 1-2 | タイリングによる速度低下の目安。`pow(Res+Pad,2)/pow(Res,2)` で自動算出 |

### Advanced

| KTT パラメータ | 初期値 | 範囲 | 意味 |
| --- | ---: | ---: | --- |
| Reference Detail Size | 1 | 0-2 | OpenCL カーネルの `Detail_Scale` にバインドされる。粒子ステップ数、力場フィードバック、Erosion Granularity の解像度依存指数の基準スケール。すべての `dx/Detail_Scale` 計算の分母 |
| Source Terrain Detail Smoothing | 1 | 0-10 | 入力地形の細部を平滑化してから侵食シミュレーションを行う度合い。流れがクリーンになる |
| Use Multigrid Acceleration | 1 | 0-1 | 複数解像度で処理して大きな特徴を作る |

### Output

| KTT パラメータ | 初期値 | 意味 |
| --- | ---: | --- |
| Visualize Output Fields | 0 | `mask` フィールドへ可視化値を書き出す |
| Vis Field | Deposits | 可視化対象フィールド (`Deposits`, `Flows`, `Age`) |
| Deposit Visualization Min | 0 | 可視化レンジ最小 |
| Deposit Visualization Max | 1 | 可視化レンジ最大 |
| Export Deposits | 0 | `deposits` フィールドを出力するかのトグル |
| Deposits Export Field | "deposits" | 出力先フィールド名 |
| Export Age | 0 | `age` フィールドを出力するかのトグル |
| Age Export Field | "age" | 出力先フィールド名 |
| Export Flows | 0 | `flows` フィールドを出力するかのトグル |
| Flows Export Field | "flows" | 出力先フィールド名 |

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

カーネル名: `Fluvial_Sim_Test`（ノード名 `Update_Forces` だがカーネル識別子は `Fluvial_Sim_Test`）。高さの近傍サンプルから勾配を計算し、流れの力場を `fx/fy` に書き込みます。

要点:

- 3 x 3 近傍 (i,j ∈ {-1,0,1}) を使って高さ差を取る
- 勾配計算の基準高さは `hbase = height + wear * Flow_Cutting`。`wear`（侵食痕フィールド）が力場にフィードバックされる。`Flow_Cutting` は UI の `Flow_Volume` パラメータがバインドされ、さらに `Flow_Cutting *= dx <= Detail_Scale ? 1.0 : 0.0` として細かいスケール専用に絞られる
- 各サンプルの寄与は方向重み付きで `i * (hsample - hbase) / (hypot(i,j) + 0.0001)` を加算（対角成分は `1/√2` で減衰）。`grad_y` も同様に `j *` 重み
- 全体を `grad_x /= dx*6`、`grad_y /= dy*6` で正規化（合計重みに合わせる）
- `fx = -grad_x`、`fy = -grad_y`
- 外力 `(FDirX, FDirY, FDirZ)` を `fx/fy` に加算する（`Force_Vector` × `Force_Strength`）
- `mask[idx] = 0` でこの反復の visit マスクをリセットする
- `#ifdef HAS_Age` のとき `Age[idx] += 0.1 * dx`（ボクセルサイズ比例）
- 境界の高さを隣接セルにコピーして補正する
- 反復カウンタ `if (gidx == 0 && gidy == 0) Iteration[0]++;` を1スレッドだけがインクリメント。次の `Transport_Particles` の散布シードに使う

### Transport_Particles

カーネル名: `Fluvial_Sim`。侵食の中心処理です。
地表に散布された粒子が `fx/fy` の力場に沿って移動しながら、高さを削る/堆積させます。

要点:

**粒子の初期化と起動チェック**
- 各グリッドスレッド（1スレッド = 1地形セル）が1粒子を担当する
- 開始位置は `Iteration[0]` を種として `Random2` で地形全体に散布される（スレッド位置とは独立）。`p.x = Iteration[0] + Random2(...) * clamp_x` を `clamp_x` で割って frac、再度 `clamp_x` 倍することで `[0, clamp_x)` にラップする。`Iteration[0]` は `Update_Forces` で毎反復1ずつ増えるため、反復ごとに散布パターンが小さくシフトする
- **角度はカーネル冒頭で `tan(angle*π/180)` に変換される**。比較対象 `slope = length(f)` は勾配ベクトルの大きさ（rise/run）なので、tangent との比較で次元が一致する
- 開始セルで `slope = length(f)`（力場の長さ）と曲率（3x3近傍 `dh/dx/8` 累積）を計算し `max(slope, -curvature) > Wear_Angle` を満たすか判定する
  - **`-curvature` は凸型の尾根で正値になる**（中心が周囲より高いと `dh < 0` で `curvature < 0`）。平坦でも凸尾根なら侵食が開始できる
  - ここで使う `slope` は高さ勾配ではなく `length(f)` であることに注意。`f = -∇h + Force_Vector` なので、勾配の大きさ + 外力の混合
- `Erosion_Granularity` で粒子密度を調整する。式は `Granularity_Threshold = 1 - 1 / (Granularity / pow(dx/Detail_Scale, e))`、`Random2 > Threshold` ならスキップ。指数 `e` は `dx > Detail_Scale` なら 2、そうでなければ `2 - 2*Small_Channel_Influence`
- `mask[sampleidx] <= 0.1` でないなら同一反復内での重複起動を防ぐ（`visitMask` ではなく `mask` フィールドを再利用）。マスクは `Update_Forces` で 0 にリセット済み
- `Erode_Mask[sampleidx] > 0` でなければ侵食しない
- `mask[sampleidx] = DO_EROSION` を書き込んでから、**`if (!DO_EROSION) return;` で粒子を即終了**（移動も発生しない）

**粒子の移動**
- 位置更新: `p += Sediment_Velocity * v / (1 + length(f))`
  - 急斜面では分母が大きくなり**粒子が自動的に減速する**
  - **重要**: ここで使う `f` はループ外で初期化された開始セルの力場（OpenCLのスコープ規則により、ループ内の `float2 f = ...` は **shadow** で、ループ次のイテレーション先頭の `length(f)` は外側スコープの開始セル `f` を参照する）
- 移動後に境界判定: `p.x < 0 || p.x > clamp_x || p.y < 0 || p.y > clamp_y` のとき `DO_EROSION = 0`（次の `while` 条件で抜ける）
- ループ内ローカル `f` を新しい位置から再サンプル（速度更新と角度判定に使う）
- 速度更新（スケール依存）:
  - `v *= pow(1 - Friction, dx/Detail_Scale)` — 高解像度ほど1ステップあたりの摩擦が少ない
  - `v += f * dx/Detail_Scale` — 力もスケール依存で加算
- ループ上限: `Spread_Iterations / (dx / Detail_Scale)` — 高解像度ほどステップ数が増える。`Spread_Iterations` には UI の `Channel_Length` がバインドされる
- 速度方向は L2 正規化後に L1 スケールを掛けて正規化する（対角方向と軸方向のステップ長を揃える）
  - `vs = v / (length(v) + 0.0001)` で L2 正規化、続いて `vs *= (|vs.x| + |vs.y|) / (length(vs) + 0.0001)`。L2正規化済み `vs` は `|vs|=1` なので実質 `vs *= |vs.x| + |vs.y|`
- 前後サンプル位置: `p2 = p + vs + shear`、`p3 = p - vs + shear`

**侵食の判定と高さ更新**
- 各ステップで `slope = length(f)`（新位置の力場長）を取り、Deposit Angle と Max Erosion Angle と比較する
  - `DO_EROSION = slope >= Deposit_Angle ? DO_EROSION : 0`
  - `DO_EROSION = slope < Max_Angle ? DO_EROSION : 0`
  - **範囲外になると `DO_EROSION = 0` が次の `while` 条件で評価されてループを抜ける**。同イテレーションでの高さ更新も `dh *= DO_EROSION = 0` で 0 に潰れる（実質 `break` と等価）
- 高さ更新式（カーネル原文）: `dh = ((h2 + h3)/2 * Flow_Strength * DO_EROSION + h1 * (1 - Flow_Strength * DO_EROSION)) - h1`
  - 整理すると `dh = Flow_Strength * DO_EROSION * ((h2 + h3)/2 - h1)` — h1 と前後平均の lerp
  - sediment capacity の概念はない
  - `Flow_Strength = Erosion_Strength`（パラメータ直値）
- **Hardness mask**: `#ifdef HAS_Hardness` のとき `dh *= 1 - Hardness[sampleidx]`（硬さで侵食を弱める）
- **Age 減衰**: `#ifdef HAS_Age` のとき `Age[sampleidx] *= pow(0.5, |dh|*10/dx)` — 侵食量に応じて age を指数減衰させる（侵食された場所は「若返る」）
- **Channeling の実際の動作**: `height -= Channeling * max(height - h1, 0)` — 高さが上がった（堆積）分を `Channeling` 割合でキャンセルする。侵食量を増やすのではなく堆積を打ち消す
- **carry による保存則**: `carry += h1 - height`。`carry < 0`（堆積が過剰）になった場合は `height = h1 - carry; carry = 0` で高さを戻す。土砂が無から生まれないことを保証する
- 高さを `[min(h2, h3), max(h2, h3)]` の範囲にクランプして破綻を抑える
- `wear += max(h1 - height, 0)`、`deposit += max(height - h1, 0)` で各フィールドを蓄積する

**Flow_Volume について**
- `Flow_Volume` は `Transport_Particles` カーネルのシグネチャには存在しないが、`Update_Forces` の `Flow_Cutting` パラメータにバインドされている。`hbase = height + wear * Flow_Cutting` の係数として、`wear`（侵食痕）を勾配計算にフィードバックする強度を制御する。これにより既存の侵食痕が次の流れを引き寄せるようになり、より明確なチャンネルが形成される。`dx <= Detail_Scale` のときだけ有効

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

| パラメータ | 初期値 | 範囲 | 備考 |
| --- | ---: | ---: | --- |
| Feature Size | 8 | 0-32 | HDA に合わせる |
| Geological Age | 20 | 0-20 | HDA に合わせる |
| Iterations | 25 | 0-100 | HDA に合わせる |
| Channel Length | 128 | 0-512 | HDA に合わせる |
| Erosion Strength | 1 | 0-1 | HDA デフォルト |
| Channeling | 0.25 | 0-1 | HDA デフォルト |
| Friction | 0.1 | 0-1 | HDA デフォルト |
| Wear Angle | 15 | 0-90 | tan() 比較 |
| Deposit Angle | 0 | 0-90 | tan() 比較 |
| Max Erosion Angle | 30 | 0-90 | tan() 比較 |
| Erosion Granularity | 10 | 0-100 | kernel には `+1` で渡す |
| Flow Volume | 0 | 0-1 | `Update_Forces` の `Flow_Cutting` |
| Small Channel Influence | 0 | 0-1 | granularity 指数を変える |
| Sediment Velocity | 1 | 0-2 | HDA デフォルト |
| Force Vector | (0,0,0) | 各 0-1 | 力場に加算する外力ベクトル |
| Force Strength | 1 | 0-1 | 外力の倍率 |
| Shear X / Y | 0 | -0.1 ~ 0.1 | サンプル位置のずらし量 |
| Reference Detail Size | 1 | 0-2 | kernel の `Detail_Scale` |
| Source Terrain Detail Smoothing | 1 | 0-10 | 入力地形の細部平滑化 |
| Use Multigrid Acceleration | 1 | 0-1 | マルチスケール処理 |

Terrain Editor 側の内部係数やマルチスケール強度は、KTT の表示パラメータから自動的に決めます。
KTT の UI に存在しない `Sediment Capacity`、`Deposition Rate`、`Large/Medium/Detail Scale`、`Level Strength`、`Seed` は設定項目として保存せず、内部係数として扱います。

### CPU 実装方針

1. 入力 heightfield を作業バッファへコピーする
2. 高さと `wear` フィードバックから `fx/fy` の力場を計算する。KTT の `Flow_Cutting`（= UI の `Flow_Volume`）に相当する係数で `wear` を力場に反映する。`dx <= Detail_Scale` のときだけ有効
3. 各サンプルの寄与は方向重み `i / (hypot(i,j)+0.0001)` で計算し、最後に `dx*6` で正規化する
4. Feature Size と内部スケール係数から、粗い谷筋、中規模支流、細かいリルの複数スケールに分ける。粗いスケールは coarse level fade で反復数を絞る。`Geological Age` による反復スケーリングは単一レベル関数内で一元管理する（レベルループと二重適用しない）
5. iteration ごとにKTTの `Transport_Particles` に近い式で粒子開始点を乱数散布する。開始位置はスレッド座標ではなく `iteration + Random2(x, z)` で地形全体に分散させる。`Iteration` は整数（カーネルでは1ずつインクリメント）として扱い、frac で `[0, clamp_x)` にラップする
6. 粒子ごとに `Channel_Length / (dx / Detail_Scale)` に近いステップ数だけ移動する（高解像度ほどステップ数増）
7. 位置更新は `p += Sediment_Velocity * v / (1 + length(f))` — 急斜面では自動的に減速する。`length(f)` は開始セル時点の力場長を使う（OpenCLのスコープ規則と一致）
8. 速度更新はスケール依存: `v *= pow(1 - Friction, dx/Detail_Scale)`、`v += f * dx/Detail_Scale`
9. 速度方向は L1 ノルムで正規化して対角/軸方向のステップ長を揃える
10. Wear Angle 等の角度はすべて `tan(angle*π/180)` に変換してから比較する（カーネルと一致）
11. Wear Angle + 曲率チェックで起動判定: `max(length(f), -curvature) > tan(Wear_Angle)`（`-curvature` は凸尾根で正値）。失敗時は `return` で粒子を即終了
12. Deposit Angle と Max Erosion Angle を各ステップで判定し、範囲外なら `DO_EROSION = 0` でループ終了（dh も自動的に 0 になる）
13. 高さ更新は `lerp(before, (ahead + behind) / 2, flowStrength)` によるブレンド。capacity ベースではない
14. `Channeling` は堆積分のキャンセルに使う: `height -= channeling * max(height - before, 0)`
15. `carry` 変数で土砂の保存則を維持する。`carry < 0` になったら高さを戻して `carry = 0` にする
16. 同一反復内の重複侵食を `mask` フィールドで抑える（`Update_Forces` で 0 リセットされる）
17. 高さを bedrock floor や前後サンプル範囲でクランプして破綻を抑える
18. `Flow Volume` は `Update_Forces` の `Flow_Cutting` として実装し、`wear` を勾配計算にフィードバックする強度を制御する（細かいスケール専用）
19. `Small Channel Influence` は granularity 指数 `2 - 2*Small_Channel_Influence` として、細かいスケールでの粒子密度を変える
20. 粒子通過量を `flows`、堆積量を `deposits`、`age` を `age` として蓄積・出力する
21. KTT の `Add_Detail_Pass` に近い形で、最終段に元地形の高周波成分を侵食度合いに応じて戻す
22. 一定間隔で `fx/fy` と flow を再計算する

### 追加で近づけたい機能

- `age` / `erosion` / `wear` 出力（HDA も `age`、`deposits`、`flows` を出力できる）
- Flow Lines 入力（HDA の Input 2）
- Erosion Mask / Hardness Mask（HDA に既存の `Use_Erosion_Mask`、`Use_Hardness_Mask`、`Invert_*` トグルと同等）
- Directionality の Force Vector / Force Strength / Shear X / Shear Y（HDA の `Update_Forces` と `Transport_Particles` で使用済み）
- タイル処理とタイル境界の padding（HDA の `Use_Tiling`、`Tile_Resolution`、`Tile_Padding`）
- 出力可視化（`Visualize_Deposits`、`Vis_Field`、`Deposit_Visualization_Min/Max`）
- 補助フィールド出力先名（`Deposits_Export_Field` 等）
- CPU 実装で見た目を固めた後の GPU/D3D12 compute 追従

## 注意点

- HDA の実装は非決定的な挙動を含むが、Terrain Editor の通常UIではKTTにない乱数シードを表示せず、内部値で再現性を保つ。
- 1 unit = 1 m の前提では、`Feature Size` と `Channel Length` はメートル単位として扱うと分かりやすい。
- 侵食は重い処理になるため、プレビュー解像度と最終解像度を分ける必要がある。
- KTT の完全なHoudini互換ではなくても、見た目、パラメータの効き方、補助フィールドの扱いはKTTにかなり近づけることを優先する。
