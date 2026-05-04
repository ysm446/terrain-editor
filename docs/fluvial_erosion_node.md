# Fluvial Erosion ノード設計メモ

このメモは Terrain Editor の `Fluvial Erosion` ノードについて、現在の実装と、KTT 風の自然な水侵食へ近づけるために今後追加したい処理を分けて整理したものです。

## 更新履歴

| バージョン | 内容 |
| --- | --- |
| 0.4.0 | KTT を参考にした CPU 版 `Fluvial Erosion` ノードを追加しました。Heightfield を入力し、簡易的な粒子移動と channeling で侵食筋を作る MVP です。 |
| 0.5.0 | ノードプロパティへツールチップと単位表示を追加しました。 |
| 0.5.1 | 本設計メモを追加し、現状プロセスと今後必要なアルゴリズムを整理しました。 |
| 0.6.0 | flow accumulation / drainage area を追加し、流量が多い場所ほど侵食が強くなるようにしました。粗い谷形成パスと細かいチャンネル形成パスのマルチスケール処理も追加しました。 |
| 0.7.0 | Terrain Editor のノード構成を地形用ノードへ整理し、旧 Rock Generator 由来のノードを追加メニューと読み込み対象から外しました。 |
| 0.7.1 | 本メモへ更新履歴と現状アップデートを追記しました。 |
| 0.8.0 | sediment capacity と deposit field を追加し、粒子が削った土砂を保持して谷底や緩斜面へ堆積できるようにしました。 |
| 0.9.0 | `Fluvial Erosion` に `Heightmap` と `Fluvial Mask` の 2 出力を追加し、出力ピンをクリックして地形プレビューとマスクプレビューを切り替えられるようにしました。 |
| 0.15.0 | `Level Strength` を追加し、低解像度から高解像度へスケール別に浸食差分を重ねるマルチレベル処理を導入しました。 |
| 0.15.1 | Basic / Advanced を分け、通常操作では `Large Scale` / `Medium Scale` / `Detail Scale` を中心に調整できるようにしました。 |
| 0.15.2 | 将来の GPU Compute 実装に向けて、`Backend` と CPU/GPU 一致方針を追加しました。 |
| 0.15.3 | GPU Compute へ移行しやすい、決定的なグリッド同期更新パスを CPU 側へ追加しました。 |
| 0.15.4 | D3D12 compute shader と compute pipeline state の初期化を追加し、GPU Compute の準備状態を UI に表示するようにしました。 |
| 0.15.5 | 小さなハイトフィールドを GPU バッファへ転送し、compute shader を dispatch して読み戻す自己診断を追加しました。 |
| 0.15.6 | `GPU Compute` を本番のハイトフィールド評価へ接続し、GPUで高さとマスクを更新して読み戻せるようにしました。 |
| 0.15.7 | GPU Compute の侵食量計算へセルサイズ、角度ゲート、Sediment Capacity、Channeling を反映し、CPUグリッドパスに近づけました。 |
| 0.15.8 | 非同期評価スレッドから本番GPU評価を実行しない安全柵を追加し、メインスレッドスケジューラ実装までCPUへフォールバックするようにしました。 |
| 0.15.9 | バックグラウンド評価スレッドからGPUジョブをキューへ積み、メインスレッドでD3D12実行して結果を返すスケジューラを追加しました。 |
| 0.15.10 | GPU Compute がマルチレベル処理を迂回していた問題を修正し、LevelごとにGPU計算するようにしました。 |
| 0.15.11 | GPU Compute にD8近似の flow accumulation パスを追加し、水が集まる筋ほど侵食が強くなるようにしました。 |

## 現在のアップデート

現在の `Fluvial Erosion` は、初期の単純な斜面方向の粒子侵食から一段進み、内部に flow accumulation を持つようになりました。

大きな変更点は次の通りです。

- D8 風の receiver を使い、各セルがどの下流セルへ流れるかを決めます。
- 標高順に flow を下流へ足し込み、drainage area を作ります。
- `log(flow)` で正規化した流量を、粒子発生率、侵食強度、channeling の重みに使います。
- 粗い谷形成パスと細かいチャンネル形成パスに分け、単一スケールよりも谷筋と支流が出やすい構成にしています。
- 粒子ごとに `sediment` を持ち、流速、斜面、流量から求めた capacity と比較して、削剥と堆積を切り替えます。
- `depositField` を内部に持ち、土砂がどこに堆積したかを蓄積します。
- `Fluvial Mask` 出力ピンを選ぶと、侵食・堆積の内部マスクを地形メッシュ上へ色付きで表示します。
- `Level Strength` により、低解像度の大きな谷筋から高解像度の細かいリルまで、スケールごとに強度を変えられます。
- Basic UI では `Large Scale` / `Medium Scale` / `Detail Scale` を使い、Advanced UI では個別 Level や角度系パラメータを確認できます。
- ただし hardness mask、flow line input、マスクの外部テクスチャ書き出しはまだ未実装です。

