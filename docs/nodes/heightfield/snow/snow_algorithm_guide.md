# Snow アルゴリズム入門

このメモは、`Snow` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `snow_node.md` にまとめています。

## まず何をするノードか

`Snow` は、ハイトフィールドの各セルの**傾斜角に応じて雪の積もり方を決める**ノードです。

`Emission Amount = 0` の場合は雪なしとして早期終了し、入力ハイトフィールドを変更せず、Snow mask も全ゼロにします。
急な斜面には雪が積もらず、なだらかな部分ほど厚く積もるという自然な挙動を、
粒子シミュレーションなしに再現します。

雪は**加算で積まれます** (`height += thickness`)。

## 全体の流れ

処理は 3 つのフェーズで構成されます。

1. **Phase 1 — 動的な厚み計算**: `Iterations Count` と `Emission Time` に従って雪を少しずつ足し、セルごとの積雪厚みを決める。
2. **Phase 2 — Envelope Smoothing**: 各ステップの雪面をならして、溝や窪みを自然に埋める。
3. **Phase 3 — 書き込み**: ならし後の厚みを高さに加算し、マスクに出力する。

`Smoothing Iterations = 0` にすると Phase 2 はスキップされ、旧来のシングルパス挙動になります。

## Phase 1 — 傾斜の計算と動的な厚み

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
stepEmission = emissionAmount / emissionIterations
thickness[i] = min(emittedSoFar, thickness[i] + stepEmission × snowFraction)
```

`Iterations Count` は積雪と安定化を何ステップ行うか、`Emission Time (%)` はそのうち何割のステップで雪を降らせ続けるかを決めます。`Emission Time = 0%` では最初に全量を置いてから安定化し、`100%` では最後まで少しずつ降らせます。

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
    radius = Largest Detail Level (m) / cellSize
    blurred[i] = separable gaussian blur of surface ← 横方向→縦方向のガウス平均
    surface[i] = min(baseHeights[i] + emittedSoFar, max(surface[i], blurred[i]))
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
| 1〜2 | 遷移域のムラが薄くなる |
| 4〜8 | 窪みや谷が深く埋まる。積雪が多い表現に (推奨) |
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
| `Iterations Count` | 雪を何ステップで積もらせて安定化するか |
| `Emission Time (%)` | Iterations Count のうち、どの割合まで雪を降らせ続けるか |
| `Slope Limit Min (deg)` | この傾斜以下では全量積もる |
| `Slope Limit Max (deg)` | この傾斜以上では積もらない |
| `Smoothing Iterations` | Envelope smoothing の反復数。0 = なし、8 = デフォルト |
| `Largest Detail Level (m)` | GeoGen Snow 相当の最大ディテール幅。4m から 512m までのプリセットから選び、隙間埋めの最大スケールを決める |
| `Mask Max Snow (m)` | マスクが 1.0 (白) になる積雪厚さ |

## よくある使い方と設定例

### 山頂の雪線をシンプルに表現する

```
Emission Amount: 2.0 m
Slope Limit Min: 50°
Slope Limit Max: 60°
Smoothing Iterations: 6
Largest Detail Level: 8 m
```

急崖 (60° 以上) には雪が付かず、なだらかな稜線や高原に積雪します。
Smoothing によって稜線と雪面の境界のムラが消えます。

### 谷が埋まった深雪の表現

```
Emission Amount: 3.0 m
Slope Limit Min: 40°
Slope Limit Max: 55°
Smoothing Iterations: 6〜8
Largest Detail Level: 16 m
```

Smoothing を多くかけると谷や窪みが積雪で埋まり、深雪らしい丸みが出ます。

### 低傾斜な台地全体を覆う雪原

```
Emission Amount: 1.0 m
Slope Limit Min: 30°
Slope Limit Max: 45°
Smoothing Iterations: 6
Largest Detail Level: 4〜8 m
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

### Emission Amount を上げると emboss 感が強くなる

現在の初期厚みは `thickness = emissionAmount × snowFraction(slope)` で決まります。
`snowFraction` は slope に強く依存するため、`Emission Amount` を上げると、緩いセルは大きく持ち上がり、急なセルはあまり持ち上がりません。
その差分が元地形の細かい傾斜差を増幅し、結果として「雪が覆った」というより、地形の凹凸が embossed されたように見えることがあります。

Phase 2 の envelope smoothing は `surface = max(surface, blurred)` なので、低い窪みは埋めますが、高く出た凸部や ridge は削りません。
そのため emission が大きいほど、残った凸部と周囲の厚み差が目立ちやすくなります。

今後の改善候補:

- slope 判定に使う高さを事前に軽く blur し、細かいノイズ状の傾斜に反応しすぎないようにする。
- `thickness` 自体を smoothing してから `baseHeights` に足し、雪の層の高周波成分を減らす。
- `max(surface, blurred)` だけでなく、雪面を一定量 blurred 側へ寄せる `Surface Smooth Strength` のような制御を追加する。
- `Slope Limit Min/Max` の幅を広げ、積もる/積もらない境界が硬く出ないようにする。

### 谷や窪みが埋まりすぎて地形の起伏が失われる

`Smoothing Iterations` が多すぎます。
1〜2 に下げてください。深い谷を保ちたい場合は 1 が目安です。

### 急崖にも雪が積もってしまう

`Slope Limit Max` が高すぎます。
典型的な岩盤の安息角は 60〜70° なので、`Slope Limit Max` をその付近に設定します。
