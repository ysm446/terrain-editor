# 進捗

このファイルは、完了した作業、確認したこと、残っている注意点を共有するための場所です。

## 2026-05-11

- `docs/plan/` に共有スペースを作成。
- `goals.md`、`plan.md`、`progress.md` の初期構成を追加。
- 次に取り組む候補として、入力の `Heightmap` と `Mask` から崩落した岩や石を転がして散布する `Crumbling` または `Debris` ノードを記録。
- 作りたい予定のマスク系ノードとして、`Mask Invert`、`Mask Curvature`、`Mask Levels` を記録。
- 作りたい予定のカラー系ノードとして、画面上のマウス軌道から色を拾ってグラデーション化する `Colorize` ノードを記録。
- `Colorize` はカラー系またはテクスチャー系ノードとして扱い、入力 `Heightmap` / `Mask` / `Gradient Mask`、出力 `Color Texture` を基本案にすることを記録。

## 完了したこと

- 進捗、ゴール、計画を分けて記録できる場所を用意。

## 確認したこと

- `docs/plan/` ディレクトリは既存で、中身は未作成だった。

## 残っていること

- `Crumbling` / `Debris` ノードの仕様を詰める。
- 発生条件、転がる方向、停止条件、堆積表現、出力形式を決める。
- `Mask Invert` の底/天井操作、`Mask Curvature` の曲率正規化、`Mask Levels` の level 操作仕様を決める。
- `Colorize` の色ピック対象、サンプル方法、グラデーションキー化、プレビュー方法を決める。
