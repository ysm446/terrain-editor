# 進捗

このファイルは、完了した作業、確認したこと、残っている注意点を共有するための場所です。

## 2026-05-11

- `docs/plan/` に共有スペースを作成。
- `goals.md`、`plan.md`、`progress.md` の初期構成を追加。
- 次に取り組む候補として、入力の `Heightmap` と `Mask` から崩落した岩や石を転がして散布する `Crumbling` または `Debris` ノードを記録。
- 作りたい予定のマスク系ノードとして、`Mask Invert`、`Mask Curvature`、`Mask Levels` を記録。
- 作りたい予定のカラー系ノードとして、画面上のマウス軌道から色を拾ってグラデーション化する `Colorize` ノードを記録。
- `Colorize` はカラー系またはテクスチャー系ノードとして扱い、入力 `Heightmap` / `Mask` / `Gradient Mask`、出力 `Color Texture` を基本案にすることを記録。

## 2026-05-17

- `Simulation Resolution` をノード単位の設定からグローバル設定へ移し、右ペインの `設定` タブ先頭で `Simulation Resolution` / `Viewport Mesh Resolution` を並べて調整できるようにした。
- ワイヤーフレーム表示を通常の表示設定から外し、プレビュー状態やメッシュトポロジーと一緒に `Debug` タブへ移動した。
- `Debug` タブに `Draw Calls` 表示切替と、直近プレビュー描画のドローコール数、投入三角形数、ライン数、レンダーターゲットサイズ、表示メッシュ/評価済みメッシュの頂点数・三角形数を追加した。
- GPU Displacement のビューポート表示に、内部の Heightfield / Mesh データへ干渉しないハードウェアテセレーション経路を追加した。
- `設定` タブにテセレーションのオン/オフ、最小/最大係数、距離フェード範囲を追加し、`Debug` タブでテセレーション使用状況とパッチ数を確認できるようにした。
- GPU Displacement のワイヤーフレーム表示を追加し、テセレーション有効時に細分化後の三角形を確認できるようにした。
- マスクプレビューの 3D 表示で、CPU Mesh 経路も頂点 mask 値ではなく mask テクスチャーをピクセルシェーダーでサンプルするようにした。
- `Mask Curvature` ノードを追加。
- 入力 `Heightmap` から局所平均との差分を計算し、`Ridges`、`Valleys`、`Absolute` の曲率マスクを出力できるようにした。
- `Radius`、`Sensitivity (m)`、`Threshold (%)`、`Gamma` で検出スケールと出力カーブを調整できるようにした。
- `docs/nodes/mask/mask_curvature/` にノード説明とアルゴリズムメモを追加。
- `Colorize` ノードに `Base Color` 入力を追加し、`Mask` でグラデーション色を既存カラーへ合成できるようにした。
- `Mask Levels` ノードを追加し、Black Point / White Point / Gamma / Invert でマスクを整えられるようにした。
- `Mask Slope` ノードを追加し、Heightmap の傾斜角から急斜面/平地マスクを作れるようにした。
- `Rock` ノードに `Rock Style` を追加し、従来互換の `Classic`、低ポリゴン状の `Polygonal`、細長い破片状の `Shard` を選べるようにした。
- `Rock Style = Polygonal` を中央が尖る多角錐ではなく、上面が平面に切り落とされた低ポリゴン岩へ寄せた。
- `Rock` ノードに `Orientation Rule` と `Layer Count` を追加し、斜面への沿わせ方と複数散布レイヤーを制御できるようにした。

## 完了したこと

- 進捗、ゴール、計画を分けて記録できる場所を用意。

## 確認したこと

- `docs/plan/` ディレクトリは既存で、中身は未作成だった。

## 残っていること

- `Crumbling` / `Debris` ノードの仕様を詰める。
- 発生条件、転がる方向、停止条件、堆積表現、出力形式を決める。
- `Mask Invert` を単独ノードとして残すか、`Mask Levels` の `Invert` に統合した扱いにするか決める。
- `Colorize` の色ピック対象、サンプル方法、グラデーションキー化をさらに調整する。
