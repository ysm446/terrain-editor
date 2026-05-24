# Mask Path

`Mask Path` は、`Path` ノードで作ったポイントとエッジから `Mask` 画像を生成するノードです。

## 使い方

- `Path` 出力を `Mask Path` の `Path` 入力へ接続します。
- `Mask Path` は `Mask` 出力を返します。
- 各ポイントの `Width`、`Feather`、`Intensity` がマスクに使われます。
- エッジ上では、両端ポイントの `Width`、`Feather`、`Intensity` を補間します。
- 複数のエッジやポイントが重なる場所は、最も強い値を使います。

## マスクの形

- `Width` は Path の全幅として扱います。
- 中心線から `Width / 2` までは白に近い値になります。
- その外側を `Feather` 幅で 0 へフェードします。
- `Intensity` は 0..1 の強さとして、距離から求めたマスク値に掛けられます。
- `Gamma` でフェード部分のカーブを調整できます。
- `Invert` で白黒を反転できます。

## 今後の予定

- エッジごとの上書きプロパティ。
- 曲線補間。
- Path から Heightmap を生成するノード。
