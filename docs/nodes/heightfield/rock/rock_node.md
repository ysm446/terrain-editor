# Rock ノード

入力ハイトフィールドに「土のかぶっていない岩肌」を被せる加工ノードです。離散的に岩を散布するのではなく、地形全体を **ジッタード Voronoi タイル** で岩塊状に持ち上げ、サブセル凹凸とセル境界の亀裂で角の立った岩肌感を出します。崩壊岩のような離散配置は別ノード(Crumbling)を想定しています。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `HeightField` |
| 出力 | `Heightmap`(凹凸を加算したハイトフィールド) / `Mask`(岩らしさの 0..1 マスク) |

## 主な設定

| 設定 | 既定値 | 役割 |
| --- | --- | --- |
| `Seed` | 0 | ハッシュオフセット。同じ他パラメータでも異なる岩配置を得るため |
| `Density (m)` | 8.0 | Voronoi セル一辺の長さ。岩のおおよそのサイズに対応(小さい=細かい岩肌、大きい=巨石サイズ) |
| `Coverage (%)` | 100 | セルが岩になる確率。下げると元地形が見える隙間が増える |
| `Rock Fill (%)` | 85 | セル内のドーム半径比。1.0 でセル境界まで到達、下げると隣接ドーム間に元地形の溝が通る |
| `Rock Height (m)` | 1.5 | 岩塊の最大盛り上がり |
| `Height Jitter (%)` | 50 | 岩ごとの高さ振れ幅(0=均一、1=0×〜2×) |
| `Bumpiness (%)` | 60 | サブセル凹凸の振幅(0=滑らかドーム、1=岩肌ゴツゴツ) |
| `Crack Depth (m)` | 0.3 | Voronoi セル境界に彫る亀裂の深さ。0 で滑らか、上げると角が立つ |

## アルゴリズム概要

各地形セルの世界座標 `(x, z)` に対して:

1. **Voronoi F1 / F2 を計算**(セルピッチ = `density`、ジッタード seed)。`F1cell` は最近傍 seed の整数セル座標。
2. **セル乱数** `cellRandom = hash(F1cell, seed + 17)`。`cellRandom > coverage` ならスキップ(その地点は元地形のまま)。
3. **ドーム形状** `dome = smoothstep(1, 0, F1 / domeRadius)`。`domeRadius = rockFill * 0.5`。
4. **セル毎の高さ** `cellHeight = rockHeight × (1 - heightJitter + 2 × heightJitter × hash(F1cell))`。
5. **サブセル凹凸**: 4× 周波数の Voronoi を別 seed で評価し `-0.5..+0.5` に正規化。
6. **岩塊高さ** `rockH = cellHeight × dome × (1 + bumpiness × subDetail)`。
7. **亀裂彫り込み**: `edge = saturate(1 - (F2 - F1) / crackWidth)` で境界からの距離を取り、`rockH -= edge × crackDepth`。
8. 出力: `grid.heights[c] += rockH` / `grid.mask[c] = clamp(dome × (1 - edge), 0, 1)`。

`crackWidth = density × 0.08` 内部固定(亀裂の物理幅)。並列化は `ParallelForRows` で行並列。

## 用途の使い分け

| 目的 | パラメータの方向性 |
| --- | --- |
| 細かい岩肌(参考画像の崖風) | `density` 4-8m / `coverage` 1.0 / `rockFill` 0.8-0.9 / `bumpiness` 0.6+ / `crackDepth` 0.3+ |
| 巨石サイズの岩塊カバー | `density` 15-30m / `coverage` 0.7-1.0 / `rockHeight` 3-8m |
| 散らばった岩(離散風の擬似) | `density` 15m+ / `coverage` 0.3-0.5(ただし離散粒子配置とは別物) |
| 滑らかな丘の追加 | `bumpiness` 0 / `crackDepth` 0 / `rockHeight` 小さめ |

## メモ

- 出力は **加算**(地形がせり上がる)です。`Mask Blend` で他のマスクと合成して、特定領域だけ岩肌を出すような使い方が想定。
- `Mask` 出力は `Heightmap` 出力の **岩がはっきりある場所** を強調する重み(ドーム頂点付近で 1、亀裂・ノーカバレッジで 0)。後段のテクスチャ分岐などに使えます。
- `density` が 1024² グリッド・1024m 地形で 8m なら、約 16,384 セルの Voronoi 探索が走ります。各地形セルは 9 近傍の seed のみ検索する O(N²) なので、1024² でも数十 ms 程度。`density` を 1m 以下にすると重くなります。
- キャッシュは入力ハッシュ + パラメータハッシュで他ノードと同じく個別キャッシュ。`seed` を 1 つずらすだけでこのノードのみ再評価されます。