そのため、現状は「水が集まる場所ほど削れる」だけでなく、「削った土砂を運び、谷底や緩斜面へ堆積させる」初期段階まで入りました。さらに `Fluvial Mask` プレビューで、どこに侵食・堆積が出ているか確認できます。次の大きな改善点は、flow / wear / deposit の個別可視化、D-infinity flow、hardness / erodibility です。

## 目的

`Fluvial Erosion` は、ハイトフィールド地形に水流による侵食跡を加えるノードです。

理想は、単に斜面へ均一な縦筋を入れることではなく、尾根から谷へ水が集まり、支流が合流し、削剥と堆積が場所によって変わるような地形を作ることです。

## 現状の位置づけ

現在のノードは、KTT の HDA をそのまま移植したものではありません。

KTT の考え方を参考にしつつ、Terrain Editor 上でまず動くことを優先した CPU 版の簡易 MVP です。KTT のような OpenCL ベースの粒子輸送、補助フィールド出力、タイル処理、マルチグリッド処理はまだ入っていません。

## 現状のプロセス

現在の実装は、おおまかに次の流れです。

1. 入力ハイトフィールドを作業用グリッドへコピーする。
2. 近傍セルの高さ差から勾配を計算する。
3. 勾配の逆方向を流れ方向として `forceX / forceZ` を作る。
4. D8 風に各セルの流出先を選び、標高順に drainage area / flow accumulation を蓄積する。
5. `log(flow)` で正規化した流量フィールドを作る。
6. 粗いパスで大きな谷筋を作る。
7. 細かいパスで支流や表面チャンネルを足す。
8. 流量が多い場所ほど粒子発生率、侵食強度、`Channeling` の効果を強める。
9. 粒子を流れ方向へ移動させる。
10. 現在地点、前方、後方の高さを比較して、斜面を削る。
11. `Friction` と `Sediment Velocity` で粒子の移動を調整する。
12. 一定間隔で流れ方向と flow accumulation を再計算する。

## 現状の主なパラメータ

| パラメータ | 役割 |
| --- | --- |
| Feature Size (m) | 侵食で扱う地形特徴の大きさ |
| Iterations | 粒子処理の反復回数 |
| Channel Length (m) | 粒子が進む距離の目安 |
| Erosion Strength (%) | 削り込みの強さ |
| Channeling (%) | 筋状の水路を強める度合い |
| Friction (%) | 粒子速度の減衰 |
| Wear Angle (deg) | 侵食が始まる斜面角度 |
| Deposit Angle (deg) | 堆積の目安になる斜面角度 |
| Max Erosion Angle (deg) | 侵食を許可する最大斜面角度 |
| Granularity (%) | 粒子密度や細かさの制御 |
| Sediment Velocity (x) | 粒子の移動速度 |
| Sediment Capacity (%) | 粒子が保持できる土砂量 |
| Deposition Rate (%) | 土砂を堆積させる速さ |
| Seed | 粒子分布の乱数シード |

## 現状で出やすい見た目

現在の処理では、斜面方向に沿った侵食筋は出せます。

一方で、粒子発生が比較的規則的で、流量の蓄積を見ていないため、筋が等間隔に見えたり、斜面全体に均質な削り跡が出やすくなります。

## 理想に対して足りないもの

### Flow Accumulation

`0.6.0` で最初の flow accumulation を追加しました。

現在は、各セルから最も低い近傍セルへ流す D8 風の receiver を作り、標高の高いセルから低いセルへ流量を積み上げています。流量は `log(flow)` で正規化し、粒子発生率、侵食強度、channeling の重みとして使います。

今後は D-infinity や複数方向分配にすると、格子方向の癖をさらに減らせます。

### Drainage Network

2 枚目のような見た目には、谷筋へ向かう枝状の流路ネットワークが必要です。

単純な斜面方向の粒子移動だけではなく、D8 / D-infinity のような流向計算や、流量に応じた支流生成が必要になります。

### Sediment Transport

`0.8.0` で最初の sediment transport を追加しました。

現在は、粒子が `sediment` を持ち、速度、斜面、流量から計算した capacity と比較します。capacity より sediment が少ない場合は削り、capacity を超えた場合は堆積します。

今後は、より物理寄りの capacity 式、粒子の水量、地質硬度、堆積物の粒径などを足すと制御しやすくなります。

現在使っている主な状態:

- water / flow
- sediment
- capacity
- wear
- deposit

### Deposit Field

`0.8.0` で内部的な deposit field を追加しました。

現状は堆積量を内部で蓄積するだけで、まだノード出力やビュー表示はありません。今後は deposit field をマスクや可視化レイヤーとして扱えるようにすると、調整しやすくなります。

