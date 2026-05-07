# Mask Fluvial ノード

入力ハイトフィールドにフロー累積を流し、川筋ネットワーク(ドレナージマップ)を `Mask` として出力する加工ノードです。GIS 標準の D8 / MFD アルゴリズムをベースにしているため、地形が同じなら出力は決定的で再現性があります。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `HeightField` |
| 出力 | `Mask` |

## 主な設定

| 設定 | 役割 |
| --- | --- |
| `Algorithm` | `D8`(最急降下方向のみに流す、細い線)/ `MFD`(複数方向に重み付き分配、面的な広がり) |
| `Output Curve` | `Log`(連続的な樹枝状ドレナージ、既定)/ `Threshold`(閾値ベースの二値川筋抽出)/ `Linear`(非対数の連続マップ) |
| `Threshold (%)` | Log/Linear ではノイズフロア(これ未満の累積はマスク 0)、Threshold モードでは「川とみなす」閾値 |
| `Gamma` | (Log/Linear) `pow(mask, gamma)`。下げると細い支流が明るくなり、上げると主流のみが残る |
| `Softness` | (Threshold) 閾値前後の smoothstep 幅。小さいほどシャープな川筋 |
| `Edge Power` | (Threshold) `pow(mask, power)` で川縁をテーパー。1 を超えると細く、1 未満で太く |
| `Pit Fill Iterations` | 局所窪みを埋める反復回数。0 で湖を残し、増やすほど排水経路が確実につながる |
| `MFD Exponent` | (MFD) 下流分配の鋭さ。大きいほど D8 寄り(主流に集中)、小さいほど面的に広がる |

## モード別の使い分け

| モード | 想定用途 | 既定の見た目 |
| --- | --- | --- |
| Log + D8 | ドレナージマップとしての可視化、樹枝状の階層表現 | 細い支流まで見える連続グラデーション |
| Threshold + D8 | 後段でテクスチャ分岐 / 河道マスクとして使うシャープな川筋抽出 | 二値寄りの細い線 |
| Log + MFD | 流域・湿地帯のヒートマップ | 連続的で面的に広がる地形依存マップ |

## アルゴリズム概要

1. **Pit Fill**: 8 近傍がすべて自分以上のセルを `min(neighbours) + ε` に持ち上げる Jacobi 反復で局所窪みを除去。境界セルは出口として扱うため変更しません。
2. **Topological sort**: 標高降順にセルインデックスを並べ替え(`std::execution::par` で並列ソート)。
3. **Flow accumulation**: 各セルは初期重み 1 を持ち、降順に処理しながら下流へ累積を加算。D8 は最急降下方向のみへ、MFD は重み `slope^p` で複数方向へ分配します。
4. **Mask 化**: `Output Curve` に応じて
   - **Log**: `pow(log(1 + max(0, accum - threshold)) / log(1 + maxAdjusted), gamma)`
   - **Threshold**: `pow(smoothstep(threshold, threshold + softness, accum), power)`
   - **Linear**: `pow((accum - threshold) / max, gamma)`

## メモ

- 出力は `Mask` 1 本のみで、ハイトフィールドのパススルーは持ちません。下流ノードへ地形を流したい場合は `Mask Fluvial` の上流ブランチを別途分岐させてください。
- `Mask Blend` の入力としても直接接続できます。マスクグラフ評価器内で `Mask Fluvial` を見つけたとき自動的にハイトフィールドパイプラインを起動して `grid.mask` を `MaskGrid` として持ち上げる仕組み。`Mask Noise` で領域マスクを作って `Multiply` で川筋を絞る、といった合成が自然に書けます。
- ノード本体を選択するだけで自動的にマスクプレビューに切り替わります(`SetPreviewNode` が「`Heightmap` 出力なし + `Mask` 出力あり」のノードでは `previewField` を Mask に設定するため)。
- 並列化箇所: Pit Fill(行並列 Jacobi)、最大値リダクション、最終マスク変換(`std::log` / `std::pow` がボトルネックなので効果大)、インデックスソート。フロー累積ループ自体は標高順依存があるため逐次のままです。Debug ビルドだと `std::execution::par` のオーバーヘッドが大きいので、体感差を見るときは Release ビルドで確認してください。
- キャッシュキーは入力ハッシュ + パラメータハッシュ。他ノードの編集や `Output Curve` 切り替えで該当ノードのみ再評価されます。
- 深い盆地を含む地形では `Pit Fill Iterations` を増やすと排水経路が安定します。逆に火口湖などを残したい場合は 0 にしてください。
