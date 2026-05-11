# Snow アルゴリズム入門

このメモは、`Snow` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `snow_node.md` にまとめています。

## まず何をするノードか

`Snow` は、ハイトフィールドの各セルの**傾斜角に応じて雪の積もり方を決める**ノードです。
急な斜面には雪が積もらず、なだらかな部分ほど厚く積もるという自然な挙動を、
粒子シミュレーションなしに 1 パスだけで再現します。

雪は**加算で積まれます** (`height += thickness`)。
出力を `Heightmap Blur` に通すと、雪だまりのエッジが滑らかになります。

## 全体の流れ

処理はすべて 1 回のシングルパスで完結します。

1. 処理前の高さをスナップショットとして保存する (自己参照を防ぐため)。
2. 各セルで中心差分を使って勾配 (傾斜の正接) を計算する。
3. 勾配が `Slope Limit Min` 以下ならば雪は 100% 積もる。
4. 勾配が `Slope Limit Max` 以上ならば雪は積もらない。
5. その間は smoothstep で滑らかにブレンドする。
6. 雪の厚さを高さに加算し、マスクに書き込む。

## 1. 傾斜の計算

中心差分 (4 近傍) で水平・奥行き方向の傾きを求め、勾配の大きさを計算します。

```text
dh/dx = (h[x+1, z] - h[x-1, z]) / (2 × cellSize)
dh/dz = (h[x, z+1] - h[x, z-1]) / (2 × cellSize)
slopeTan = sqrt((dh/dx)² + (dh/dz)²)
```

`slopeTan` は傾斜角の正接 (tangent) です。
`slopeTan = 1.0` は傾斜 45° に相当します。

境界セルでは隣接セルが存在しないため、端のピクセルで `clamp` して折り返します。
これは境界付近では傾斜がやや過小評価されることを意味します。

## 2. 積雪量の決定

傾斜角に基づいて snowFraction (雪の積もる割合) を smoothstep で計算します。

```text
minTan = tan(slopeLimitMinDeg × π/180)
maxTan = tan(slopeLimitMaxDeg × π/180)

t = clamp((slopeTan - minTan) / (maxTan - minTan), 0, 1)
smoothT = t² × (3 - 2t)          ← smoothstep
snowFraction = 1 - smoothT
```

| 勾配の状態 | t | smoothT | snowFraction | 意味 |
| --- | --- | --- | --- | --- |
| ≤ minTan | 0 | 0 | 1.0 | 全量積もる |
| minTan〜maxTan の中間 | 0.5 | 0.5 | 0.5 | 半量積もる |
| ≥ maxTan | 1 | 1 | 0.0 | 積もらない |

smoothstep の二次曲線 (エルミート補間) により、積雪の境界が自然なグラデーションになります。

## 3. 高さとマスクへの書き込み

```text
thickness = emissionAmount × snowFraction
height[i] += thickness
mask[i] = clamp(thickness / maskMaxSnow, 0, 1)
```

`maskMaxSnow` は「この厚さで白 (1.0)」を決める正規化基準です。
マスクは可視化チャンネルなので、後続のマスクノードへ流すことはできません。
マスクを接続したい場合は `Mask Fluvial` や `Mask Noise` を活用してください。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Emission Amount (m)` | 最大積雪量。なだらかな平地にこの厚さで積もる |
| `Slope Limit Min (deg)` | この傾斜以下では全量積もる |
| `Slope Limit Max (deg)` | この傾斜以上では積もらない |
| `Mask Max Snow (m)` | マスクが 1.0 (白) になる積雪厚さ |

## よくある使い方と設定例

### 山頂の雪線をシンプルに表現する

```
Emission Amount: 2.0 m
Slope Limit Min: 50°
Slope Limit Max: 60°
```

急崖 (60° 以上) には雪が付かず、なだらかな稜線や高原に積雪します。

### 低傾斜な台地全体を覆う雪原

```
Emission Amount: 1.0 m
Slope Limit Min: 30°
Slope Limit Max: 45°
```

谷の側壁には積もらず、平坦な頂部のみ白くなります。

## 見た目が崩れるときの考え方

### 雪の境界線がカクカクしている

`Slope Limit Min` と `Slope Limit Max` の差が狭すぎます。
最低でも 10° 程度の幅を持たせてください。

### 急崖にも雪が積もってしまう

`Slope Limit Max` が高すぎます。
典型的な岩盤の安息角は 60〜70° なので、`Slope Limit Max` をその付近に設定します。

### 積雪後にエッジが鋭く見える

`Heightmap Blur` (Radius `1.0`, Strength `20%`) を後段に追加すると、
雪と地表の境界が自然に溶け込みます。
