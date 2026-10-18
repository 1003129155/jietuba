[中文](README_zh-CN.md) | [English](README.md) | **[日本語](README_JA.md)**

# jietuba — Windows 向けスクリーンショット・OCR・ピン留め・翻訳・クリップボードツール

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/ci.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/ci.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Windows 版をダウンロード](https://github.com/1003129155/jietuba/releases/latest) · [ソースから実行](#source-setup) · [開発とテスト](#development)

<img width="1391" height="844" alt="21" src="https://github.com/user-attachments/assets/5eb5e82a-7a5f-4c60-a877-679d1b29cf0c" />


## 概要

jietuba は Windows 向けの無料・オープンソースのスクリーンショットツールです。領域/ウィンドウキャプチャ、スクロール（長い）スクリーンショット、注釈、OCR 文字認識、翻訳、画像のピン留め、GIF/MP4 録画、QR コード/バーコード読み取り、PDF エクスポート、そして完全なクリップボード履歴管理を備えています。すべてローカルで動作します。

MP4 領域録画はフレームレートとビットレートの指定、一時停止/再開、マウスポインター、システム音声に対応しています。一般的な 5.1/7.1 再生デバイスの音声はステレオに変換します。マイク録音は含まれません。

jietuba は C++（Qt6）で実装され、画像処理・クリップボード操作・PP-OCR エンジンなどのコアは Rust で実装しています。Windows x86_64 および ARM64 に対応。以前の Python（PySide6）実装は [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) ブランチにアーカイブ済みで、今後は更新しません。

すぐに使える Windows 版の配布パッケージと、ソースからのビルド方法を用意しています。

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

Windows x86_64 版および ARM64 版の配布パッケージは、そのまま実行できます。ランタイムや開発環境のインストールは不要です。

1. [Releases ページ](https://github.com/1003129155/jietuba/releases/latest)を開き、端末に合わせて `-x64.zip` または `-arm64.zip` で終わるファイルをダウンロードします。
2. パッケージは 2 種類あります。ZIP 全体を展開し、中の exe をダブルクリックして起動します。
   - **完全版** `jietuba_pp-…zip`：PP-OCR エンジンとモデルを同梱し、どの Windows でも OCR が使えます。`jietuba_pp.exe` と `models/` フォルダを同じ階層に置いてください。
   - **軽量版** `jietuba_lite-…zip`：PP-OCR を含まない単一 exe で、サイズが小さくなります。既定では Windows 11 の Snipping Tool に内蔵された OCR だけを使います。Snipping Tool がない PC や、11.2308 より古く「テキスト アクション」がない PC では OCR を使えません。Snipping Tool を更新するか、フル版をお使いください。exe の横に PP-OCR コンポーネントを置くと PP-OCR も使えます。[cpp/README.md](cpp/README.md#pp-ocr-组件包)（中国語）を参照してください。
3. どちらも既定では Snipping Tool の OCR（高速で対応言語が多い）を優先します。エンジンは設定の「OCR設定」で切り替えられます。
4. アプリにはデジタル署名がないため、ブラウザーからダウンロードすると Windows の警告が表示される場合があります。表示された場合は「詳細情報」をクリックし、「実行」を選択すると起動できます。

2 つの版は設定とクリップボード履歴を共有するので切り替えて使えますが、同時に起動しないでください（既定のホットキーが競合します）。

更新機能を含む最初のリリースをインストールした後、**バージョン情報 → 更新を確認 → 更新して再起動**で更新できます。対応する GitHub Release ZIP をダウンロードし、メイン EXE のみを置換して再起動します。設定、クリップボード履歴、モデルなどのファイルは保持されます。録画、書き出し、保存を終了してからインストールしてください。直前の EXE は `.jietuba-update/` に保存されます。[復元手順](rust_libs/updater/README.md)を参照してください。2.1.8 より前の Python 版はワンクリック更新で C++ 版に置き換わり、設定・クリップボード履歴・モデルはそのまま保持されます。

## コード署名ポリシー

Windows 版リリースに含まれる `jietuba_pp.exe` と `jietuba_lite.exe` は、どちらも GitHub Actions がこのリポジトリの [cpp/](cpp/) の C++ ソースから自動ビルドし、SignPath で署名します。

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- コミッター・レビュアー・承認者：[@1003129155](https://github.com/1003129155)
- 外部からの貢献は Pull Request で受け付け、メンテナーのレビュー後にのみマージします。

### プライバシー

ユーザーが明示的に要求しない限り、本プログラムはネットワーク上の他のシステムに情報を送信しません。

- スクリーンショット、OCR、クリップボード履歴はすべてこの PC 上で処理・保存されます。
- 翻訳を使うと、翻訳対象のテキストが設定で選んだ翻訳サービス（DeepL、Google、Azure、Amazon、Baidu、Volcengine、Tencent Cloud、Youdao、DeepSeek、または自分で設定した OpenAI 互換エンドポイント）に、ご自身の API キーで送信されます。そのサービスのプライバシーポリシーが適用されます。
- **バージョン情報 → 更新を確認**で更新を確認・インストールすると、リリース情報の取得とパッケージのダウンロードのために GitHub に接続します。

---

<a id="source-setup"></a>

## ソースからビルド

Visual Studio の「C++ によるデスクトップ開発」（CMake を含む）、Rust MSVC ツールチェーン、Qt 6.11 の MSVC SDK が必要です。`cpp/` で PowerShell から実行します。

```powershell
python -m venv .tools
.\.tools\Scripts\python.exe -m pip install py7zr==1.1.4
.\.tools\Scripts\python.exe scripts\install_qt.py   # Qt SDK を .sdk/ にダウンロード
.\build.cmd                                         # ビルド・テストし dist/Release に配置
```

`.\build.cmd -Static` でリリース用の軽量版単一 exe、`-Static -Full` でフル版をビルドします。静的 Qt の準備や PP-OCR コンポーネントなどの詳細は [cpp/README.md](cpp/README.md)（中国語）を参照してください。

PP-OCR モデルはリポジトリの [models/](models/) にあります（`PP-OCRv6_det_small.onnx` と `PP-OCRv6_rec_small.onnx`）。Snipping Tool OCR はローカルの Windows 11 Snipping Tool のファイルを使い、初回使用時に `%LOCALAPPDATA%\Jietuba\oneocr` へ自動コピーするため、追加のダウンロードは不要です。

### Rust 拡張パッケージ

[rust_libs/](rust_libs/) の 7 つのライブラリは、単独で使える Python 拡張パッケージとしても PyPI で公開しています。jietuba 本体は C++ から Rust コアを直接リンクしており、これらのパッケージには依存しません。PyPI 配布名と Python の import 名は次のとおりです。

| pip パッケージ名 | import 名 | バージョン | 機能 |
|------|------|------|------|
| [`j-gif`](https://pypi.org/project/j-gif/) | `gifrecorder` | 0.4.1 | GIF/動画合成エンコーダー |
| [`j-stitch`](https://pypi.org/project/j-stitch/) | `longstitch` | 0.5.1 | 長いスクリーンショット結合アルゴリズム |
| [`j-clipboard`](https://pypi.org/project/j-clipboard/) | `pyclipboard` | 0.4.7 | クリップボード操作 |
| [`j-ppocr`](https://pypi.org/project/j-ppocr/) | `ppocr_rust` | 0.3.0 | PP-OCR (PaddleOCR) ONNX 文字認識エンジン（純 Rust + ONNX Runtime、det/rec モデルが必要） |
| [`j-hdrcapture`](https://pypi.org/project/j-hdrcapture/) | `hdrcapture` | 0.1.0 | HDR 対応のデスクトップキャプチャ（DXGI Desktop Duplication + GPU トーンマッピング） |
| [`j-input`](https://pypi.org/project/j-input/) | `inputhub` | 0.1.2 | GUI スレッドを止めないグローバルなマウス・キーボードフック（ドラッグジェスチャ、サイドボタン、ホットキー、ホイール、前面ウィンドウ） |
| [`j-video`](https://pypi.org/project/j-video/) | `video_recorder` | 0.1.0 | Media Foundation H.264/MP4 録画と WASAPI システム音声 |

7 つの拡張は Windows x86_64 と ARM64 を対象に、Python `>=3.11` と `abi3-py311` を指定しています。設定は各パッケージの `pyproject.toml` と `Cargo.toml` を参照してください。

---

<a id="development"></a>

## 開発とテスト

ビルド、テスト、開発ルールは [cpp/README.md](cpp/README.md) にあります。[C++ CI](.github/workflows/cpp.yml) はすべてのプルリクエストでビルドし、デスクトップを必要としないテストを実行します。`rust_libs/` を変更したときは [Rust CI](.github/workflows/rust-wheels.yml) が Rust の単体テスト、サードパーティライセンスの再確認、wheel のビルドを行います。実際の入力を注入する、ホットキーを登録する、画面をキャプチャするデスクトップテストは開発機でのみ実行します。

リリースの流れは [build.yml](.github/workflows/build.yml) にあります。タグは `release-<VERSION ファイルのバージョン>` でなければなりません。[lite.yml](.github/workflows/lite.yml) が x64 と ARM64 の軽量版・フル版をビルドし、署名後にそれぞれ `jietuba_lite-<tag>-<arch>.zip` と `jietuba_pp-<tag>-<arch>.zip` を作成します。

### C++ 実装

2.1.8 以降の実装は C++（[cpp/](cpp/)）だけで、配布する 2 つのパッケージはどちらも単一の静的 exe としてここからビルドします。軽量版 `jietuba_lite.exe`（`build.ps1 -Static`）、フル版 `jietuba_pp.exe`（`-Static -Full`）は ONNX Runtime を内蔵し、パッケージに既定の PP-OCR モデル `models/` を含みます。旧版の更新機能はパッケージ名で厳密に照合し、zip 直下の同名 exe だけを置き換えて `models/` を残すため、パッケージ名と exe 名は変更できません。フル版の `onnxruntime.dll` は起動時に exe が `models/` へ補います。バージョン番号はリポジトリ直下の `VERSION` ファイルにだけ書きます。

Python（PySide6）版は [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) ブランチにアーカイブ済みで、今後は更新しません。C++ テストにある純ロジックのフィクスチャは当時それから生成したもので、コミット済みのため再生成しません。新機能は C++ で実装し、期待値は C++ のテストに書きます。

### Rust ライブラリの階層

`rust_libs/` のロジックは 1 つだけ書き、C++ 版と PyPI で公開する Python 拡張で共有します。

- 各ライブラリは 2 層構成です。コアは純粋な Rust で、pyo3 バインディングは既定で有効な `python` フィーチャーの下の `python.rs`（録画ライブラリは `bindings.rs`）だけに置きます。C++ 版は `default-features = false` で依存し、独自の C インターフェース（`cpp/native/`）から同じコアを呼び出します。新しい機能は `cpp/native/` につなぐだけでよく、pyo3 バインディングを書く必要はありません。
- バインディング層は型変換、GIL の取得と解放、エラーの変換、オブジェクトの寿命管理だけを行います。判断、既定値、リトライ、スレッドといったロジックはコアに書きます。
- コアの回復可能なエラーは `panic` せず `Result` で返します。
- Qt に依存しない非 UI ロジックは、まず Rust コアに書きます。Qt の型を使う UI コードは C++ に残します。

### コードコメント

コメントには、コード自体では表せない制約と設計上の理由だけを、短く正確に書きます。

- 調査の経緯、変更履歴、障害の振り返り、開発中の推論は書きません。
- 他のソフトウェアやプロジェクトに言及せず、根拠にもしません。
- 長い説明文にせず、コード、関数名、テスト名で分かることは繰り返しません。

---

## ディレクトリ構造

```text
# プロジェクトルート
├── README.md / README_zh-CN.md / README_JA.md   # 英語・中国語・日本語のドキュメント
├── VERSION                  # バージョン番号（CMake とリリースワークフローが参照）
├── 托盘.ico                 # アプリと更新機能のアイコン
├── licenses/                # 配布パッケージに同梱するサードパーティライセンス
│
├── cpp/                     # C++（Qt6）実装
│   ├── src/                 # ソース（<パッケージ>/<モジュール> 構成）
│   │   ├── barcode/         # QR コード・バーコード認識（zxing-cpp）
│   │   ├── canvas/          # キャンバス — 編集の中核、元に戻す、レイヤー
│   │   ├── capture/         # キャプチャ — スクリーンショット、ウィンドウ・要素の検出
│   │   ├── clipboard/       # クリップボード管理 — 履歴、グループ/クイック起動、インポート/エクスポート、検索
│   │   ├── core/            # コア — 起動、ログ、リソース、テーマ、国際化、ホットキー
│   │   ├── gif/             # GIF/MP4 録画、GIF 編集、再生、書き出し
│   │   ├── ocr/             # Snipping Tool OCR と PP-OCR
│   │   ├── pin/             # ピン留め — 最前面表示、編集、OCR、翻訳
│   │   ├── settings/        # 設定の一元管理
│   │   ├── stitch/          # 長いスクリーンショットの結合
│   │   ├── text_recognition/ # OCR 結果のレイアウト
│   │   ├── tools/           # 描画ツール — ペン、矩形、矢印、テキスト、モザイクなど
│   │   ├── translation/     # 複数プロバイダーの翻訳と読み上げ
│   │   ├── ui/              # UI コンポーネント、キャプチャ画面、ツールバー、設定画面
│   │   └── video/           # MP4 録画子プロセス
│   ├── native/              # rust_libs の C インターフェース（静的リンク）
│   ├── resources/           # 既定設定、UI 翻訳（XML と .qm）、ログ文言
│   ├── scripts/             # ビルド、Qt SDK の準備、翻訳のコンパイル
│   └── tests/               # QtTest のテストと照合データ
│
├── rust_libs/               # Rust ライブラリ（C++ 実装と PyPI 拡張で共有）
│   ├── gifrecorder/         # GIF/動画合成エンコーダー
│   ├── longstitch/          # 長いスクリーンショット結合アルゴリズム
│   ├── pyclipboard/         # クリップボード操作
│   ├── ppocr_rust/          # PP-OCR (PaddleOCR) ONNX 認識エンジン
│   ├── hdrcapture/          # HDR デスクトップキャプチャ
│   ├── inputhub/            # グローバルなマウス・キーボードフック
│   ├── updater/             # メイン EXE だけを置き換える独立した更新機能
│   └── video_recorder/      # Media Foundation 録画と WASAPI システム音声の取得
│
├── models/                  # PP-OCR ONNX モデル（フル版に同梱）
│   ├── PP-OCRv6_det_small.onnx   # 文字検出モデル (DBNet)
│   └── PP-OCRv6_rec_small.onnx   # 文字認識モデル (CRNN/CTC)
│
└── svg/                     # SVG アイコン
```
