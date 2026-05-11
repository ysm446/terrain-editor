# Snow アルゴリズム入門

このメモは、`Snow` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `snow_node.md` にまとめています。

## まず何をするノードか

`Snow` は、ハイトフィールドの各セルの**傾斜角に応じて雪の積もり方を決める**ノードです。
急な斜面には雪が積もらず、なだらかな部分ほど厚く積もるという自然な挙動を、
粒子シミュレーションなしに再現します。

雪は**加算で積まれます** (`height += thickness`)。

## 全体の流れ

処理は 3 つのフェーズで構成されます。

1. **Phase 1 — 初期厚み計算**: 元の高さから傾斜を計算し、セルごとの初期積雪厚みを決める。
2. **Phase 2 — Envelope Smoothing**: 雪面を繰り返しならして、溝や窪みを自然に埋める。
3. **Phase 3 — 書き込み**: ならし後の厚みを高さに加算し、マスクに出力する。

`Smoothing Iterations = 0` にすると Phase 2 はスキップされ、旧来のシングルパス挙動になります。

## Phase 1 — 傾斜の計算と初期厚み

中心差分 (4 近傍) で水平・奥行き方向の傾きを求め、勾配の大きさを計算します。

```text
dh/dx = (h[x+1, z] - h[x-1, z]) / (2 × cellSize)
dh/dz = (h[x, z+1] - h[x, z-1]) / (2 × cellSize)
slopeTan = sqrt((dh/dx)² + (dh/dz)²)
```

`slopeTan` は傾斜角の正接 (tangent) です。
`slopeTan = 1.0` は傾斜 45° に相当します。

傾斜角に基づいて snowFraction (雪の積もる割合) を smoothstep で計算します。

```text
minTan = tan(slopeLimitMinDeg × π/180)
maxTan = tan(slopeLimitMaxDeg × π/180)

t = clamp((slopeTan - minTan) / (maxTan - minTan), 0, 1)
smoothT = t² × (3 - 2t)          ← smoothstep
snowFraction = 1 - smoothT
thickness[i] = emissionAmount × snowFraction
```

| 勾配の状態 | t | snowFraction | 意味 |
| --- | --- | --- | --- |
| ≤ minTan | 0 | 1.0 | 全量積もる |
| minTan〜maxTan の中間 | 0.5 | 0.5 | 半量積もる |
| ≥ maxTan | 1 | 0.0 | 積もらない |

smoothstep の二次曲線により、積雪の境界が自然なグラデーションになります。
傾斜計算は**元の高さのスナップショット**から行うため、厚みが近傍のセル間で干渉しません。

## Phase 2 — Snow Envelope Smoothing

Phase 1 の厚みは各セルの傾斜だけから独立に計算されるため、スロープ遷移域では
セルごとに厚みが細かくばらつくことがあります。

Envelope Smoothing はこのばらつきを物理的に意味のある方向へ補正します。
「雪は周囲より低い窪みに流れ込んで埋まる」という性質を模倣します。

```text
surface[i] = baseHeights[i] + thickness[i]   ← 雪面の高さ

各反復:
    blurred[i] = 3×3 box blur of surface      ← 近傍平均
    surface[i] = max(surface[i], blurred[i])  ← 出っ張りは保ち、窪みを埋める
```

`max` を使うことで、**周囲より高いセル (出っ張り) は変わらず、周囲より低いセル (溝の底) だけが雪で持ち上がります**。

反復後に厚みを再計算します。

```text
thickness[i] = max(0, surface[i] - baseHeights[i])
```

Jacobi 二重バッファで並列実行されるため、反復内のセル間に依存はありません。

### Smoothing Iterations の効果

| 反復数 | 効果 |
| --- | --- |
| 0 | ならしなし。スロープ遷移域に細かいムラが残ることがある |
| 1〜2 | 遷移域のムラがほぼ消え、雪面が自然に見える (推奨) |
| 4〜8 | 窪みや谷が深く埋まる。積雪が多い表現に |
| 16 | ほぼ全体が均された厚い雪の平原になる |

## Phase 3 — 高さとマスクへの書き込み

```text
heights[i] = baseHeights[i] + thickness[i]
mask[i] = clamp(thickness[i] / maskMaxSnow, 0, 1)
```

`maskMaxSnow` は「この厚さで白 (1.0)」を決める正規化基準です。
マスクは可視化チャンネルなので、後続のマスクノードへ流すことはできません。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Backend` | CpuReference = CPU 参照実装、GpuCompute = GPU Compute (高速) |
| `Emission Amount (m)` | 最大積雪量。なだらかな平地にこの厚さで積もる |
| `Slope Limit Min (deg)` | この傾斜以下では全量積もる |
| `Slope Limit Max (deg)` | この傾斜以上では積もらない |
| `Smoothing Iterations` | Envelope smoothing の反復数。0 = なし、2 = デフォルト |
| `Mask Max Snow (m)` | マスクが 1.0 (白) になる積雪厚さ |

## よくある使い方と設定例

### 山頂の雪線をシンプルに表現する

```
Emission Amount: 2.0 m
Slope Limit Min: 50°
Slope Limit Max: 60°
Smoothing Iterations: 2
```

急崖 (60° 以上) には雪が付かず、なだらかな稜線や高原に積雪します。
Smoothing によって稜線と雪面の境界のムラが消えます。

### 谷が埋まった深雪の表現

```
Emission Amount: 3.0 m
Slope Limit Min: 40°
Slope Limit Max: 55°
Smoothing Iterations: 6〜8
```

Smoothing を多くかけると谷や窪みが積雪で埋まり、深雪らしい丸みが出ます。

### 低傾斜な台地全体を覆う雪原

```
Emission Amount: 1.0 m
Slope Limit Min: 30°
Slope Limit Max: 45°
Smoothing Iterations: 2
```

谷の側壁には積もらず、平坦な頂部のみ白くなります。

## 見た目が崩れるときの考え方

### 雪の境界線がカクカクしている

`Slope Limit Min` と `Slope Limit Max` の差が狭すぎます。
最低でも 10° 程度の幅を持たせてください。
`Smoothing Iterations` を 2〜4 にするとさらに滑らかになります。

### 積雪後にスロープ遷移域がざらついて見える

Phase 1 の per-cell なばらつきが残っています。
`Smoothing Iterations` を 2 以上にすれば解消します。

### 谷や窪みが埋まりすぎて地形の起伏が失われる

`Smoothing Iterations` が多すぎます。
1〜2 に下げてください。深い谷を保ちたい場合は 1 が目安です。

### 急崖にも雪が積もってしまう

`Slope Limit Max` が高すぎます。
典型的な岩盤の安息角は 60〜70° なので、`Slope Limit Max` をその付近に設定します。
