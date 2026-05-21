# Snow ノード

入力 `Heightmap` の上に雪を注入し、斜面上で低い場所へ再配分して積雪地形と `Snow` マスクを出力する Heightfield ノードです。

従来の「傾斜が急な場所ほど雪を減らす」フィルタではなく、`Emission Amount (m)` で投入した雪を `Iterations Count` と `Emission Time (%)` に沿って動的に積もらせます。`Snow Motion Slope Limit (deg)` より急な雪面では雪が流れ、谷底、棚、緩い尾根などに集まりやすくなります。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 出力 | `Heightmap` (元地形 + 積雪厚) / `Snow` (coverage mask) |

## 主な設定

| 設定 | 既定 | 役割 |
| --- | --- | --- |
| `Emission Amount (m)` | 1.0 | 地形へ注入する雪の総量です。`0` のときは入力地形をそのまま通し、`Snow` mask も全ゼロです。 |
| `Iterations Count` | 40 | simulation step 数です。GeoGen の既定値に合わせています。値を上げるほど、まばらな雪が安定位置へ集まりやすくなります。 |
| `Emission Time (%)` | 0 | どの割合の simulation step まで雪を降らせ続けるかです。`0%` は最初に全量を置いてから流し、`100%` は最後まで少しずつ追加します。 |
| `Snow Motion Slope Limit (deg)` | 35.0 | この角度以下の雪面では雪が流れません。これより急な雪面では低い隣接セルへ雪が移動します。 |
| `Transport Rate (%)` | 45 | 不安定な雪のうち、1 settling pass で移動する割合です。高いほど急斜面から雪が早く逃げます。 |
| `Snow Surface Smoothing (%)` | 25 | 積もった雪面だけをならす強さです。半径は `Largest Detail Level (m)` を流用します。 |
| `Settling Passes` | 4 | 各 simulation step の中で雪を再配分する回数です。増やすほど谷底や棚へまとまりやすくなります。 |
| `Mask Threshold (m)` | 0.02 | この雪厚以上を積雪域として白に近づけます。 |
| `Mask Feather (m)` | 0.015 | 積雪境界のグレー幅です。小さいほど二値に近い mask になります。 |
| `Largest Detail Level (m)` | 8.0 | 雪が移動先を探す最大スケールです。`4 m` から `512 m` まで選べます。 |
| `Backend` | GPU | `CPU` / `GPU` (D3D12 compute) を切り替えます。GPU は gather 方式の再配分モデルで、失敗時は CPU へフォールバックします。 |

## アルゴリズム

1. `Emission Amount` を `Iterations Count` と `Emission Time` から各 step の注入量へ分割します。
2. 各 step で雪厚を追加します。
3. `Settling Passes` 回、雪面 `base height + snow thickness` を見て、`Snow Motion Slope Limit` より急なセルから最も低い近傍へ雪を移動します。
4. `Snow Surface Smoothing (%)` が 0 より大きい場合、積もった雪面側を中心に雪厚をならします。半径は `Largest Detail Level (m)` を使います。
5. 最終的な雪厚を入力地形へ加算して `Heightmap` を作ります。
6. `Snow` mask は `Mask Threshold` と `Mask Feather` で coverage 化します。中間グレーは主に積雪境界だけに出ます。

## GeoGen 観察メモ

- `Iterations Count` はスライダー最低値が `1`、既定値が `40`。マウスオーバー説明は `number of simulation steps`。値を上げるほど、まばらに点在している雪が一か所に集まるように見える。
- `Emission Time` のヘルプは `Emission time during simulation. 0% inject all the snow and runs the simulation after, 100% is spreading the snow amount during the whole simulation time.`。
- `Emission Amount` のヘルプは `Amount of snow injected on the terrain in meters.`。
- `Snow Motion Slope Limit` のヘルプは `angle of the terrain under which the snow is not falling/flowing`。
- GeoGen には風パラメータがあるため、斜面上部への残り方や偏りには風の影響も含まれている可能性があります。現在の Terrain Editor の Snow ノードにはまだ風要素は入れていません。

## メモ

- `Slope Limit Min/Max` と `Mask Max Snow` は古いプロジェクトの保存互換用に残っています。現在の UI と再配分モデルでは、主に `Snow Motion Slope Limit`、`Mask Threshold`、`Mask Feather` を使います。
- GPU compute shader は、各セルが近傍から流入量を gather する方式です。atomic 加算を使わないため、複数セルが同じ場所へ流れ込む場合でも書き込み競合を避けられます。
