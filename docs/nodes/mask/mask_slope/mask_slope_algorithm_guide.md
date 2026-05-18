# Mask Slope アルゴリズムメモ

`Mask Slope` は、ハイトマップの局所勾配から傾斜角を求め、それを 0..1 のマスクに変換します。

## 1. 解析用ハイトを用意する

`Largest Detail Level (m)` が `Max` の場合は入力 height をそのまま使います。数値が指定されている場合は、そのメートル値から求めたセル半径で入力 height を box blur し、解析用ハイトだけをならします。入力地形そのものは変更しません。

## 2. 勾配を計算する

各セルで解析用ハイトの左右と上下の中央差分を取り、地形サイズからセル間隔を求めます。

```text
dhdx = (h[x+1,z] - h[x-1,z]) / (2 * cellSize)
dhdz = (h[x,z+1] - h[x,z-1]) / (2 * cellSize)
slopeTan = sqrt(dhdx^2 + dhdz^2)
```

## 3. 角度へ変換する

```text
slopeDeg = atan(slopeTan) * 180 / pi
```

`slopeDeg` は度数法の傾斜角です。0 度は平地、45 度は高さ変化と水平距離が同じ斜面です。

## 4. 範囲をマスクにする

```text
t = saturate((slopeDeg - slopeMinDeg) / max(slopeMaxDeg - slopeMinDeg, epsilon))
t = smoothstep(t)
value = pow(t, 1 / gamma)
if invert:
    value = 1 - value
```

`Slope Min` と `Slope Max` の間は smoothstep で滑らかに遷移します。差を狭くすると硬い境界になり、広くするとなだらかな境界になります。

## 使いどころ

- 急斜面マスク: `Invert` off。
- 平地マスク: `Invert` on。
- 岩肌検出: `Slope Min` を 35-45 度、`Slope Max` を 55-70 度。
- 雪や植生の制限: 平地マスクとして使い、後段の `Mask Blend` で乗算する。
