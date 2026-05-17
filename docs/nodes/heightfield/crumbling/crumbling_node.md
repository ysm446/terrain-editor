# Crumbling ノード

`Crumbling` は、入力 `Heightmap` 上で `Emission Mask` の明るい場所から崩落岩片を発生させ、地形の低い方向へ流して堆積させる Heightfield 系ノードです。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
| 入力 | `Emission Mask` |
| 出力 | `Heightmap`。岩屑を加算したハイトフィールド |
| 出力 | `Mask`。岩屑が堆積した場所 |
| 出力 | `Unique Mask`。岩片ごとの決定論的なランダム値 |

## パラメータ

| パラメータ | 既定値 | 内容 |
| --- | --- | --- |
| `Physics Count` | 48 | 崩落粒子を下方向へ進めるステップ数。大きいほど斜面下部へ長く流れる |
| `Debris Amount` | 65% | 発生する岩屑の量。粒子数と盛り上がりの強さに効く |
| `Debris Min Size (m)` | 2.0 | 岩片の最小直径 |
| `Debris Max Size (m)` | 8.0 | 岩片の最大直径 |
| `Rock Style` | Shard | `Classic` / `Polygonal` / `Shard` から岩片形状を選ぶ |
| `Gravity` | 75% | 低い方向へ流れる強さ。高いほど直線的に下る |
| `Seed` | 0 | 発生位置とばらつきのシード |

## 使い方

- `Mask Slope` や `Mask Curvature` で崩れやすい急斜面を作り、`Emission Mask` へ接続します。
- `Physics Count` を上げると岩屑が斜面下部や谷底へ届きやすくなります。
- `Unique Mask` を `Colorize` の `Gradient Mask` へ接続すると、岩片ごとに色を変えられます。

## メモ

- `Rock` はその場に岩を散布するノード、`Crumbling` は発生源から下方向へ動かした岩屑を堆積するノードとして使い分けます。
- 初期実装は CPU 評価です。見た目、発生密度、停止条件、堆積量、評価速度は今後の調整対象です。