### Particle Distribution

現在はグリッド間隔をベースに粒子を発生させています。

そのため、規則的な筋が出やすくなります。粒子発生をランダム、ブルーノイズ、斜面依存、流量依存に変えると自然さが増します。

### Multi Scale Pass

`0.6.0` で最初のマルチスケール処理を追加しました。

現在は、`Iterations` を粗い谷形成パスと細かいチャンネル形成パスに分けています。粗いパスでは Feature Size と Channel Length を大きめに扱い、大きな谷筋を作ります。細かいパスでは Feature Size と Channel Length を小さめに扱い、支流や表面ディテールを足します。

今後は解像度を変えた専用の coarse grid / detail grid を持つと、より安定した大地形と細部を両立できます。

### Hardness / Erosion Mask

地質の硬さや侵食されにくさがないため、地形全体が均質に削れます。

硬さフィールドやマスクがあると、削れやすい場所、削れにくい場所を作れます。

### Flow Lines Input

KTT には Flow Lines 入力があります。

Terrain Editor でも将来的に `Flow Lines` ノードや `River Guide` ノードを追加すると、任意の水路や谷筋をユーザーが誘導できます。

### Detail Pass

最後に細かいチャンネルやノイズ状の侵食を足す処理が必要です。

ただし、単なるノイズではなく、流向や流量に沿って細部を足すことが重要です。

## 今後の実装順

1. D-infinity などの複数方向 flow accumulation へ改善する。
2. 粒子発生をさらにランダム / ブルーノイズ / 流量依存へ変更する。
3. deposit / flow / wear などの補助フィールドを可視化・出力できるようにする。
4. capacity 式を水量、速度、斜面、粒子量に分けて調整しやすくする。
5. coarse grid / detail grid を分けたマルチスケール処理へ発展させる。
6. Hardness / Erosion Mask 入力を追加する。
7. Flow Lines 入力ノードを追加する。
8. CPU 実装で見た目を固めたあと、GPU compute へ移行する。

## 近い目標

次の改善としては、deposit / flow / wear フィールドを可視化するのが効果的です。

削る場所と堆積する場所を見ながら調整できるようになると、capacity や deposition rate の係数を安定して詰められます。その次に D-infinity flow や hardness / erodibility を足すと、より汎用的な侵食ノードへ近づきます。

## GPU 化について

KTT は OpenCL を使っており、粒子輸送や流れ場更新は GPU 向きです。

Terrain Editor では D3D12 を使っているため、将来的には compute shader で以下を処理するのがよさそうです。

- flow direction の更新
- flow accumulation
- particle transport
- sediment capacity
- wear / deposit の蓄積
- smoothing / detail pass

### CPU / GPU 一致方針

最終的なゴールは、`Backend` で `CPU Reference` と `GPU Compute` を切り替えられ、同じ入力、同じパラメータ、同じ Seed ならできるだけ同じ結果になることです。

そのため、GPU 版だけを別物として作るのではなく、まず CPU/GPU の両方で実装しやすいグリッドベースのパス構成へ寄せます。

方針:

1. `CPU Reference` を正しさ確認用として残す。
2. `GPU Compute` は D3D12 compute shader で追加する。
3. 並列実行順序で結果が変わりやすい粒子のランダム加算は減らし、固定順序のグリッド更新、ping-pong バッファ、決定的な乱数を優先する。
4. CPU 版にも GPU と同じパス構成を持たせ、比較できるようにする。
5. 将来的に `CPU vs GPU Difference Mask` を追加し、差分を視覚的に確認できるようにする。

現時点の `GPU Compute` は compute shader と pipeline state の初期化に加えて、本番の `HeightfieldGrid` を GPU バッファへ転送し、compute shader を複数回 dispatch して CPU 側へ読み戻すところまで実装済みです。失敗した場合は `CPU Reference` へフォールバックします。

ただし、GPU 版はまだ CPU Reference と同じ完全なアルゴリズムではありません。現段階では、GPU実行の配線、バッファ往復、ping-pong 更新、マスク読み戻しを確認するための最初の実装です。`0.15.7` でセルサイズ、角度ゲート、Sediment Capacity、Channeling は反映し、`0.15.10` でマルチレベル処理にも接続しました。`0.15.11` ではD8近似の flow accumulation を追加しましたが、CPU版と同じ順序付きの集水計算や堆積の散布はまだ一致していません。

また、ノード評価はバックグラウンドスレッドで走るため、D3D12の本番GPU評価をそのまま非同期評価スレッドから実行すると描画側と競合する可能性があります。`0.15.9` では、バックグラウンド評価スレッドがGPUジョブをキューへ積み、メインスレッドがD3D12上で実行して結果を返す形にしました。これで描画キューの所有をメインスレッド側へ寄せたまま、非同期評価からGPU計算を要求できます。
