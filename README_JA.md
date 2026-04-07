[中文](README.md) | [English](README_EN.md) | **[日本語](README_JA.md)**

# スクリーンショット＆クリップボード管理ソフト — jietuba

## 概要

PySide6 and RUSTベースのスクリーンショットおよびクリップボード管理アプリケーションです。領域キャプチャ、ウィンドウスマート検出、GIF録画、長いスクリーンショットの結合、OCR文字認識、画像ピン留め、翻訳機能を備え、完全なクリップボード履歴管理システムを搭載しています。

---

## インストール手順

本プロジェクトは4つの自作Rustライブラリに依存しています。**プログラムを実行する前に、必ずこれらのパッケージをインストールしてください。**

### 1. Python 3.11 仮想環境の作成と有効化

```bash
python -m venv venv311
# Windows:
venv311\Scripts\activate
```

### 2. 自作Rustパッケージのインストール（必須）

```bash
pip install gifrecorder-0.2.1-cp311-cp311-win_amd64.whl
pip install longstitch-0.3.8-cp311-cp311-win_amd64.whl
pip install pyclipboard-0.3.10-cp311-cp311-win_amd64.whl
pip install windows_media_ocr-0.3.1-cp311-cp311-win_amd64.whl
```

| パッケージ名 | バージョン | 機能 |
|-------------|-----------|------|
| `gifrecorder` | 0.2.1 | GIF/動画合成エンコーダー |
| `longstitch` | 0.3.8 | 長いスクリーンショット結合アルゴリズム |
| `pyclipboard` | 0.3.10 | クリップボード低レベル操作 |
| `windows_media_ocr` | 0.3.1 | Windows Media OCR APIとoneocr.dllのラッパー |

> **注意：** これらの `.whl` ファイルは Windows x86_64 + Python 3.11 専用です。グローバルPython環境にはインストールしないでください。

### 3. Python依存パッケージのインストール

```bash
pip install PySide6==6.11.0
pip install PySide6-Fluent-Widgets==1.11.1
pip install PySideSix-Frameless-Window==0.8.1
pip install pillow==12.1.1
pip install mss==10.1.0
pip install pynput==1.8.1
pip install pywin32==311
pip install darkdetect==0.8.0
pip install emoji==2.15.0
pip install av==17.0.0
pip install colorama==0.4.6
```

**開発/ビルド依存（任意）：**

```bash
pip install pytest==9.0.2 pytest-qt==4.5.0   # テスト
pip install pyinstaller==6.17.0              # パッケージング
pip install maturin==1.12.6                  # Rustライブラリビルド
```

### 4. プログラムの実行

```bash
cd main
python main_app.py
```

---

## ディレクトリ構造

```
# プロジェクトルート
├── gifrecorder-0.2.1-cp311-cp311-win_amd64.whl       # GIF録画 Rustビルド済みパッケージ
├── longstitch-0.3.8-cp311-cp311-win_amd64.whl        # 長いスクリーンショット Rustビルド済みパッケージ
├── pyclipboard-0.3.10-cp311-cp311-win_amd64.whl      # クリップボード Rustビルド済みパッケージ
├── windows_media_ocr-0.3.1-cp311-cp311-win_amd64.whl # OCR Rustビルド済みパッケージ
│
├── main/                    # Python メインプログラム
│   ├── main_app.py          # アプリエントリポイント：システムトレイ、グローバルホットキー、ライフサイクル管理
│   ├── compile_translations.py  # 翻訳コンパイラ（.xml → .qm）
│   │
│   ├── canvas/              # キャンバスモジュール — グラフィックス編集コア
│   ├── capture/             # キャプチャモジュール — スクリーンキャプチャ＆ウィンドウ検出
│   ├── clipboard/           # クリップボードモジュール — 履歴、グループ、検索
│   ├── core/                # コアモジュール — ブートストラップ、ログ、リソース、テーマ、i18n、ホットキー
│   ├── gif/                 # GIFモジュール — 画面録画、編集、再生、エクスポート
│   ├── ocr/                 # OCRモジュール — マルチエンジン文字認識
│   ├── pin/                 # ピンモジュール — スクリーンショットピン留め、編集、OCR、翻訳
│   ├── settings/            # 設定モジュール — 統一設定管理
│   ├── stitch/              # 結合モジュール — スクロールキャプチャ、自動結合
│   ├── tools/               # ツールモジュール — ペン、矩形、矢印、テキスト等
│   ├── translation/         # 翻訳モジュール — DeepL APIサービス
│   ├── translations/        # 言語リソース — 中国語/英語/日本語
│   ├── ui/                  # UIモジュール — 共通UIコンポーネントライブラリ
│   └── tests/               # テストモジュール — ユニットテスト＆統合テスト
│
├── rust_libs/               # Rustライブラリソースコード（ソースからビルド可能）
│   ├── gifrecorder/         # GIF/動画合成エンコーダーソース
│   ├── longstitch/          # 長いスクリーンショット結合アルゴリズムソース
│   ├── pyclipboard/         # クリップボード低レベル操作ソース
│   └── windows_media_ocr/   # Windows Media OCRラッパーソース
│
└── svg/                     # SVGアイコンリソース
```

