# Soil ノード

入力 `Heightmap` の上に表土を注入し、安息角に従って再配分することで、緩斜面・上面に土をかぶせて急斜面・崖では岩盤を露出させる **被覆型** の Heightfield ノードです。

堆積系ノードの役割分担は次のとおりです ([node_candidates.md](../../node_candidates.md) の設計メモ参照)。

- `Sediment` = 谷埋め型。重力輸送で溝・谷底に厚く溜まり、尾根は剥き出しになる (樹枝状)。
- `Soil` = 被覆型。表土マントルとして上面にかぶさり、急斜面では剥げる。**このノード。**
- `Snow` = 雪特化。アルゴリズムは被覆型だが、雪固有の拡張 (風・雪線・融雪) へ進化させる。

再配分コアは `Snow` と共有です (`src/evaluation/GranularSettle.cpp`、GPU は `shaders/snow_compute.hlsl`)。`Snow` との差分は、土向けの既定値 (安息角 32°、控えめな表面平滑化) と、以下の 2 つの固有パラメータです。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 出力 | `Heightmap` (元地形 + 表土厚) / `Soil` (mask) |

## 主な設定

| 設定 | 既定 | 役割 |
| --- | --- | --- |
| `Emission Amount (m)` | 1.0 | 平地に注入する表土の総厚です。`0` のときは入力地形をそのまま通し、`Soil` mask も全ゼロです。 |
| `Iterations Count` | 40 | simulation step 数です。値を上げるほど表土が安定位置へ落ち着きやすくなります。 |
| `Emission Time (%)` | 0 | どの割合の step まで表土を追加し続けるかです。`0%` は最初に全量を置いてから流します。 |
| `Soil Motion Slope Limit (deg)` | 32.0 | 安息角です。この角度以下の面では表土が動かず、これより急な面では低い隣接セルへ滑ります。 |
| `Slope-Dependent Emission (%)` | 50 | **Soil 固有。** 基盤地形の傾斜が安息角に近いほど注入量を減らします。`0%` は Snow と同じ一様注入、`100%` は安息角以上の斜面へ注入しなくなり、尾根・崖の岩盤露出が早く出ます。 |
| `Transport Rate (%)` | 45 | 不安定な表土のうち、1 settling pass で移動する割合です。 |
| `Soil Surface Smoothing (%)` | 10 | 積もった表土面だけをならす強さです。雪面ほど滑らかにならないよう、Snow (25%) より弱い既定値です。半径は `Largest Detail Level (m)` を流用します。 |
| `Mask Mode` | Coverage | **Soil 固有。** `Coverage` は `Mask Threshold / Feather` でほぼ白黒の被覆マスク (Snow と同じ意味論)。`Thickness` は表土厚を max で 0..1 正規化した厚み分布マスク (Sediment と同じ意味論) で、堆積量でグラデーションを付けたいときに使います。 |
| `Mask Threshold (m)` | 0.05 | Coverage 時: この厚み以上を被覆域として白に近づけます。 |
| `Mask Feather (m)` | 0.05 | Coverage 時: 被覆境界のグレー幅です。 |
| `Settling Passes` | 4 | 各 simulation step の中で表土を再配分する回数です。 |
| `Largest Detail Level (m)` | 8.0 | 表土が移動先を探す最大スケールです。`4 m` から `512 m` まで選べます。 |
| `Backend` | GPU | `CPU` / `GPU` (D3D12 compute) を切り替えます。GPU は Snow と共有の gather 方式再配分シェーダーで、失敗時は CPU へフォールバックします。 |

## アルゴリズム

1. `Emission Amount` を `Iterations Count` と `Emission Time` から各 step の注入量へ分割します。
2. 各 step で表土厚を追加します。`Slope-Dependent Emission` が 0 より大きい場合、基盤の傾斜から注入スケール `lerp(1, max(0, 1 - tan(slope)/tan(limit)), amount)` を掛けます (基盤は固定なのでスケールは前計算)。
3. `Settling Passes` 回、表土面 `base height + soil thickness` を見て、`Soil Motion Slope Limit` より急なセルから最も低い近傍へ表土を移動します。
4. `Soil Surface Smoothing (%)` が 0 より大きい場合、被覆域を中心に表土厚をならします。
5. 最終的な表土厚を入力地形へ加算して `Heightmap` を作ります。
6. `Soil` mask は `Mask Mode` に従い、Coverage (Threshold / Feather) または Thickness (max 正規化) で出力します。

手順 1〜5 の詳細は [Snow アルゴリズムメモ](../snow/snow_algorithm_guide.md) と共通です。

## 用途の使い分け

| 目的 | 推奨パラメータ |
| --- | --- |
| 標準的な表土被覆 (緩斜面に土、崖は岩盤) | 既定値 |
| 岩盤露出を強調 (稜線・崖をくっきり) | `Slope-Dependent Emission 80-100%` / `Soil Motion Slope Limit 28-30°` |
| 厚い土壌で谷寄りにも溜める | `Emission Amount 2-4m` / `Slope-Dependent Emission 0-30%` |
| 植生・土色マスクとして使う | `Mask Mode Coverage` のまま `Colorize` / `Mask Blend` へ |
| 堆積量グラデーションで色を分ける | `Mask Mode Thickness` + `Mask Levels` で補正 |

## メモ

- `Snow` を土の堆積に代用していたグラフは、このノードへ置き換えることを想定しています。
- 曲率依存の厚み分布、`Multi-Scale Erosion` の deposits 連携、植生マスク接続は今後の拡張候補です。
