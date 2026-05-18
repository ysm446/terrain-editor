# Scatter ノード

`Scatter` は、入力 `Heightmap` 上に汎用的な形状を散布し、配置用の `Mask` と個体識別用の `Unique Mask` を作るノードです。`Rock` よりもシンプルな形状を扱い、当面は植生分布のプロキシとして使うことを想定しています。

## 入力

| 入力 | 内容 |
| --- | --- |
| Heightmap | 散布形状を置く地形。 |
| Mask | 散布範囲を制限するマスク。未接続の場合は全域に散布します。 |

## 出力

| 出力 | 内容 |
| --- | --- |
| Heightmap | 入力地形に散布形状の高さを加えた地形。 |
| Mask | 散布形状の存在範囲。`Height (m)` が 0 でも出力されます。 |
| Unique Mask | 個体ごとに異なる値を持つマスク。色分けや後段のランダム化に使えます。 |

## パラメータ

| パラメータ | 内容 |
| --- | --- |
| Shape Type | `Hemisphere` または `Cone`。 |
| Seed | 散布位置と個体差のシード。 |
| Density (m) | 散布点の間隔。 |
| Coverage (%) | 散布点が実際に配置される確率。 |
| Size Min (m) / Size Max (m) | 個体の直径範囲。 |
| Height (m) | 地形に盛り上げる高さ。0 でも `Mask` / `Unique Mask` は生成されます。 |
| Height Jitter (%) | 個体ごとの高さのばらつき。 |
| Rotation Variation (%) | 個体の向きのばらつき。 |
| Aspect Variation (%) | 個体の細長さのばらつき。 |
| Ground Detail Level | 散布形状を置く底面に使う地形ディテール。`Max` は入力地形そのままです。 |
| Backend | `CPU` / `GPU`。`GPU` は `Mask` 入力なし、`Ground Detail Level = Max` の場合に D3D12 compute で評価します。 |

## メモ

- `Hemisphere` は丸い低木や樹冠の分布確認に向いています。
- `Cone` は尖った草、針葉樹の簡易 proxy、または散布密度の確認に向いています。
- 専用の `Vegetation` ノードを作る前段として、植生の分布マスク作成に使うことを想定しています。
- GPU 経路はシェーダーのコンパイルや実行に失敗した場合、自動的に CPU 経路へフォールバックします。
