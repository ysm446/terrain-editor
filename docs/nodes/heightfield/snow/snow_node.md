# Snow ノード

入力ハイトフィールドの上に**雪を降り積もらせる**フィルタノードです。GeoGen Snow ノードの「斜面に雪は積もらない」見た目を、複数ステップの簡易モデルで再現します。粒子シムは使わず、入力高さとパラメータから決定的に計算します。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 出力 | `Heightmap` (元地形 + 雪の厚み) / `Snow` (mask、雪量を 0..1 正規化) |

## 主な設定

| 設定 | 既定 | 役割 |
| --- | --- | --- |
| `Emission Amount (m)` | 1.0 | 平地 (slope <= Slope Limit Min) に積もる雪の最大厚み (m)。`0` のときは雪なしとして入力地形をそのまま通し、Snow mask も全ゼロになる |
| `Iterations Count` | 1 | 雪を何ステップで積もらせて安定化するか。値を増やすと少しずつ積もり、少量の Emission Amount で急に積雪面が出る挙動を抑えやすい |
| `Emission Time (%)` | 100 | `Iterations Count` のうち、どの割合まで雪を降らせ続けるか。0% は最初に全量を置いてから安定化し、100% は最後まで少しずつ降らせる |
| `Slope Limit Min (deg)` | 50.0 | この角度以下では雪が満杯まで積もる (Emission Amount まるごと) |
| `Slope Limit Max (deg)` | 60.0 | この角度以上では雪はまったく積もらない (剥き出しの岩肌)。Min と Max の間は smoothstep で滑らかに遷移 |
| `Mask Max Snow (m)` | 1.0 | Snow mask 出力の正規化基準 (`雪厚 / Mask Max Snow` を [0,1] にクランプ)。Emission Amount と同じ値にすれば満雪域が真っ白に出る |
| `Smoothing Iterations` | 8 | 雪の表面を反復的に平滑化 + 溝埋めする回数。各反復で `surface = heights + thickness` を分離ガウスブラーし、`max(surface, blurred)` でセル更新。0 = 平滑化なし、6-8 で積雪面が出やすい。詳細はアルゴリズム節参照 |
| `Largest Detail Level (m)` | 8.0 | GeoGen Snow 相当の最大ディテール幅。`4 m` から `512 m` までのプリセットから選び、雪面をならして隙間を埋める最大スケールをメートル単位で制御する |
| `Backend` | GPU | `CPU` / `GPU` (D3D12 compute shader) を切り替え。既定 GPU。シェーダーコンパイル/ディスパッチ失敗時は CPU に自動フォールバック |

## アルゴリズム

3 フェーズ構成:

### Phase 1: 動的な thickness 計算

1. **斜面角を計算**: 各セルで 4 タップ中央差分から `tan(slope) = √((dh/dx)² + (dh/dz)²)` を求める。複数 iteration では現在の雪面から傾斜を読みます。
2. **積雪割合**: `t = clamp((tan(slope) - tan(min)) / (tan(max) - tan(min)), 0, 1)` を smoothstep `t² × (3 - 2t)` で滑らかにし、`snowFraction = 1 - smoothT` を得る。
3. **雪の厚み**: `Emission Amount` を `Iterations Count` と `Emission Time` で分け、各ステップで `stepEmission × snowFraction` だけ足す。
4. **surface**: `surface = baseHeights + thickness` を smoothing 用バッファに書く。

`tan` で比較しているのは、ラジアンや度の比較より勾配の生値とそのまま噛み合うため。Min/Max は度で UI に出していますが、内部では `std::tan(deg × π/180)` に変換してから比較しています。

### Phase 2: snow envelope smoothing (`Smoothing Iterations` 回)

各反復で:
1. 現在の `surface` を `Largest Detail Level (m)` から求めた半径で、横方向→縦方向の分離ガウスブラーにより平均化する。
2. 各セルで `surface[c] = max(surface[c], blurred[c])` で更新。ただし、その時点までに発生した累積雪量を超える厚みは作りません。

これにより:
- **周囲より低いセル (= 溝の底)** は `blurred` が `surface` より高くなるので雪が増えて埋まる
- **周囲より高いセル (= 出っ張り)** は `surface` のまま変わらない (`max` が元値を保持)
- スロープ遷移域の per-cell な thickness 揺らぎが消え、雪が物理的に「積もって流れて埋める」自然な見た目になる

反復するごとに「snow envelope」がさらに滑らかになり、より広い範囲の溝が埋まります。`Smoothing Iterations = 0` で旧挙動 (素のフィルタ)、`6-8` と `Largest Detail Level = 8m` 前後で積雪面が出やすくなります。

### Phase 3: 出力書き戻し

最終的な `surface` から `thickness = surface - baseHeights` を取り、`grid.heights = baseHeights + thickness` と `grid.mask = clamp(thickness / maskMaxSnow, 0, 1)` を書き出す。

