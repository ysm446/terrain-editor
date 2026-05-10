# Mask Fluvial ノード

入力ハイトフィールドにフロー累積を流し、川筋ネットワーク(ドレナージマップ)を `Mask` として出力する加工ノードです。GIS 標準の D8 / MFD アルゴリズムをベースにしているため、地形が同じなら出力は決定的で再現性があります。

## 入出力

| 種類 | 内容 |
| --- | --- |
| 入力 | `Heightmap` |
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
| `Inertia (%)` | 受信ウェイト計算時に「3×3 Sobel で平滑化された下流方向」へのバイアスを混ぜる係数。0%(既定): 完全にローカル最急降下 — グリッド整列のジグザグ川。30-70%: 滑らかに蛇行する川。100%: 平滑化下流方向に強く従う。 |
| `Backend` | `CPU`(sort + 降順トポロジカル走査の厳密実装) / `GPU`(Jacobi 反復ゲザーの近似実装、視覚的同等)。既定 `GPU`。詳細は下の「GPU Compute バックエンド」節 |

## モード別の使い分け

| モード | 想定用途 | 既定の見た目 |
| --- | --- | --- |
| Log + D8 | ドレナージマップとしての可視化、樹枝状の階層表現 | 細い支流まで見える連続グラデーション |
| Threshold + D8 | 後段でテクスチャ分岐 / 河道マスクとして使うシャープな川筋抽出 | 二値寄りの細い線 |
| Log + MFD | 流域・湿地帯のヒートマップ | 連続的で面的に広がる地形依存マップ |

## アルゴリズム概要

1. **Pit Fill**: 8 近傍がすべて自分以上のセルを `min(neighbours) + ε` に持ち上げる Jacobi 反復で局所窪みを除去。境界セルは出口として扱うため変更しません。
2. **Topological sort**: 標高降順にセルインデックスを並べ替え(`std::execution::par` で並列ソート)。
3. **Flow accumulation**: 各セルは初期重み 1 を持ち、降順に処理しながら下流へ累積を加算。D8 は最急降下方向のみへ、MFD は重み `slope^p` で複数方向へ分配します。`Inertia` > 0 のときは、各方向の整合度 `align[k] = (1 - inertia) + inertia × max(0, dot(dir_k, downhill_smoothed))` を計算し (`downhill_smoothed` は 3×3 Sobel で平滑化した下流方向の単位ベクトル)、D8 では `score = slope × align`、MFD では `weight = pow(slope × align, p)` で重み付けします (MFD では align も slope と同じ指数に乗せるのが重要 — そうしないと p=4 では `slope^4` が支配的で align のシフトがほぼ見えない)。これにより川がローカル最急降下のジグザグから「広域の下流方向」に従って滑らかに蛇行するようになります。GeoGen の particle inertia とは別物 (粒子状態を持たない) ですが、視覚的には同様の「滑らかに曲がる川」を狙ったヒューリスティックです。
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

## GPU Compute バックエンド

`Backend` プルダウンで `GPU` を選ぶと [shaders/mask_fluvial_compute.hlsl](../../../../shaders/mask_fluvial_compute.hlsl) の compute shader 群で評価します。CPU 側の sort + 降順トポロジカル走査は本質的に逐次なので GPU 直接移植できないため、**Jacobi 反復ゲザー (iterative scatter via gather)** という別アルゴリズムで置き換えています。

**5 段パイプライン:**

1. `CSCopyInputHeights` — InputHeights → Heights buffer (UAV)
2. `CSPitFillJacobi` + `CSCommitHeights` — `pitFillIterations` 回 Jacobi 二重バッファ
3. `CSComputeWeights` — D8 / MFD 各セルの 8 方向受信ウェイトを 1 回計算
4. `CSAccumIter` — Jacobi 反復ゲザー (各セル: `total = 1 + Σ accum_prev[n] * weight[n→c]`)。`accumDirection` フラグで AccumA / AccumB を ping-pong しつつ **2 × resolution** 回ループ
5. `CSMaxReduce` (Log/Linear のみ、`InterlockedMax` で最大値集約) → `CSToMaskLog` / `CSToMaskLinear` / `CSToMaskThreshold`

**バッファ構成 (8 UAV):**

| スロット | 用途 |
| --- | --- |
| `u0` | Heights (pit-fill 後の作業ハイト) |
| `u1` | HeightsScratch (pit-fill 二重バッファ) |
| `u2` | Weights (8 floats / cell, 受信側 ping-pong 不要のため固定) |
| `u3` / `u4` | AccumA / AccumB (Jacobi 反復で交互に read/write) |
| `u5` | OutMask |
| `u6` | MaxScratch (`uint`、`InterlockedMax` で最大値集約。マスク変換時に `asfloat` で読む) |
| `u7` | InputHeights (CPU からのアップロード先) |

**収束について:**

Jacobi 反復は「自身の起源 → 1 セルずつ下流へ伝播」する性質を持つため、収束に必要な反復回数は ≈ **最長流路長**。1024² 解像度の地形では概ね 1024-2048 回のループでほぼ収束します。本実装は安全側で `2 × resolution` 反復に固定しています。

**CPU 結果との差:**

CPU は累積を sort 順に逐次更新するため浮動小数の加算順序が一意 (= 完全に決定論的)。GPU は各反復で全セルを並列ゲザーするため、累積の加算順序が異なります。結果として:

- 視覚的なドレナージ模様 (どの川が太く / どこに支流が分岐するか) は CPU と同等
- 個別セルの累積値は微小にずれる (浮動小数の累積順序差)
- 反復不足の場合、最下流の累積値が小さめに出る (浅い色で表示される)

数値精度を要求するパイプライン後段がある場合は CPU バックエンドを使ってください。視覚的なマスク用途 (河川描画 / 浸食マスク / 表示) には GPU で問題ありません。

**性能:**

1024² 既定パラメータでおおよそ **CPU 比 5-10 倍高速** (CPU ~100ms → GPU ~10-20ms 程度の見込み)。Pit fill / max reduce / mask conversion 部分は単発 dispatch なので軽く、ボトルネックは `2 × resolution` 回の Accum Iter ループです。

**フォールバック:**

シェーダーコンパイル / ディスパッチ失敗時は CPU 実装に自動フォールバックします。