---

## モジュール詳細

### canvas/ — キャンバスモジュール

シーン管理、ビューレンダリング、アイテム選択、アンドゥ/リドゥ機能を備えたグラフィックス編集キャンバスシステム。
![jietuba_gif_20260404_000903](https://github.com/user-attachments/assets/5318b991-b0de-46a2-9c0e-d75eeae2a827)

```
canvas/
├── scene.py                 # CanvasScene — QGraphicsScene継承のキャンバスシーン
├── view.py                  # CanvasView — QGraphicsView継承のキャンバスビュー
├── selection_model.py       # SelectionModel — 選択グラフィックスアイテムの管理
├── undo.py                  # CommandUndoStack — アンドゥ/リドゥスタック
├── smart_edit_controller.py # SmartEditController — 選択/編集モード切替
├── handle_editor.py         # LayerEditor / EditHandle — コントロールポイントドラッグ編集
└── items/
    ├── drawing_items.py     # StrokeItem / RectItem / EllipseItem / ArrowItem / TextItem / NumberItem
    ├── background_item.py   # BackgroundItem — 選択領域の背景
    └── selection_item.py    # SelectionItem — 選択境界表示
```

---

### capture/ — キャプチャモジュール

スクリーンキャプチャとスマートウィンドウ検出。

```
capture/
├── capture_service.py       # CaptureService — スクリーンショットコアロジック
└── window_finder.py         # WindowFinder — スマートウィンドウ選択、カーソル位置検出
```

---

### clipboard/ — クリップボード管理モジュール

Ditto風のクリップボード履歴マネージャー。テキスト、画像、ファイル等に対応。
![jietuba_gif_20260404_001128](https://github.com/user-attachments/assets/b0a116e8-d944-43c9-b895-e6fc10d8c08a)

```
clipboard/
├── data_manager.py          # ClipboardManager / ClipboardItem / Group — DB保存＆検索
├── window.py                # ClipboardWindow — クリップボード履歴メインウインドウ
├── data_controller.py       # ClipboardController — ビジネスロジック、ショートカット、コンテキストメニュー
├── data_setting.py          # ManageDialog — グループ管理ダイアログ
├── interaction.py           # SelectionManager — リスト選択状態管理
├── item_widget.py           # ClipboardItemWidget — 履歴アイテム表示
├── item_delegate.py         # ClipboardItemDelegate — カスタムリストアイテムレンダリング
├── preview_popup.py         # PreviewPopup — 大画像/長文プレビューポップアップ
├── themes.py                # ThemeManager / Theme / ThemeColors — テーマ管理
├── theme_styles.py          # ThemeStyleGenerator — CSSスタイルシート生成
├── pin_window.py            # クリップボードアイテムからピン作成
├── emoji_data.py            # 絵文字データ管理
├── frameless_mixin.py       # FramelessMixin — フレームレスウィンドウミックスイン
└── setting_panel.py         # クリップボード設定パネル
```

---

### core/ — コアモジュール

ログ、リソースローディング、テーマ管理、国際化、ホットキー等のインフラ。

```
core/
├── bootstrap.py             # PreloadManager — 起動ブートストラップ、環境初期化、DPI、シングルインスタンス
├── logger.py                # Logger — ファイル＋コンソールログ（debug/info/warning/error/exception）
├── crash_handler.py         # install_crash_hooks() — グローバル例外キャッチ
├── resource_manager.py      # ResourceManager — SVG/画像リソースローディング
├── theme.py                 # ThemeManager — アプリテーマカラー管理
├── i18n.py                  # I18nManager / XmlTranslator / tr() — 国際化
├── shortcut_manager.py      # HotkeySystem / ShortcutManager — グローバル＆アプリ内ホットキー
├── save.py                  # SaveService — ファイル保存サービス
├── export.py                # ExportService — 画像エクスポート
├── clipboard_utils.py       # copy_image_to_clipboard() — 画像をクリップボードにコピー
├── platform_utils.py        # DPI設定、AppUserModelID、Windows APIユーティリティ
├── qt_utils.py              # safe_disconnect() — Qtシグナル安全切断
└── constants.py             # グローバル定数（フォント、パス等）
```

---

### gif/ — GIF録画モジュール

画面録画、編集、再生、GIF/動画エクスポート。
<img width="766" height="630" alt="image" src="https://github.com/user-attachments/assets/8653fffb-b419-4584-ab4b-9fe95bb9f246" />
```
gif/
├── record_window.py         # GifRecordWindow / AppState — ステートマシンコーディネーター（3層ウィンドウ）
├── overlay.py               # CaptureOverlay / OverlayMode — キャプチャオーバーレイ、領域調整
├── drawing_view.py          # GifDrawingView / GifDrawingScene — 録画中描画
├── drawing_toolbar.py       # GifDrawingToolbar — 描画ツールバー
├── record_toolbar.py        # RecordToolbar — 開始/一時停止/停止コントロール
├── frame_recorder.py        # FrameRecorder / FrameData / CursorSnapshot — フレームサンプリング
├── playback_engine.py       # PlaybackEngine / PlayState — フレーム再生＆プレビュー
├── playback_controller.py   # PlaybackController — 再生UI＆エクスポート管理
├── playback_toolbar.py      # PlaybackToolbar / RangeSlider — プログレスバー、速度調整
├── composer.py              # _ComposeWorker / ComposerProgressDialog — GIF/動画合成
├── cursor_overlay.py        # CursorOverlay — カーソルレンダリング＆クリックアニメーション
└── _widgets.py              # ClickMenuButton / svg_icon() — カスタムウィジェット
```

---

### ocr/ — OCRモジュール

マルチエンジン文字認識管理。

```
ocr/
└── ocr_manager.py           # OCRManager — Windows Media OCR + oneocr デュアルエンジン
```

- Windows Media OCR（軽量、システム内蔵）とoneocr高精度エンジン（Rust FFI経由）をサポート
- 中国語/英語/日本語認識
- シングルトンパターン、統一認識インターフェース

---

### pin/ — ピンモジュール

スクリーンショットを画面にピン留め。編集、ズーム、OCR、翻訳対応。

```
pin/
├── pin_window.py            # PinWindow — ドラッグ可能、ズーム可能、常に最前面の画像ウィンドウ
├── pin_canvas_view.py       # PinCanvasView — ピンキャンバスビュー
├── pin_canvas.py            # ピンキャンバスオブジェクト
├── pin_manager.py           # PinManager — 全ピンウィンドウ管理（シングルトン）
├── pin_toolbar.py           # PinToolbar — ピンツールバー
├── pin_controls.py          # PinControlButtons — 閉じる、編集、コピーボタン
├── pin_context_menu.py      # PinContextMenu — 右クリックメニュー
├── pin_border_overlay.py    # PinBorderOverlay — ボーダーエフェクトオーバーレイ
├── pin_ocr_manager.py       # PinOCRManager / _OCRThread — 非同期OCR認識
├── pin_shortcut.py          # PinShortcutController — 通常/編集モードショートカット
├── pin_thumbnail.py         # PinThumbnailMode — サムネイルモード
├── pin_translation.py       # PinTranslationHelper — 翻訳ヘルパー
├── pin_image_transform.py   # PinImageTransform — 回転、反転等
└── ocr_text_layer.py        # OCRTextLayer / OCRTextItem — OCRテキストレイヤー表示
```

---

### settings/ — 設定モジュール

```
settings/
└── tool_settings.py         # ToolSettingsManager / ToolSettings — ツールの色、サイズ、ホットキー設定
```

---

### stitch/ — 長いスクリーンショット結合モジュール
![jietuba_gif_20260404_001930](https://github.com/user-attachments/assets/a9720f08-5128-447d-b425-6d0640272e6a)

```
stitch/
├── jietuba_long_stitch.py           # コア結合アルゴリズム
├── jietuba_long_stitch_unified.py   # 統一結合インターフェース
├── scroll_window.py                 # ScrollCaptureWindow — スクロールキャプチャウィンドウ
└── scroll_toolbar.py                # スクロールキャプチャツールバー
```

---

### tools/ — 描画ツールモジュール

```
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
├── highlighter.py           # HighlighterTool — 蛍光ペン/モザイク
├── cursor.py                # CursorTool — カーソル/選択
├── eraser.py                # EraserTool — 消しゴム
└── cursor_manager.py        # CursorManager — カーソルスタイル管理
```

---

### translation/ — 翻訳モジュール

DeepL APIベースのテキスト翻訳。

```
translation/
├── deepl_service.py         # DeepLService / TranslationThread — 非同期DeepL API呼び出し
├── languages.py             # SupportedLanguages — DeepL対応言語リスト・言語コード
├── translation_manager.py   # TranslationManager — 翻訳ウィンドウマネージャー（シングルトン）
├── translation_dialog.py    # TranslationDialog — 翻訳結果ウィンドウ
└── ui/
    ├── dialog.py            # 翻訳ダイアログUI
    └── widgets.py           # 翻訳ウィジェット
```

---

### translations/ — 言語リソース

```
translations/
├── app_zh.xml / app_zh.qm  # 中国語
├── app_en.xml / app_en.qm  # 英語
└── app_ja.xml / app_ja.qm  # 日本語
```

`.xml` = 編集可能なソースファイル、`.qm` = Qtランタイムで読み込むコンパイル済みファイル。変更後は `compile_translations.py` を実行して再コンパイルしてください。

---

### ui/ — UIモジュール

共通UIコンポーネントライブラリ。

```
ui/
├── toolbar.py               # Toolbar / _DragHandle — ドラッグ可能なツールバー基底クラス
├── screenshot_window.py     # ScreenshotWindow — フルスクリーンキャプチャウィンドウ
├── dialogs.py               # StandardDialog — 確認、警告、情報、エラーダイアログ
├── magnifier.py             # MagnifierOverlay — ピクセルレベル拡大鏡
├── color_picker_dialog.py   # ColorPickerDialog — カスタムHSVカラーピッカー
├── color_picker_button.py   # ColorPickerButton — カラー選択ボタン
├── hotkey_edit.py           # HotkeyEdit — グローバルホットキーエディター
├── inapp_key_edit.py        # InAppKeyEdit — アプリ内ショートカットエディター
├── mask_overlay.py          # マスクオーバーレイヤー
├── base_settings_panel.py   # BaseSettingsPanel / StepperWidget — 設定パネル基底クラス
├── paint_settings_panel.py  # PaintSettingsPanel — ブラシ設定パネル
├── shape_settings_panel.py  # ShapeSettingsPanel — 形状設定パネル
├── text_settings_panel.py   # TextSettingsPanel — テキスト設定パネル
├── arrow_settings_panel.py  # ArrowSettingsPanel — 矢印設定パネル
├── number_settings_panel.py # 番号ツール設定パネル
│
├── settings_ui/             # アプリ設定ダイアログ
│   ├── dialog.py            # SettingsDialog — タブ式設定ダイアログ
│   ├── components.py        # SettingCardGroup / ToggleSwitch — 設定コンポーネント
│   ├── page_appearance.py   # 外観設定（テーマ、言語）
│   ├── page_capture.py      # キャプチャ設定
│   ├── page_clipboard.py    # クリップボード設定
│   ├── page_hotkey.py       # ホットキー設定
│   ├── page_translation.py  # 翻訳設定
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
│   ├── page4_smart_select.py # スマート選択説明ページ
│   ├── page5_translation.py # 翻訳機能説明ページ
│   └── page6_finish.py      # 完了ページ
│
└── selection_info/          # 選择情報UI
    ├── controller.py        # 選择情報コントローラー
    ├── panel.py             # 選择情報パネル（サイズ、座標）
    ├── hook_manager.py      # フックマネージャー
    ├── border_shadow.py     # 選择ボーダーシャドウエフェクト
    ├── lock_ratio.py        # アスペクト比ロック
    └── rounded_corners.py   # 角丸スクリーンショット
```

---

### tests/ — テストモジュール

```
tests/
├── conftest.py              # pytest設定＆共通フィクスチャ
├── pytest.ini               # pytest実行設定
├── run_tests.py             # テスト実行スクリプト
├── test_undo_stack.py       # アンドゥスタックテスト
├── test_selection_model.py  # 選择モデルテスト
├── test_clipboard_data.py   # クリップボードデータテスト
├── test_clipboard_themes.py # クリップボードテーマテスト
├── test_core_utils.py       # コアユーティリティテスト
├── test_crash_handler.py    # クラッシュハンドラーテスト
├── test_emoji_data.py       # 絵文字データテスト
├── test_gif_data.py         # GIFデータ構造テスト
├── test_i18n.py             # 国際化テスト
├── test_resource_manager.py # リソースマネージャーテスト
├── test_save_service.py     # 保存サービステスト
├── test_stitch_algorithm.py # 結合アルゴリズムテスト
├── test_theme_manager.py    # テーママネージャーテスト
├── test_tool_settings.py    # ツール設定テスト
└── test_tools_base.py       # ツール基底クラステスト
```

