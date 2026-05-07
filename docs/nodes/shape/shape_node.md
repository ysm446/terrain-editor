# Shape ノード

基本形状からハイトフィールドを生成する入力ノードです。外部ハイトマップなしで、侵食やノイズ処理のテスト地形を作るために使います。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | なし |
| 出力 | `Heightfield` |

## 主な設定

| 設定 | 役割 |
| --- | --- |
| `Shape Type` | `Hemisphere` または `Pyramid` |
| `Scale (m)` | 地形の横幅と奥行き |
| `Relative Height (%)` | 最大高さ。実高さは `Scale × Relative Height / 100` |
| `Simulation Resolution` | 生成するハイトフィールドの内部解像度 |

## メモ

`Hemisphere` は山体や孤立峰のテストに、`Pyramid` は斜面方向や侵食の基本挙動確認に向いています。
