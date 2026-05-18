# Mask Curvature ノード

`Mask Curvature` は、入力 `Heightmap` の局所的な凹凸を検出して `Mask` を出力するノードです。

周囲を blur した高さと元の高さを比較し、周囲より高い部分を `Ridges`、周囲より低い部分を `Valleys`、両方を `Absolute` として扱います。

## 入力

- `Heightmap`: 曲率を検出する高さフィールド。

## 出力

- `Mask`: 曲率の強さを `[0, 1]` に正規化したマスク。

## パラメータ

- `Mode`: `Ridges` / `Valleys` / `Absolute` を切り替えます。
- `Largest Detail Level (m)`: 周囲平均との差分を見る前に、解析用ハイトをならす最大スケールです。小さいほど細かい凹凸、大きいほど小さな揺れを無視して広い尾根や谷を拾います。入力地形そのものは変更しません。
- `Sensitivity (m)`: この高さ差で `mask = 1` になります。小さいほど弱い曲率も明るくなります。
- `Threshold (%)`: 弱い曲率を落とす下限です。
- `Gamma`: 出力カーブです。`1` 未満で弱い曲率を明るく、`1` より大きいと強い曲率だけを強調します。

## 使いどころ

- `Snow` の積もりやすい谷や窪みの制御。
- `Colorize` で岩肌、尾根、谷底の色を分けるための `Gradient Mask` や `Mask`。
- `Debris` / `Crumbling` の発生源や堆積しやすい場所の制御。
