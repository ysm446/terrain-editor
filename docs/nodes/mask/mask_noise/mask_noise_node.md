# Mask Noise ノード

Perlin / fBM ノイズから `Mask` を生成する入力ノードです。地形の一部だけに処理をかけるための領域マスクや、見た目確認用のパターン作成に使います。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | なし |
| 出力 | `Mask` |

## 主な設定

| 設定 | 役割 |
| --- | --- |
| `Backend` | `CPU Parallel` または `GPU Compute` |
| `Seed` | ノイズパターン |
| `Octaves` | fBM の階層数 |
| `Frequency` | 基本周波数 |
| `Lacunarity` | オクターブごとの周波数倍率 |
| `Persistence` | オクターブごとの振幅倍率 |
| `Simulation Resolution` | 生成するマスク解像度 |

## メモ

Mask 系ノードはハイトフィールドを持たないため、3D プレビューでは平面上にマスク値を表示します。GPU 経路が使えない場合は CPU 版へフォールバックします。
