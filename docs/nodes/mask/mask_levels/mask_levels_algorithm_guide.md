# Mask Levels アルゴリズムメモ

`Mask Levels` は、入力値 `v` を `[0, 1]` として次の順で変換します。

1. `Black Point` と `White Point` で入力範囲を正規化する。
2. `Gamma` で中間調カーブをかける。
3. `Invert` が有効なら `1 - value` に反転する。
4. 最終値を `[0, 1]` に clamp して出力する。

式としては概ね次の形です。

```text
value = saturate((v - blackPoint) / max(whitePoint - blackPoint, epsilon))
value = pow(value, 1 / gamma)
if invert:
    value = 1 - value
```

`Black Point` と `White Point` が逆転した場合は内部で入れ替えて扱います。
