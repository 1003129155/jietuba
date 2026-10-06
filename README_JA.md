[中文](README_zh-CN.md) | [English](README.md) | **[日本語](README_JA.md)**

# jietuba — Windows 向けスクリーンショット・OCR・ピン留め・翻訳・クリップボードツール

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/ci.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/ci.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Windows 版をダウンロード](https://github.com/1003129155/jietuba/releases/latest) · [ソースから実行](#source-setup) · [開発とテスト](#development)

<img width="1391" height="844" alt="21" src="https://github.com/user-attachments/assets/5eb5e82a-7a5f-4c60-a877-679d1b29cf0c" />


## 概要

jietuba は Windows 向けの無料・オープンソースのスクリーンショットツールです。領域/ウィンドウキャプチャ、スクロール（長い）スクリーンショット、注釈、OCR 文字認識、翻訳、画像のピン留め、GIF/MP4 録画、QR コード/バーコード読み取り、PDF エクスポート、そして完全なクリップボード履歴管理を備えています。すべてローカルで動作します。

MP4 領域録画はフレームレートとビットレートの指定、一時停止/再開、マウスポインター、システム音声に対応しています。一般的な 5.1/7.1 再生デバイスの音声はステレオに変換します。マイク録音は含まれません。

UI は PySide6、画像処理・クリップボード操作・PP-OCR エンジンは Rust で実装しています。Windows x86_64 および ARM64 に対応。

すぐに使える Windows 版の配布パッケージと、ソースからの実行方法を用意しています。

---

## 主な特長

「スクリーンショット ⇄ クリップボード ⇄ ピン留め」がなめらかにつながる三方向連携。

- **コピー元を問わず自動で履歴に**：自分で撮ったスクリーンショットだけでなく、コピーしたテキスト・画像・ファイル・HTMLはすべて同じクリップボード履歴に自動で入ります。グループ分けして永久保存でき、ワンクリックでエクスポート、複数デバイス間で共有できます

- **履歴 → ピン留め → 作業を継続**：履歴内のどの画像もワンクリックで画面最前面にピン留めできます。いつでも見比べたり、拡大縮小したり、注釈を続けたり、再OCRもできます

- **細部まで磨き込まれた機能**：スクロールキャプチャは上下左右全方向をミリ秒単位で正確に結合。マルチモニター・マルチDPI環境でもシームレスにキャプチャし、さまざまなエンコードの画像をエクスポートできます。各機能は有料ソフトに匹敵するか、それ以上です

- **メモリ使用量**：厳格なメモリ設計により、連続した複雑な操作でも常駐メモリを非常に低いレベルに抑えられます（多くの場合、わずか十数MB、場合によっては数MBレベル）

- **快適なUI**：負荷の高いシーンを最適化し、CPU使用率も低減。低スペックPCでも快適に動作します

- **データの安全性**：すべての機能が端末上だけで完結し、データ収集も裏でのネット通信もありません。データはすべてローカルに保存されます。唯一ネット接続が必要なのは翻訳機能で、これもユーザーが自分で選んだ翻訳APIを能動的に使ったときだけ通信します

---

## ダウンロードと起動

Windows x86_64 版および ARM64 版の配布パッケージは、そのまま実行できます。Python、Rust、開発環境のインストールは不要です。

1. [Releases ページ](https://github.com/1003129155/jietuba/releases/latest)を開き、端末に合わせて `-x64.zip` または `-arm64.zip` で終わるファイルをダウンロードします。
2. パッケージは 2 種類あります。ZIP 全体を展開し、中の exe をダブルクリックして起動します。
   - **完全版** `jietuba_pp-…zip`：PP-OCR エンジンとモデルを同梱し、どの Windows でも OCR が使えます。`jietuba_pp.exe` と `models/` フォルダを同じ階層に置いてください。
   - **軽量版** `jietuba_lite-…zip`：サイズが小さく、Windows 11 の Snipping Tool に内蔵された OCR だけを使います。Snipping Tool がない PC や、11.2308 より古く「テキスト アクション」がない PC では OCR を使えません。Snipping Tool を更新するか、フル版をお使いください。
3. どちらも既定では Snipping Tool の OCR（高速で対応言語が多い）を優先します。エンジンは設定の「OCR設定」で切り替えられます。
4. アプリにはデジタル署名がないため、ブラウザーからダウンロードすると Windows の警告が表示される場合があります。表示された場合は「詳細情報」をクリックし、「実行」を選択すると起動できます。

更新機能を含む最初のリリースをインストールした後、**バージョン情報 → 更新を確認 → 更新して再起動**で更新できます。対応する GitHub Release ZIP をダウンロードし、メイン EXE のみを置換して再起動します。設定、クリップボード履歴、モデルなどのファイルは保持されます。録画、書き出し、保存を終了してからインストールしてください。直前の EXE は `.jietuba-update/` に保存されます。[復元手順](rust_libs/updater/README.md)を参照してください。

---

<a id="source-setup"></a>

## ソースから実行

ビルド済み Rust パッケージを含む実行時依存パッケージは、[requirements.txt](requirements.txt) で一括インストールできます。ソースから実行するだけなら Rust ツールチェーンは不要です。

この開発ブランチの `j-video-recorder==0.1.0` は、まだ PyPI に公開されていません。以下のセットアップを行う前に、手動手順 1 で `venv311` を作成し、Python のアーキテクチャに合うローカル wheel をその環境にインストールしてください。[録画パッケージのインストール](rust_libs/video_recorder/README.md#安装预编译包)を参照してください。

### ワンクリックセットアップ

1. Windows のシステム構成に合った Python 3.11（x64 または ARM64、Python Launcher を含む）をインストールします。
2. 本リポジトリをダウンロード・展開するかクローンし、プロジェクトルートの [setup.bat](setup.bat) をダブルクリックします。

### 手動セットアップ

Windows のシステム構成に合った Python 3.11（x64 または ARM64、Python Launcher を含む）をインストールしてから、プロジェクトルートで Windows コマンドプロンプト（CMD）を開き、順に実行してください。

**1. 仮想環境の作成と有効化**

```bat
py -3.11 -m venv venv311
call venv311\Scripts\activate.bat
```

**2. 実行時依存パッケージのインストール**

```bat
python -m pip install --upgrade pip
python -m pip install -r requirements.txt
```

**3. アプリの実行**

```bat
cd main
python main_app.py
```

PP-OCR モデルの `PP-OCRv6_det_small.onnx` と `PP-OCRv6_rec_small.onnx` は、リポジトリの [models/](models/) に同梱されています。PP-OCR を使う際は、このディレクトリをそのまま保持してください。Snipping Tool の OCR は PyPI の [`oneocr`](https://pypi.org/project/oneocr/) パッケージ経由で呼び出します。必要なファイルはローカルの Windows 11 Snipping Tool から取得し、初回使用時に `%LOCALAPPDATA%\Jietuba\oneocr` へ自動でコピーするため、追加のダウンロードは不要です。

### Rust 拡張パッケージ

7 つの Python 拡張パッケージを `requirements.txt` に含めています。個別にも利用でき、ソースコードは [rust_libs/](rust_libs/) にあります。PyPI 配布名と Python の import 名は次のとおりです。

| pip パッケージ名 | import 名 | バージョン | 機能 |
|------|------|------|------|
| [`j-gif`](https://pypi.org/project/j-gif/) | `gifrecorder` | 0.4.0 | GIF/動画合成エンコーダー |
| [`j-stitch`](https://pypi.org/project/j-stitch/) | `longstitch` | 0.5.0 | 長いスクリーンショット結合アルゴリズム |
| [`j-clipboard`](https://pypi.org/project/j-clipboard/) | `pyclipboard` | 0.4.5 | クリップボード操作 |
| [`j-ppocr`](https://pypi.org/project/j-ppocr/) | `ppocr_rust` | 0.2.1 | PP-OCR (PaddleOCR) ONNX 文字認識エンジン（純 Rust + ONNX Runtime、det/rec モデルが必要） |
| [`j-hdrcapture`](https://pypi.org/project/j-hdrcapture/) | `hdrcapture` | 0.1.0 | HDR 対応のデスクトップキャプチャ（DXGI Desktop Duplication + GPU トーンマッピング） |
| [`j-input`](https://pypi.org/project/j-input/) | `inputhub` | 0.1.1 | GUI スレッドを止めないグローバルなマウス・キーボードフック（ドラッグジェスチャ、サイドボタン、ホットキー、ホイール、前面ウィンドウ） |
| `j-video-recorder` | `video_recorder` | 0.1.0 | Media Foundation H.264/MP4 録画と WASAPI システム音声 |

7 つの拡張は Windows x86_64 と ARM64 を対象に、Python `>=3.11` と `abi3-py311` を指定しています。録画も他の拡張と同じ PyO3 インターフェースを使用します。アプリは専用 Python 子プロセスで拡張を読み込み、終了時に録画リソースを解放します。設定は各パッケージの `pyproject.toml` と `Cargo.toml` を参照してください。

---

<a id="development"></a>

## 開発とテスト

仮想環境を有効にした状態で、プロジェクトルートから開発用依存パッケージをインストールし、テストを実行します。

```bat
python -m pip install -r requirements-dev.txt
python -m pytest main/tests -c main/tests/pytest.ini
```

[テストディレクトリ](main/tests/)には、キャプチャ、クリップボード、モザイク編集、ピン留め画像のズーム、GIF 再生、OCR テキストレイヤーなどのユニットテスト・統合テストがあります。[CI 設定](.github/workflows/ci.yml)では Windows x86_64 と ARM64 の Python 3.11 環境でテストとカバレッジ検査を実行し、x86_64 では静的解析も行います。実行結果は [GitHub Actions](https://github.com/1003129155/jietuba/actions/workflows/ci.yml) で確認できます。

実際にマウスとキーボードを操作するテスト（`test_quick_capture_real_hooks.py` など）は既定でスキップされます。環境変数 `RUN_REAL_INPUT_TESTS=1` を設定すると実行され、実行中はマウスとキーボードに触れないでください。実際のシステムクリップボードを書き換えるテスト（`test_clipboard_monitor_real.py`）も既定でスキップされます。環境変数 `RUN_REAL_CLIPBOARD_TESTS=1` を設定すると実行され、実行中は何もコピーしないでください。

Windows 版をビルドするには、現在の Python アーキテクチャに合う録画 wheel をインストールしてから `python build_with_ocr_onefile.py` を実行します。インストール済みの録画拡張とライセンス通知を収集し、独立した更新プログラムは Cargo でビルドします。完全版の生成物は `dist/jietuba_pp.exe` と `dist/models/` です。`--lite` を付けると軽量版の `dist_lite/jietuba_lite.exe` を生成します。録画 wheel のビルド手順は[録画 README](rust_libs/video_recorder/README.md#构建与维护)を参照してください。[自動リリースワークフロー](.github/workflows/build.yml)では、x64 と ARM64 それぞれの完全版と軽量版のアーカイブを生成します。

### コードコメント

コメントには、コード自体では表せない制約と設計上の理由だけを、短く正確に書きます。

- 調査の経緯、変更履歴、障害の振り返り、開発中の推論は書きません。
- 他のソフトウェアやプロジェクトに言及せず、根拠にもしません。
- 長い説明文にせず、コード、関数名、テスト名で分かることは繰り返しません。

---

## ディレクトリ構造

<details>
<summary>ディレクトリ構造を表示</summary>

```text
# プロジェクトルート
├── README.md / README_zh-CN.md / README_JA.md          # 英語・中国語・日本語ドキュメント
├── pyproject.toml                                      # Pythonプロジェクトのメタデータと依存関係
├── requirements.txt                                   # 実行時依存パッケージ
├── requirements-dev.txt                               # テスト・ビルド用依存パッケージ
├── build_with_ocr_onefile.py                           # PyInstaller単一ファイルビルドスクリプト（--lite で軽量版）
├── licenses/                                          # 配布パッケージに同梱するサードパーティライセンス
│
├── main/                    # Python メインプログラム
│   ├── main_app.py          # アプリエントリポイント：システムトレイ、グローバルホットキー、ライフサイクル管理
│   ├── video_helper.py      # 録画子プロセスの起動コマンドとライセンス取得
│   ├── video_worker.py      # PyO3 録画ワーカーと有界 JSONL プロトコル
│   ├── video_worker_hook.py # PyInstaller Qt hooks より前に録画ワーカーへ分岐
│   ├── compile_translations.py  # 翻訳コンパイラ（.xml → .qm）
│   ├── scripts/             # 補助スクリプト — 翻訳プロバイダー比較
│   │
│   ├── barcode/             # バーコードモジュール — QRコード/バーコード読み取り（zxing-cpp）
│   ├── canvas/              # キャンバスモジュール — グラフィックス編集コア
│   ├── capture/             # キャプチャモジュール — スクリーンキャプチャ＆ウィンドウ検出
│   ├── clipboard/           # クリップボードモジュール — 履歴、グループ/クイック起動、入出力、検索
│   ├── core/                # コアモジュール — ブートストラップ、ログ、リソース、テーマ、i18n、ホットキー
│   ├── gif/                 # GIF/MP4 録画、GIF 編集、再生、エクスポート
│   ├── ocr/                 # OCRモジュール — Snipping Tool OCR と PP-OCR による文字認識
│   ├── pin/                 # ピンモジュール — スクリーンショットピン留め、編集、OCR、翻訳
│   ├── settings/            # 設定モジュール — 統一設定管理
│   ├── stitch/              # 結合モジュール — スクロールキャプチャ、自動結合
│   ├── tools/               # ツールモジュール — ペン、矩形、矢印、テキスト、モザイク等
│   ├── translation/         # 翻訳モジュール — マルチプロバイダー翻訳サービス
│   ├── translations/        # 言語リソース — 中国語/英語/日本語/韓国語
│   ├── ui/                  # UIモジュール — 共通UIコンポーネントライブラリ
│   └── tests/               # テストモジュール — ユニットテスト＆統合テスト
│
├── rust_libs/               # Rust 拡張のソースコード
│   ├── gifrecorder/         # GIF/動画合成エンコーダーソース
│   ├── longstitch/          # 長いスクリーンショット結合アルゴリズムソース
│   ├── pyclipboard/         # クリップボード低レベル操作ソース
│   ├── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX 認識エンジンソース
│   ├── hdrcapture/          # HDR 対応デスクトップキャプチャ拡張ソース
│   ├── inputhub/            # グローバルマウス・キーボードフック拡張ソース
│   ├── updater/             # メイン EXE のみを置換する独立 Rust 更新プログラム
│   └── video_recorder/      # PyO3 Media Foundation 録画拡張
│       ├── src/             # 録画コア、PyO3 バインディングと WASAPI キャプチャ
│       ├── pyproject.toml   # j-video-recorder abi3 wheel 設定
│       └── LICENSE          # wheel に同梱する MIT ライセンス
│
├── models/                  # PP-OCR ONNX モデル（PP-OCR に必須）
│   ├── PP-OCRv6_det_small.onnx   # テキスト検出モデル (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # テキスト認識モデル (CRNN/CTC)
│
└── svg/                     # SVGアイコンリソース
```

</details>

---

## モジュール詳細

### barcode/ — バーコードモジュール

スクリーンショットの選択範囲にある QR コードやバーコードを読み取ります。結果ウィンドウでは、左側のスクリーンショット上に各コードの位置を示し、右側に番号順で内容を一覧表示します。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
barcode/
├── __init__.py
├── reader.py                # read_codes / DecodedCode — zxing-cpp でデコードし、内容・形式・輪郭を読み順で返す
└── result_window.py         # BarcodeResultWindow — 結果ウィンドウ。スクリーンショットと結果一覧を同じ番号で対応付け
```

</details>

- QR コード、Data Matrix、Aztec、PDF417 と、Code 128・EAN/UPC などの一般的な一次元バーコードに対応
- 選択範囲内の複数のコードを一度に読み取り。どちら側でコードにホバーしても両方がハイライト
- ワンクリックでコピー。http(s) リンクはブラウザで直接開ける

---

### canvas/ — キャンバスモジュール

シーン管理、ビューレンダリング、アイテム選択、アンドゥ/リドゥ機能を備えたグラフィックス編集キャンバスシステム。
![jietuba_gif_20260404_000903](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
canvas/
├── scene.py                 # CanvasScene — QGraphicsScene継承のキャンバスシーン
├── view.py                  # CanvasView — QGraphicsView継承のキャンバスビュー
├── selection_model.py       # SelectionModel — 選択グラフィックスアイテムの管理
├── undo.py                  # CommandUndoStack — アンドゥ/リドゥスタック
├── smart_edit_controller.py # SmartEditController — 選択/編集モード切替
├── smart_selection_anim.py  # SmartSelectionAnimator — ウィンドウ切り替え時の選択範囲の補間
├── handle_editor.py         # LayerEditor / EditHandle — コントロールポイントドラッグ編集
├── gestures.py              # マウスジェスチャの状態機械 — 文字の端ドラッグ、ラバーバンド選択、保留中のクリック編集
├── handle_overlay.py        # HandleOverlay — 編集ハンドル専用の合成レイヤー、シーン全体の再描画を回避
└── items/
    ├── drawing_items.py     # StrokeItem / RectItem / EllipseItem / NumberItem — DrawingItemMixin を共有する描画アイテム
    ├── background_item.py   # BackgroundItem — 選択領域の背景
    ├── mosaic_item.py       # MosaicItem — モザイクアイテム
    ├── spotlight_item.py    # SpotlightItem / SpotlightCurtain — スポットライトの穴と共有の幕
    ├── selection_item.py    # SelectionItem — 選択境界表示
    ├── arrow_item.py        # ArrowItem — 矢印アイテム、9 種類の軸・先端・輪郭のジオメトリ
    └── text_item.py         # TextItem — テキストアイテム、縁取り/影/背景と三状態の操作枠
```

</details>

---

### capture/ — キャプチャモジュール

スクリーンキャプチャとスマートウィンドウ検出。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
capture/
├── capture_service.py       # CaptureService — スクリーンショットコアロジック
├── display_watcher.py       # DisplayChangeWatcher — ディスプレイの接続・解像度・HDR が変わったらキャプチャセッションを裏で再構築
├── system_cursor.py         # SystemCursor — キャプチャ時のマウスポインターを記録し画像に描き込む
├── quick_capture_controller.py # QuickCaptureController — 修飾キー＋ドラッグ撮影とアクション実行
├── uia_element_finder.py    # UI Automation によるバックグラウンド要素検出・キャッシュ
└── window_finder.py         # WindowFinder — スマートウィンドウ選択、カーソル位置検出
```

</details>

---

### clipboard/ — クリップボード管理モジュール

Ditto風のクリップボード履歴マネージャーで、controllers・core・services・ui の4層構成に再編されています。
テキスト、画像、HTML、ファイルに対応し、グループと内容を管理する独立した3ペイン管理ウィンドウも備えています。
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
clipboard/
├── __init__.py
├── controllers/             # 制御層 — 履歴読み込み、貼り付け処理、メニュー、選択状態
│   ├── clipboard_controller.py   # ClipboardController — 読み込み、貼り付け、コンテキストメニュー
│   ├── selection_manager.py      # SelectionManager — リスト選択状態管理
│   ├── mouse_shortcut_controller.py  # クリップボード項目のマウス操作とクリック判定
│   ├── context_menu_controller.py  # ContextMenuController — コンテキストメニューのデータと動作の組み立て
│   ├── foreground_tracker.py    # ForegroundWindowTracker — 貼り付け先ウィンドウを記憶
│   ├── paste_keystroke.py       # 対象ウィンドウにフォーカスを戻してから Ctrl+V を送信
│   └── __init__.py
├── core/                    # データ層 — pyclipboard ラッパー、モデル、グループ種別
│   ├── manager.py           # ClipboardManager — 保存、監視、貼り付け API
│   ├── models.py            # ClipboardItem / Group データモデル
│   ├── enums.py             # GroupType 定義
│   ├── text_transform.py    # プレーンテキスト変換 — 「特殊貼り付け」用の副作用のない純関数群
│   └── __init__.py
├── services/                # サービス層 — file payload、グループ規則、入出力、保存ロジック
│   ├── file_payload_service.py   # file 型 JSON payload と旧形式互換
│   ├── group_service.py          # グループのアイコン・命名・削除確認補助
│   ├── import_export_service.py  # テキスト項目の CSV インポート/エクスポート
│   └── manage_dialog_service.py  # 管理ウィンドウの保存ロジック
├── ui/
│   ├── layout_scale.py           # 管理ウィンドウ共通のサイズ定数
│   ├── image_item_actions.py     # 画像項目の読み込みと保存、両ウィンドウで共用
│   ├── dialogs/
│   │   └── manage_dialog.py      # グループ・内容・入出力を扱う3ペイン管理ウィンドウ
│   ├── forms/
│   │   ├── group_form.py
│   │   ├── text_content_form.py
│   │   ├── file_content_form.py
│   │   ├── import_export_form.py
│   │   ├── group_icon_picker.py
│   │   ├── form_widgets.py
│   │   └── image_content_form.py
│   ├── menus/
│   │   ├── action_menu.py
│   │   ├── group_context_menu.py
│   │   ├── item_context_menu.py
│   │   └── submenu_position.py
│   ├── mixins/
│   │   └── frameless_mixin.py
│   ├── panels/
│   │   └── setting_panel.py
│   ├── resources/
│   │   └── emoji_data.py
│   ├── theme/
│   │   ├── themes.py
│   │   └── theme_styles.py
│   ├── widgets/
│   │   ├── group_bar.py
│   │   ├── item_delegate.py
│   │   ├── item_widget.py
│   │   ├── preview_popup.py
│   │   ├── quick_edit_popup.py
│   │   ├── manage_rows.py
│   │   └── reorder_list.py
│   └── windows/
│       ├── clipboard_window.py   # 履歴ウィンドウ、検索、プレビュー、高速貼り付け
│       └── pin_window.py         # 履歴項目からピンを作成
```

</details>

**主な機能:**
- システムクリップボードの変更を監視し、自動で履歴を保存
- テキスト、画像、HTML、ファイル等に対応
- 通常グループ、クイック起動グループ、お気に入り、検索に対応
- 独立した3ペイン管理ウィンドウでグループ・テキスト内容・ファイル内容を編集可能
- テキスト項目の CSV インポート/エクスポートに対応
- テーマ切替、ショートカットによる高速貼り付け、大画像/長文プレビューに対応
- 既定で Win+V が Windows のクリップボード履歴の代わりにこのクリップボードを開く（「クイック動作」設定でオフにすると Windows に戻る）

---

### core/ — コアモジュール

ログ、リソースローディング、テーマ管理、国際化、ホットキー等のインフラ。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
core/
├── bootstrap.py             # PreloadManager — 起動ブートストラップ、環境初期化、DPI、シングルインスタンス
├── logger.py                # Logger — ファイル＋コンソールログ（debug/info/warning/error/exception）
├── crash_handler.py         # install_crash_hooks() — グローバル例外キャッチ
├── resource_manager.py      # ResourceManager — SVG/画像リソースローディング
├── theme.py                 # ThemeManager — アプリテーマカラー管理
├── ui_scale.py              # UIScaleManager — ツールバー/パネル/ポップアップ共通の拡大率
├── i18n.py                  # I18nManager / XmlTranslator / tr() — 国際化
├── shortcut_manager.py      # HotkeySystem / ShortcutManager — グローバル＆アプリ内ホットキー
├── input_hub.py             # プロセス共通のグローバル入力：ネイティブフック（j-input）は GUI スレッド外で動き、イベントを Qt シグナルで通知
├── quick_capture_input.py    # グローバルマウスショートカットのドラッグ入力（共通入力ハブを利用）
├── last_capture_region.py   # 「前回の選択範囲を復元」用のプロセス内メモリ
├── save.py                  # SaveService — ファイル保存サービス（高品質 PDF 出力対応）
├── export.py                # ExportService — 画像エクスポート
├── clipboard_utils.py       # copy_image_to_clipboard() — 画像をクリップボードにコピー
├── platform_utils.py        # DPI設定、AppUserModelID、Windows APIユーティリティ
├── qt_utils.py              # safe_disconnect() — Qtシグナル安全切断
├── log_translations/        # 各モジュールのログ翻訳ヘルパー
├── constants.py             # グローバル定数（フォント、パス等）
├── background_tasks.py      # バックグラウンド保存の追跡
├── update_cache.py          # 更新プログラムの一時ファイルとキャッシュのクリーンアップ
├── updater_process.py       # Rust 更新プログラムとの非同期 JSONL 通信
├── update_controller.py     # 更新ダウンロードと安全な再起動の調整
├── update_checker.py        # GitHub 最新リリースの非同期取得とバージョン比較
└── ui_theme.py              # UIThemeManager — アプリ窓と Qt ネイティブ部品のライト/ダーク外観
```

</details>

---

### gif/ — GIF録画モジュール

画面録画、編集、再生、GIF/動画エクスポート。

録画ツールバーで **MP4** を選択すると、H.264 動画を直接保存できます。1–60 fps、0.5–50 Mbps の目標ビットレート、システム音声とマウスポインターを設定できます。利用可能ならハードウェアを優先し、Windows のソフトウェアエンコーダーにフォールバックします。独立プロセスから逐次保存し、Python に動画フレームを蓄積しません。停止後は MP4 の処理完了を待ちます。MP4 は GIF の編集・再生に進まず直接保存されます。現在は SDR キャプチャと既定のモノラル/ステレオ再生デバイスを使用し、マイクは含みません。

PyO3 録画拡張は、実行時依存パッケージと一緒にビルド済み `j-video-recorder==0.1.0` wheel からインストールします。ソースから実行するだけなら Rust ツールチェーンは不要です。専用 Python 子プロセスが拡張を呼び出し、GUI は制御コマンドと状態イベントだけを交換します。onefile スクリプトはインストール済み拡張とライセンス通知を収集し、再ビルドしません。[Rust 録画コンポーネントの説明](rust_libs/video_recorder/README.md)には、PyPI 公開前のローカル wheel インストール、開発者向けビルド、プロセスプロトコルと検証コマンドがあります。Media Foundation と H.264/AAC が必要で、Windows N では Media Feature Pack が必要な場合があります。FFmpeg や追加コーデックは同梱しません。
<img width="766" height="630" alt="image" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />
<details>
<summary>ディレクトリ構造を表示</summary>

```text
gif/
├── record_window.py         # GifRecordWindow / AppState — ステートマシンコーディネーター（3層ウィンドウ）
├── overlay.py               # CaptureOverlay / OverlayMode — キャプチャオーバーレイ、領域調整
├── drawing_view.py          # GifDrawingView / GifDrawingScene — 録画中描画
├── drawing_toolbar.py       # GifDrawingToolbar — 描画ツールバー
├── record_toolbar.py        # RecordToolbar — 開始/一時停止/停止コントロール
├── frame_recorder.py        # FrameRecorder / FrameData / CursorSnapshot — フレームサンプリング
├── video_recorder.py        # VideoRecorder — MP4 Python ワーカーの有界 JSONL 制御
├── video_settings.py        # RecordingOptions — 動画のフレームレート・ビットレート・音声設定
├── playback_engine.py       # PlaybackEngine / PlayState — フレーム再生＆プレビュー
├── playback_controller.py   # PlaybackController — 再生UI＆エクスポート管理
├── playback_toolbar.py      # PlaybackToolbar / RangeSlider — プログレスバー、速度調整
├── composer.py              # _ComposeWorker / ComposerProgressDialog — GIF/動画合成
├── cursor_overlay.py        # CursorOverlay — カーソルレンダリング＆クリックアニメーション
└── _widgets.py              # ClickMenuButton / svg_icon() — カスタムウィジェット
```

</details>

---

### ocr/ — OCRモジュール

文字認識管理。Windows の Snipping Tool に内蔵された OCR と PP-OCR の 2 つのエンジンに対応。

<img width="580" height="505" alt="image" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />

<details>
<summary>ディレクトリ構造を表示</summary>

```text
ocr/
├── ocr_manager.py           # OCRManager — エンジン選択・読み込み・解放、統一認識インターフェース
└── snipping_tool_ocr.py     # SnippingToolOcr — Snipping Tool の検出、OCR ファイルのコピー、余白追加と認識
```

</details>

- 設定で「自動 / Windows Snipping Tool / PP-OCR」を選択。自動では Snipping Tool があればそれを、なければ PP-OCR を使う
- Snipping Tool OCR：PyPI の oneocr パッケージ経由。高速で対応言語が多い（韓国語、ロシア語、タイ語など）。Windows 11 の Snipping Tool が必要
- PP-OCR：ppocr_rust エンジン（純 Rust + ONNX Runtime、PP-OCR det + rec）。どの Windows でも動作。完全版のみ
- 認識と解放は排他。エンジン切り替え時に旧エンジンを解放し、新エンジンは次回の認識時に読み込む
- シングルトンパターン、統一認識インターフェース

---

### pin/ — ピンモジュール

スクリーンショットを画面にピン留め。編集、ズーム、OCR、翻訳対応。

<img width="737" height="657" alt="image" src="https://github.com/user-attachments/assets/827b912c-11ac-4692-b3f6-826561957615" />

<details>
<summary>ディレクトリ構造を表示</summary>

```text
pin/
├── pin_window.py            # PinWindow — ドラッグ可能、ズーム可能、常に最前面の画像ウィンドウ
├── pin_canvas_view.py       # PinCanvasView — ピンキャンバスビュー
├── pin_canvas.py            # ピンキャンバスオブジェクト
├── pin_manager.py           # PinManager — 全ピンウィンドウ管理（シングルトン）
├── pin_toolbar.py           # PinToolbar — ピンツールバー
├── pin_controls.py          # PinControlButtons — 閉じる、編集、コピーボタン
├── pin_hover.py             # PinHoverControls — ホバーボタンとツールバーの表示判定
├── pin_context_menu.py      # PinContextMenu — 右クリックメニュー
├── pin_border_overlay.py    # PinBorderOverlay — ボーダーエフェクトオーバーレイ
├── pin_ocr_manager.py       # PinOCRManager / _OCRThread — 非同期OCR認識
├── pin_shortcut.py          # PinShortcutController — 通常/編集モードショートカット
├── pin_thumbnail.py         # PinThumbnailMode — サムネイルモード
├── pin_translation.py       # PinTranslationHelper — 翻訳ヘルパー
├── pin_image_transform.py   # PinImageTransform — 回転、反転等
└── ocr_text_layer.py        # OCRTextLayer / OCRTextItem — OCRテキストレイヤー表示
```

</details>

---

### settings/ — 設定モジュール

<details>
<summary>ディレクトリ構造を表示</summary>

```text
settings/
├── color_formats.py         # 拡大鏡のカラー形式テンプレート：描画・読み込み・保存
├── settings_transfer.py     # 設定画面の項目を JSON ファイルへエクスポート／インポート
└── tool_settings.py         # ToolSettingsManager / ToolSettings — ツールの色、サイズ、ホットキー設定
```

</details>

---

### stitch/ — 長いスクリーンショット結合モジュール
![jietuba_gif_20260404_001930](https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a)

<details>
<summary>ディレクトリ構造を表示</summary>

```text
stitch/
├── auto_scroll.py                   # AutoScroller — 自動スクロール：結合結果から歩幅を決め、末尾かマウス移動で停止
├── incremental.py                   # IncrementalStitcher — バックグラウンド結合とプレビュー縮小画像
├── jietuba_long_stitch_unified.py   # 結合インターフェース（Rust の longstitch を呼び出す）
├── scroll_window.py                 # ScrollCaptureWindow — スクロールキャプチャウィンドウ
└── scroll_toolbar.py                # スクロールキャプチャツールバー
```

</details>

---

### tools/ — 描画ツールモジュール

<details>
<summary>ディレクトリ構造を表示</summary>

```text
tools/
├── base.py                  # Tool / ToolContext — 抽象基底クラス
├── controller.py            # ToolController — ツール切替＆状態管理
├── action.py                # ActionTools — コピー、保存、キャンセルアクション
├── pen.py                   # PenTool — フリーハンド描画
├── rect.py                  # RectTool — 矩形（塗りつぶし/アウトライン）
├── ellipse.py               # EllipseTool — 楕円
├── arrow.py                 # ArrowTool — 矢印
├── text.py                  # TextTool — テキスト
├── number.py                # NumberTool — 自動インクリメント番号
├── highlighter.py           # HighlighterTool — 蛍光ペン
├── mosaic.py                # MosaicTool — モザイク
├── spotlight.py             # SpotlightTool — スポットライト（枠の外を暗くする）
├── cursor.py                # CursorTool — カーソル/選択
├── eraser.py                # EraserTool — 消しゴム
└── cursor_manager.py        # CursorManager — カーソルスタイル管理
```

</details>

---

### translation/ — 翻訳モジュール

DeepL / Google / Azure / Amazon などに対応したマルチプロバイダー翻訳サービス。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
translation/
├── provider.py              # TranslationProvider / ProviderMetadata — プロバイダー抽象契約
├── registry.py              # ProviderRegistry — プロバイダー登録とファクトリ
├── service.py               # TranslationService — プロバイダー選択と翻訳オーケストレーション
├── models.py                # TranslationRequest / TranslationResult — プロバイダー非依存モデル
├── worker.py                # TranslationWorker — 全プロバイダー共通の Qt ワーカー
├── providers/               # 各翻訳プロバイダー実装
│   ├── deepl.py             # DeepL
│   ├── google.py            # Google
│   ├── azure.py             # Azure
│   ├── amazon.py            # Amazon
│   ├── baidu.py             # Baidu
│   ├── custom_llm.py        # カスタム OpenAI 互換サービス（Ollama、LM Studio など）
│   ├── deepseek.py          # DeepSeek (LLM)
│   └── openai_compatible.py # OpenAI 兼容接口基类
├── smart_translation_controller.py # SmartTranslationController — ワンキー選択テキスト検出＆ポップアップルーティング
├── translation_popup.py     # TranslationPopup — コンパクト翻訳ポップアップ（選択テキスト/手入力）
├── deepl_service.py         # DeepLService / TranslationThread — 旧版 DeepL API 非同期翻訳
├── languages.py             # SupportedLanguages — 対応言語リスト・言語コード
├── translation_manager.py   # TranslationManager — 翻訳ウィンドウマネージャー（シングルトン）
├── translation_dialog.py    # TranslationDialog — 翻訳結果ウィンドウ
└── ui/
    ├── dialog.py            # 翻訳ダイアログUI
    └── widgets.py           # 翻訳ウィジェット
```

</details>

**主な機能:**
- プラグイン可能なマルチプロバイダー構成（DeepL / Google / Azure / Amazon）をレジストリで一元管理
- ワンキー：選択テキストを自動検出して翻訳ポップアップへ
- 選択テキスト／手入力の2モード対応コンパクトポップアップ
- 非同期翻訳でUIをブロックしない
- 翻訳結果のコピーに対応

---

### translations/ — 言語リソース

<details>
<summary>ディレクトリ構造を表示</summary>

```text
translations/
├── app_zh.xml / app_en.xml  # 中国語 / 英語ソースファイル
├── app_ja.xml / app_ko.xml  # 日本語 / 韓国語ソースファイル
└── app_*.qm                 # コンパイル済み Qt バイナリ（app_zh.qm 等）
```

</details>

`.xml` = 編集可能なソースファイル、`.qm` = Qtランタイムで読み込むコンパイル済みファイル。UI テキストを追加・変更したら、4 つの `.xml` すべてに同じコンテキストのエントリを追加し（複数行の文言は実際の改行で書く）、`compile_translations.py` を実行して再コンパイルしてください。`tests/test_translation_coverage.py` が漏れをチェックします。

---

### ui/ — UIモジュール

共通UIコンポーネントライブラリ。

<details>
<summary>ディレクトリ構造を表示</summary>

```text
ui/
├── toolbar.py               # Toolbar / _DragHandle — ドラッグ可能なツールバー基底クラス
├── toolbar_layout.py        # スクリーンショットツールバーのボタン配置（順序・表示方法）の正規化と読み書き
├── toolbar_layout_dialog.py # ToolbarLayoutDialog — スクリーンショットツールバーの配置編集ダイアログ
├── reorderable_rows.py      # DragGrip / DraggableRow / ReorderableRowList — ドラッグで並べ替えられる行リスト
├── tray_menu.py             # TrayMenu — システムトレイメニュー
├── screenshot_window.py     # ScreenshotWindow — フルスクリーンキャプチャウィンドウ
├── quick_capture_overlay.py # 通常キャプチャの選択枠・座標・拡大鏡を再利用する透明レイヤー
├── update_dialog.py         # 更新内容とダウンロード進捗
├── dialogs.py               # StandardDialog — 確認、警告、情報、エラーダイアログ
├── toast.py                 # Toast — カーソル横に出る、フォーカスを奪わない一行通知
├── magnifier.py             # MagnifierOverlay — ピクセルレベル拡大鏡
├── color_picker_dialog.py   # ColorPickerDialog — カスタムHSVカラーピッカー
├── color_picker_button.py   # ColorPickerButton — カラー選択ボタン
├── hotkey_edit.py           # HotkeyEdit — グローバルホットキーエディター
├── inapp_key_edit.py        # InAppKeyEdit — アプリ内ショートカットエディター
├── key_chip.py              # KeyChipLineEdit / StatusIcon — ショートカットのキー表示と状態アイコン
├── mask_overlay.py          # マスクオーバーレイヤー
├── selection_overlay.py     # SelectionOverlayWidget — マスク上に重なる選択装飾レイヤー
├── base_settings_panel.py   # BaseSettingsPanel / StepperWidget — 設定パネル基底クラス
├── paint_settings_panel.py  # PaintSettingsPanel — ブラシ設定パネル
├── shape_settings_panel.py  # ShapeSettingsPanel — 形状設定パネル
├── text_settings_panel.py   # TextSettingsPanel — テキスト設定パネル
├── arrow_settings_panel.py  # ArrowSettingsPanel — 矢印設定パネル
├── number_settings_panel.py # 番号ツール設定パネル
├── mosaic_settings_panel.py # モザイクツール設定パネル
│
├── fluent_lite/             # Fluent スタイル軽量コンポーネントライブラリ
│   ├── buttons.py / cards.py / icons.py / inputs.py  # ボタン、カード、アイコン、入力欄
│   ├── labels.py / navigation.py / segmented.py      # ラベル、ナビゲーション、セグメントコントロール
│   ├── switch.py / theme.py / titlebar.py            # スイッチ、テーマ、タイトルバー
│   ├── frameless.py         # フレームレスウィンドウ
│   └── text_context_menu.py # テキスト右クリックメニュー
│
├── settings_ui/             # アプリ設定ダイアログ
│   ├── dialog.py            # SettingsDialog — タブ式設定ダイアログ
│   ├── components.py        # SettingCardGroup / ToggleSwitch — 設定コンポーネント
│   ├── page_appearance.py   # 外観設定（テーマ、言語）
│   ├── page_capture.py      # キャプチャ設定
│   ├── page_quick_actions.py # クイック動作設定（確認・結果ウィンドウの省略、Win+V の引き継ぎ）
│   ├── color_format_dialog.py # ColorFormatDialog — 拡大鏡のカラー形式を管理するダイアログ
│   ├── page_clipboard.py    # クリップボード設定
│   ├── page_hotkey.py       # ホットキー設定
│   ├── page_mouse.py        # マウスショートカット設定
│   ├── page_translation.py  # 翻訳設定
│   ├── provider_fields.py   # 服务商字段的读写（按声明）
│   ├── page_log.py          # ログ設定
│   ├── page_developer.py    # 開発者設定
│   ├── page_misc.py         # その他設定
│   ├── page_about.py        # アバウトページ
│   └── mock_config.py       # MockConfig — テスト用モック設定
│
├── welcome/                 # 初回起動ウェルカムウィザード（6ページガイド）
│   ├── wizard.py            # WelcomeWizard — ウィザードメインウィンドウ
│   ├── base_page.py         # BasePage — ウィザードページ基底クラス
│   ├── page1_welcome.py     # ウェルカムページ
│   ├── page2_screenshot.py  # スクリーンショットホットキー設定ページ
│   ├── page3_clipboard.py   # クリップボードホットキー設定ページ
│   ├── page5_translation.py # 翻訳機能説明ページ
│   ├── page6_finish.py      # 完了ページ
│   └── page_hotkeys.py      # グローバルホットキーページ — 6 つのキーは同一の衝突領域、まとめて設定・重複判定
│
└── selection_info/          # 選択情報UI
    ├── controller.py        # 選択情報コントローラー
    ├── panel.py             # 選択情報パネル（サイズ、座標）
    ├── hook_manager.py      # フックマネージャー
    ├── border_shadow.py     # 選択ボーダーシャドウエフェクト
    ├── lock_ratio.py        # アスペクト比ロック
    └── rounded_corners.py   # 角丸スクリーンショット
```

</details>

---

### tests/ — テストモジュール

<details>
<summary>ディレクトリ構造を表示</summary>

```text
tests/
├── conftest.py              # pytest設定＆共通フィクスチャ
├── pytest.ini               # pytest実行設定
├── run_tests.py             # テスト実行スクリプト
├── test_drawing_tools.py    # 描画ツールテスト（ペン/矩形/矢印/テキスト/番号/蛍光ペン）
├── test_mosaic_tool.py      # モザイクツールテスト
├── test_spotlight.py        # スポットライトテスト
├── test_functional_handles.py # コントロールポイント編集テスト
├── test_capture_service.py  # キャプチャサービステスト
├── test_clipboard_api.py    # クリップボード公開 API テスト
├── test_clipboard_manage_dialog.py # クリップボード管理ウィンドウテスト
├── test_frame_recorder.py   # GIFフレーム録画テスト
├── test_video_helper.py     # ワーカー起動コマンドとライセンス取得
├── test_video_worker.py     # PyO3 アダプター・プロトコル・起動時分岐テスト
├── test_video_settings.py   # 動画設定の検証・保存テスト
├── test_video_recorder.py   # MP4 プロセス制御・録画ウィンドウテスト
├── test_video_native_process.py # 現在のソースから生成した録画プロトコルテスト
├── test_video_native_binding.py # PyO3 スレッド制御・コールバック・リソース解放テスト
├── test_video_quit.py       # 録画中のアプリ終了調整テスト
├── test_playback_engine.py  # GIF再生エンジンテスト
├── test_ocr_text_layer.py   # OCRテキストレイヤーテスト
├── test_pin_window_zoom.py  # ピンウィンドウズームテスト
├── test_smart_translation.py # スマート翻訳テスト
├── test_translation_architecture.py # 翻訳プロバイダーアーキテクチャテスト
├── test_settings_dialog_state.py # 設定ダイアログ状態テスト
├── test_welcome_translation.py # ウェルカムウィザード翻訳ページテスト
└── …（その他 70 以上のユニット/統合テストファイル）
```

</details>