スレッド並列は `ParallelForRows` で行単位。元高さは事前にスナップショットして使うため、in-place 更新の競合は発生しません。

## GPU Compute バックエンド

`Backend` プルダウンで `GPU` を選ぶと [shaders/snow_compute.hlsl](../../../../shaders/snow_compute.hlsl) の compute shader 群で評価します。アルゴリズムは CPU 版と同じ 3 フェーズ:

| エントリ | 役割 |
| --- | --- |
| `CSCopyInputHeights` | InputHeights → BaseHeights (UAV) |
| `CSComputeThickness` | per-cell slope + smoothstep + 初期 SurfA = base + thickness |
| `CSEnvelopeSmoothing` | `Smoothing Iterations` 回。各反復で横方向と縦方向のガウス blur を行い、`Largest Detail Level (m)` から求めた半径で envelope 更新 |
| `CSApply` | 最終 surface から thickness を求めて OutHeights + OutMask |

CB に `smoothDirection` フラグを入れて、横方向と縦方向のガウス blur パスを同じ compute shader で切り替えています。方向ごとに `max` せず、ガウス blur が完成した後に一度だけ `max(original, blurred)` するため、十字や斜めの伸びが出にくくなります。

per-pixel 完全並列なので 1024² で **CPU 比 5-15 倍程度高速** の見込み。シェーダーコンパイル / ディスパッチ失敗時は CPU 実装に自動フォールバックします。

## 用途の使い分け

| 目的 | パラメータの方向性 |
| --- | --- |
| 雪山頂上に厚く雪を被せる | `Emission Amount` 5-15m / `Slope Limit Min` 40° / `Slope Limit Max` 55° |
| 薄い積雪 (谷底だけ白く) | `Emission Amount` 0.5-1m / `Slope Limit Min` 10° / `Slope Limit Max` 30° |
| 寒冷地全面雪 (ほぼ全体に雪) | `Emission Amount` 2-5m / `Slope Limit Min` 70° / `Slope Limit Max` 80° |
| 急峻な雪山 (風衝地で雪が剥がれた感じ) | `Emission Amount` 3-5m / `Slope Limit Min` 30° / `Slope Limit Max` 45° |

## メモ

- 出力は **加算**。地形がせり上がります。`Mask Blend` で他のマスクと合成して特定領域だけ雪を出す合成も可能です。
- GeoGen Snow にあるパラメータのうち、本実装では `Emission Amount` / `Iterations Count` / `Emission Time` / `Slope Limit Min/Max` / `Smoothing Iterations` / `Largest Detail Level` (envelope smoothing) を採用。風 (Wind direction/intensity/chaos) や `Hardness mask intensity` などは未実装。必要になったら追加可能。
- キャッシュキーは入力ハッシュ + パラメータハッシュ。他ノードの編集や Snow パラメータ変更で該当ノードのみ再評価されます。
- 出力 mask は満雪域が 1.0、雪なし斜面が 0.0 のグラデーション。マスクシェーディングを `グレースケール` にするとほぼ GeoGen 参考画像と同じ見た目になります。

## GeoGen 観察メモ

- `Iterations Count` はスライダー最低値が `1`、既定値が `40`。マウスオーバー説明は `number of simulation steps`。値を上げるほど、まばらに点在している雪が一か所へ集まるように見える。
- `Emission Time` のヘルプは `Emission time during simulation. 0% inject all the snow and runs the simulation after, 100% is spreading the snow amount during the whole simulation time.`。`0%` では `Emission Amount` が初期状態で存在しているように見え、値を上げると勾配の上のほうに雪が発生しているのを確認できる。
- `Emission Amount` のヘルプは `Amount of snow injected on the terrain in meters.`。
- `Snow Motion Slope Limit` のヘルプは `angle of the terrain under which the snow is not falling/flowing`。これは「雪が落ちる/流れるか止まるか」を決める角度で、現在の `Slope Limit Min/Max` とは別の運動制御として扱う候補。
- GeoGen には風パラメータがあるため、雪の偏りや斜面上部への残り方には風の影響も入っている可能性がある。本実装ではまだ風要素は入れていない。

## 今後の調整メモ

- `Snow` mask は、積もっている場所か地面が顔を出している場所かを判別しやすい、はっきりした二値寄りの画像を優先したい。中間グレーは基本的に積雪境界やフェード幅だけに出るのが望ましい。
- 現在の `Snow` mask は雪厚を `Mask Max Snow` で正規化した連続値なので、薄い積雪が広いグレーとして出やすい。GeoGen 風に寄せるなら、内部雪厚とは別に表示/出力用 mask を `coverage` として作り、閾値 + feather で白黒寄りに整える案がある。
- `Snow Motion Slope Limit` を追加する場合は、雪の発生条件ではなく「積もった雪が流れる/落ちる」条件として設計する。既存の `Slope Limit Min/Max` は積雪量の初期分布、`Snow Motion Slope Limit` は移動・停止判定、という役割分担が自然。
