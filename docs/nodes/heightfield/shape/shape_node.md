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
| `Scale (m)` | グローバル `Terrain Size (m)` 内でシェープが占める横幅と奥行き |
| `Relative Height (%)` | 最大高さ。実高さは `Scale × Relative Height / 100` |
| `Simulation Resolution` | 生成するハイトフィールドの内部解像度 |

## メモ

`Hemisphere` は山体や孤立峰のテストに、`Pyramid` は斜面方向や侵食の基本挙動確認に向いています。

地形全体の縦横サイズは設定タブの `Terrain Size (m)` で決まり、`Scale (m)` がそれより小さい場合はシェープが中央に配置され、外側は高さ 0 になります。
