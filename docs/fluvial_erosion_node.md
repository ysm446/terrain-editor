# Fluvial Erosion ノード設計メモ

このメモは Terrain Editor の `Fluvial Erosion` ノードについて、現在の実装と、KTT 風の自然な水侵食へ近づけるために今後追加したい処理を分けて整理したものです。

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

現在の土砂運搬はかなり簡略化されています。

KTT 風にするには、粒子が土砂を保持し、速度、斜面、流量、容量に応じて削る量と堆積する量を決める必要があります。

必要になる状態:

- water / flow
- sediment
- capacity
- wear
- deposit

### Deposit Field

削った土砂をどこへ積むかが弱いため、谷底や緩斜面の自然な埋まり方が出にくいです。

堆積量を別フィールドとして持ち、最終的に高さへ合成する処理があると制御しやすくなります。

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
3. sediment capacity を導入し、削剥と堆積を分ける。
4. deposit / flow / wear などの補助フィールドを内部に持つ。
5. coarse grid / detail grid を分けたマルチスケール処理へ発展させる。
6. Hardness / Erosion Mask 入力を追加する。
7. Flow Lines 入力ノードを追加する。
8. CPU 実装で見た目を固めたあと、GPU compute へ移行する。

## 近い目標

次の改善としては、sediment capacity と deposit field を入れるのが効果的です。

流量によって削る場所は見え始めるため、次は削った土砂を保持し、谷底や緩斜面へ堆積させる処理を足すと、より KTT に近い侵食になります。

## GPU 化について

KTT は OpenCL を使っており、粒子輸送や流れ場更新は GPU 向きです。

Terrain Editor では D3D12 を使っているため、将来的には compute shader で以下を処理するのがよさそうです。

- flow direction の更新
- flow accumulation
- particle transport
- sediment capacity
- wear / deposit の蓄積
- smoothing / detail pass

ただし、まずは CPU 版でアルゴリズムと見た目を固める方が安全です。見た目の正解が定まる前に GPU 化すると、調整とデバッグが難しくなります。
