# Mask Height アルゴリズムメモ

`Mask Height` は、ハイトマップの各セルの高さを直接参照し、標高を 0..1 のマスクに変換します。
Heightmap の値は変更せず、出力用の `mask` だけを書き換えます。

## 1. Full Range モード

`Use Full Range` が on の場合は、入力 `Heightmap` の最低標高を 0、最高標高を 1 として正規化します。

```text
minHeight = min(height[])
maxHeight = max(height[])
value = saturate((height - minHeight) / max(maxHeight - minHeight, epsilon))
```

このモードでは、地形全体の相対標高グラデーションがそのまま `Mask` になります。
`Height Min`、`Height Max`、`Feather` は使いません。

## 2. 標高帯モード

`Use Full Range` が off の場合は、指定した標高帯を取り出します。

### 標高範囲を整理する

`Height Min` と `Height Max` はメートル単位です。
読み込みや編集で Max が Min より小さくなった場合は入れ替えます。

```text
minMeters = min(Height Min, Height Max)
maxMeters = max(Height Min, Height Max)
```

### 範囲内判定を作る

`Feather` が 0 の場合は、範囲内を白、範囲外を黒にする硬いマスクになります。

```text
value = (height >= minMeters && height <= maxMeters) ? 1 : 0
```

`Feather` が 0 より大きい場合は、下端と上端の外側に smoothstep の遷移幅を作ります。

```text
lower = smoothstep(minMeters - feather, minMeters, height)
upper = 1 - smoothstep(maxMeters, maxMeters + feather, height)
value = saturate(min(lower, upper))
```

## 3. カーブと反転を適用する

どちらのモードでも、最後に `Gamma` と `Invert` を適用します。

```text
value = pow(value, 1 / gamma)
if invert:
    value = 1 - value
```

## 使い分け

- 山頂マスク: `Height Min` を高め、`Height Max` を十分高くする。
- 低地マスク: 低い標高帯を指定するか、山頂側の設定に `Invert` を使う。
- なだらかな雪線: `Feather` を広めにする。
- 硬い段差状の帯: `Feather` を 0 にする。
- 標高グラデーション: `Use Full Range` を on にして、`Colorize` の `Gradient Mask` に接続する。
