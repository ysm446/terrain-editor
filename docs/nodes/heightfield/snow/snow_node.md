# Snow ノード

入力ハイトフィールドの上に**雪を降り積もらせる**フィルタノードです。GeoGen Snow ノードの「斜面に雪は積もらない」見た目を、シングルパスの簡易モデルで再現します。粒子シムや反復は無く、入力高さから決定的に計算します。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 出力 | `Heightmap` (元地形 + 雪の厚み) / `Snow` (mask、雪量を 0..1 正規化) |

## 主な設定

| 設定 | 既定 | 役割 |
| --- | --- | --- |
| `Emission Amount (m)` | 1.0 | 平地 (slope <= Slope Limit Min) に積もる雪の最大厚み (m)。地形が全体的にこの分だけ持ち上がる感覚 |
| `Slope Limit Min (deg)` | 50.0 | この角度以下では雪が満杯まで積もる (Emission Amount まるごと) |
| `Slope Limit Max (deg)` | 60.0 | この角度以上では雪はまったく積もらない (剥き出しの岩肌)。Min と Max の間は smoothstep で滑らかに遷移 |
| `Mask Max Snow (m)` | 1.0 | Snow mask 出力の正規化基準 (`雪厚 / Mask Max Snow` を [0,1] にクランプ)。Emission Amount と同じ値にすれば満雪域が真っ白に出る |

## アルゴリズム

シングルパスの簡易モデル:

1. **斜面角を計算**: 各セルで 4 タップ中央差分から `tan(slope) = √((dh/dx)² + (dh/dz)²)` を求める。
2. **積雪割合**: `t = clamp((tan(slope) - tan(min)) / (tan(max) - tan(min)), 0, 1)` を smoothstep `t² × (3 - 2t)` で滑らかにし、`snowFraction = 1 - smoothT` を得る。
3. **雪の厚み**: `thickness = emissionAmount × snowFraction`
4. **書き戻し**: `grid.heights[c] += thickness`、`grid.mask[c] = clamp(thickness / maskMaxSnow, 0, 1)`

`tan` で比較しているのは、ラジアンや度の比較より勾配の生値とそのまま噛み合うため。Min/Max は度で UI に出していますが、内部では `std::tan(deg × π/180)` に変換してから比較しています。

スレッド並列は `ParallelForRows` で行単位。元高さは事前にスナップショットしてから雪を加算するため、隣接セルへの「雪を含む高さの読み込み」は発生しません (将来 iterations を入れる余地を残してこの構造にしている)。

## 用途の使い分け

| 目的 | パラメータの方向性 |
| --- | --- |
| 雪山頂上に厚く雪を被せる | `Emission Amount` 5-15m / `Slope Limit Min` 40° / `Slope Limit Max` 55° |
| 薄い積雪 (谷底だけ白く) | `Emission Amount` 0.5-1m / `Slope Limit Min` 10° / `Slope Limit Max` 30° |
| 寒冷地全面雪 (ほぼ全体に雪) | `Emission Amount` 2-5m / `Slope Limit Min` 70° / `Slope Limit Max` 80° |
| 急峻な雪山 (風衝地で雪が剥がれた感じ) | `Emission Amount` 3-5m / `Slope Limit Min` 30° / `Slope Limit Max` 45° |

## メモ

- 出力は **加算**。地形がせり上がります。`Mask Blend` で他のマスクと合成して特定領域だけ雪を出す合成も可能です。
- GeoGen Snow にあるパラメータのうち、本実装では `Emission Amount` と `Slope Limit Min/Max` の三つを基本機能として採用。`Iterations count` / 風 (Wind direction/intensity/chaos) / `Hardness mask intensity` などは未実装 (粒子シムや反復が前提のものは省略)。必要になったら追加可能。
- アルゴリズムが per-pixel で完全に並列なので将来 GPU compute 化は容易。現状は CPU 実装のみ。
- キャッシュキーは入力ハッシュ + パラメータハッシュ。他ノードの編集や Snow パラメータ変更で該当ノードのみ再評価されます。
- 出力 mask は満雪域が 1.0、雪なし斜面が 0.0 のグラデーション。マスクシェーディングを `グレースケール` にするとほぼ GeoGen 参考画像と同じ見た目になります。
