# AGENTS.md

**ルール:** 各コマンドでは **定義 → 使用** の順にする。`$` はエスケープしない。パス例には汎用的な `'path/to/file.ext'` を使う。

---

## 作業開始時の確認

このプロジェクトで作業を始める前に、まず以下を確認する。

1. `docs/plan/goals.md`
   - プロジェクトの目的、完成形、重視する価値を把握する。

2. `docs/plan/plan.md`
   - 実装方針、優先順位、今後の予定を把握する。

3. `docs/plan/progress.md`
   - 現在の進捗、完了済み作業、未完了作業、注意点を把握する。

そのうえで、今回の依頼が現在の計画や進捗のどこに関係するかを把握してから作業する。作業内容がこれらの方針と矛盾しそうな場合は、実装前に確認する。

---

## バージョン管理

- ユーザーから明示的にコミットを指示されるまで、変更をコミットしない。
- プロジェクトのバージョンは `CMakeLists.txt` の `project(RoadEditor VERSION x.y.z ...)` で定義する。
- `0.1.0` から開始し、ビルド上で識別したいユーザー向け変更を行ったらバージョンを上げる。
- 小さな修正はパッチ、機能追加はマイナー、破壊的なプロジェクト/データ変更はメジャーの更新を優先する。
- パッチ番号やマイナー番号は `0.1.10`、`0.1.23` のように二けた以上になってもよい。
- ソースファイルに手書きの重複バージョン文字列を置かない。CMake が生成する `Version.h` を使う。
- ユーザー向け変更は、対応するバージョン見出しの下に `docs/changelog.md` へ記録する。
- `docs/changelog.md` は日本語で書く。
- 未確定の変更は先頭の `## 未リリース` セクションに書き、バージョンを上げるときにリリース済みの見出しへ移す。
- バージョン見出しには `YYYY-MM-DD HH:MM` 形式の日付と時刻を含める。例: `## 0.1.1 - 2026-04-26 09:30`

---

## ビルド確認

- 基本のビルド確認は Debug 構成で行う。通常は `cmake --build build --config Debug` を使う。
- Release 構成での確認は、パフォーマンス、配布物、最適化時の挙動確認が必要な場合に限って行う。

---

## UI 実装

- ボタン、プルダウン、入力欄などの UI 要素は、必要がない限り親領域の幅いっぱいに伸ばさない。内容に合う固定幅、既存 UI と揃う幅、または自然な最小幅を優先する。

---

## 1) 読み取り (UTF-8 BOM なし、行番号付き)

```bash
bash -lc 'powershell -NoLogo -Command "
$OutputEncoding = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false);
Set-Location -LiteralPath (Convert-Path .);
function Get-Lines { param([string]$Path,[int]$Skip=0,[int]$First=40)
  $enc=[Text.UTF8Encoding]::new($false)
  $text=[IO.File]::ReadAllText($Path,$enc)
  if($text.Length -gt 0 -and $text[0] -eq [char]0xFEFF){ $text=$text.Substring(1) }
  $ls=$text -split \"`r?`n\"
  for($i=$Skip; $i -lt [Math]::Min($Skip+$First,$ls.Length); $i++){ \"{0:D4}: {1}\" -f ($i+1), $ls[$i] }
}
Get-Lines -Path \"path/to/file.ext\" -First 120 -Skip 0
"'
```

---

## 2) 書き込み (UTF-8 BOM なし、アトミック置換、バックアップ)

```bash
bash -lc 'powershell -NoLogo -Command "
$OutputEncoding = [Console]::OutputEncoding = [Text.UTF8Encoding]::new($false);
Set-Location -LiteralPath (Convert-Path .);
function Write-Utf8NoBom { param([string]$Path,[string]$Content)
  $dir = Split-Path -Parent $Path
  if (-not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
  }
  $tmp = [IO.Path]::GetTempFileName()
  try {
    $enc = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($tmp,$Content,$enc)
    Move-Item $tmp $Path -Force
  }
  finally {
    if (Test-Path $tmp) {
      Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    }
  }
}
$file = "path/to/your_file.ext"
$enc  = [Text.UTF8Encoding]::new($false)
$old  = (Test-Path $file) ? ([IO.File]::ReadAllText($file,$enc)) : ''
Write-Utf8NoBom -Path $file -Content ($old+"`nYOUR_TEXT_HERE`n")
"'
```
