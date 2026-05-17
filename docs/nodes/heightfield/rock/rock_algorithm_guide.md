# Rock アルゴリズム入門

このメモは、`Rock` ノードが内部で何をしているかを段階的に理解するための読み物です。
パラメータの詳細一覧は `rock_node.md` にまとめています。

## まず何をするノードか

`Rock` は、ハイトフィールドの上に**岩を散布して高さを加算するノード**です。
岩は粒子シミュレーションではなく、グリッドセルをまたいだ**ジッター付きボロノイ散布**で配置されます。

各岩は独立した確率的な形状を持ちます。
- 大きさ / アスペクト比 / 向きのランダムバリエーション
- ドーム形状 + 多面体クリッピングで輪郭を作る
- サブボロノイによる表面のごつごつ感

出力は`加算高さ`と`マスク`です。入力地形の上に岩が積み重なります。

## 全体の流れ

1. グリッドを `density` (m/cell) で分割した散布グリッドを想定する。
2. 各セル `(gx, gz)` の中心にハッシュジッターで岩の位置を決める。
3. `Coverage` 未満のハッシュ値を持つセルはスキップする。
4. 各ピクセルに対して、影響範囲内のすべてのセルを走査し、
   最も高い岩の寄与を `bestRockH` として記録する (最大合成)。
5. 最終的に `grid.heights[i] += bestRockH`, `grid.mask[i] = bestDome`, `grid.uniqueMask[i] = bestUnique` を書き込む。
   `bestUnique` は採用された岩ごとの 0..1 ランダム値で、`Colorize` の `Gradient Mask` に使うと岩単位の色違いを作れる。

## 1. ジッター付きボロノイ散布

格子状に並んだグリッドセルに対し、セル中心を `[-0.45, 0.45]` の範囲でランダムにずらします。

```text
jx = HashFloat01(gx, gz, seed) * 0.9 - 0.45
jz = HashFloat01(gx, gz, seed + 73) * 0.9 - 0.45
rockCenter = (gx + 0.5 + jx, gz + 0.5 + jz)
```

`±0.45` に収めているのは、隣のセルと重なって破綻するのを防ぐためです。
`Coverage` パラメータは `HashFloat01(gx, gz, coverageSeed) < coverage` で岩の有無を決めます。

## 2. 岩ごとのランダマイゼーション

1 つの岩に対して複数の属性がシード分離されたハッシュから独立に決まります。

| 属性 | 式 |
| --- | --- |
| 大きさ | `lerp(minSizeCells, maxSizeCells, sizeRand)` |
| 向き (回転角) | `(rotRand - 0.5) × 2π × rotationVar` |
| アスペクト比 | `2^(aspectVar × (2 × aspectRand - 1))` |
| 高さスケール | `rockHeight × (1 - heightJitter + 2 × heightJitter × heightRand)` |

アスペクト比は対数スケールで対称になっています。
`aspectVar = 0.3` なら `2^(-0.3) ≈ 0.81` 〜 `2^(+0.3) ≈ 1.23` の範囲で変化し、
縦長と横長が等確率で現れます。

## 3. 楕円距離とドーム形状

回転させた局所座標系で楕円距離 `d_local` を計算します。

```text
rx_unrot =  ddx × cosθ + ddz × sinθ
rz_unrot = -ddx × sinθ + ddz × cosθ

rx = rx_unrot / aspect_x
rz = rz_unrot / aspect_z

d_local = sqrt(rx² + rz²)
```

`d_local` が岩の半径以下のピクセルに対して高さを計算します。

```text
radialT = clamp(1 - d_local / domeRadius, 0, 1)
```

## 4. 多面体クリッピング (Edge Sharpness)

`edgeSharpness > 0` のとき、`4〜7` 角形の不規則な多面体輪郭を生成し、岩の輪郭に適用します。

各辺は次のように決まります。

```text
facetCount = 4 + int(HashFloat01(...) * 4)    ← 4〜7 辺
baseInradius = domeRadius × cos(π / facetCount)  ← 内接円半径

各辺i: 角度 = 2π × i / facetCount + angleJitter[i]
        半径 = baseInradius × radiusJitter[i]
```

各ピクセルから各辺の内側距離を計算し、最小値を `polyhedralT` とします。
最終的なドームパラメータは次の線形合成です。

```text
t = (1 - edgeSharpness) × radialT + edgeSharpness × polyhedralT
dome = t ^ domeExp
```

`domeExp = 1 + facetSharpness × 1.5 × (1 - edgeSharpness)` は、
輪郭が丸い岩ほどなだらかに、角ばった岩ほど面が平坦になるように exponent を調整します。

## 5. 表面凹凸 (Bumpiness / Facet)

岩の局所座標系で追加のサブボロノイ場を計算し、表面の凹凸を作ります。

```text
(sub_f1, sub_f2) = VoronoiF1F2(rx × facetScale, rz × facetScale)

smoothBump = smoothstep(1 - sub_f1 / 0.5) - 0.5    ← 連続的な起伏
facetH = HashFloat01(sub_cx, sub_cz, ...) - 0.5     ← ファセットごとの高低
edgeT = clamp((sub_f2 - sub_f1) × 4, 0, 1)         ← ファセット境界の遷移

surfaceMod = (1 - facetSharpness) × smoothBump + facetSharpness × facetTerm
rockH = cellHeight × dome × (1 + bumpiness × surfaceMod)
```

`bumpiness` が大きいほど岩肌の凹凸が激しく、`facetSharpness` が高いほど平面ファセットが強調されます。

## 6. 最大合成

各ピクセルで複数の岩が重なる場合、最も高い岩だけを採用します。

```text
if rockH > bestRockH:
    bestRockH = rockH
    bestDome  = dome    ← マスクはドームのグラデーション
```

これにより岩同士が滑らかに重なり、奇妙な干渉が起きません。

## パラメータを直感で見る

| パラメータ | 直感的な役割 |
| --- | --- |
| `Density (m)` | 岩の配置格子の間隔。小さいほど密になる |
| `Coverage (%)` | 配置格子のうち何 % に岩を置くか |
| `Size Min / Max (m)` | 岩の大きさの下限・上限 |
| `Rock Height (m)` | 岩の高さスケール |
| `Height Jitter` | 高さのランダムばらつき |
| `Rotation Variation` | 向きのランダムばらつき |
| `Aspect Variation` | 縦横比のランダムばらつき |
| `Edge Sharpness` | 多面体輪郭の強さ。0 = 完全な楕円、1 = 角ばった岩 |
| `Bumpiness` | 表面の凹凸の強さ |
| `Facet Sharpness` | 平面ファセットの強調度 |
| `Facet Scale` | サブボロノイの空間スケール。小さいほどファセットが大きい |

## 見た目が崩れるときの考え方

### 岩が格子状に整列して見える

`Coverage` が 1.0 に近いと、岩の欠け間がなくなって格子パターンが目立ちます。
`Coverage` を 0.6〜0.8 に下げると自然なばらつきが生まれます。

### 岩が滑らかすぎて石に見えない

`Edge Sharpness` を 0.5〜0.8 に上げ、`Facet Sharpness` を 0.5 前後にします。

### 岩の形が均一に見える

`Aspect Variation` と `Rotation Variation` をともに上げてください。
また `Size Min` と `Size Max` の比を 1:3 以上にとると、大小のばらつきが出ます。

### パフォーマンスが遅い

`Density` が小さい (格子が細かい) と走査範囲が広がります。
まず大きめの `Density` で形を確認し、仕上げで細かくします。
`GPU Compute` バックエンドを選ぶと大幅に高速化されます。
