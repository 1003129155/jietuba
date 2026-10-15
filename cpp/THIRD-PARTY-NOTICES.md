# Third-party components

This is the C++ lite build of Jietuba, Copyright (c) 2025 JYAARU, under the MIT license in LICENSE.

The build script copies these license texts into licenses/ beside the executable:

- The Snipping Tool OCR binding (`src/ocr/snipping_tool_ocr.cpp`) follows the oneocr package's interface to the Windows Snipping Tool engine, and the engine's model key is taken at build time from that package's `oneocr.py` (https://github.com/AuroraWright/oneocr, 1.0.12): MIT, `oneocr-LICENSE.txt`. The engine files belong to Microsoft; they are copied from the user's own Snipping Tool installation at run time and are not redistributed.
- zxing-cpp 3.1.1, linked statically: Apache-2.0, `zxing-cpp-LICENSE.txt`. Its bundled libzueci: BSD-3-Clause, with an MIT-licensed UTF-8 decoder, `libzueci-LICENSE.txt`.
- The Rust libraries of the repository's `rust_libs/`, linked statically through `native/`: the notices of each library's dependencies in `<library>-THIRD-PARTY-NOTICES.txt`. The updater is listed only in builds made with `-Updates`. `ppocr_rust` loads ONNX Runtime at run time through libloading 0.9.0: ISC, `libloading-LICENSE.txt`. ONNX Runtime itself is not part of the executable; the PP-OCR pack that brings its `onnxruntime.dll` carries its notices. hdrcapture contains code from windows-capture: MIT, `windows-capture-LICENSE.txt`.
- Lucide and Tabler icons among the SVG resources: `lucide-LICENSE.txt`, `tabler-icons-LICENSE.txt`.
- Qt and the third-party code inside it: `qt-THIRD-PARTY-NOTICES.txt`, with the license texts it names under `qt/`.

The default build ships Qt 6.11.0 (Core, Gui, Widgets, Svg, the qtimageformats plugins and the other plugins windeployqt selects) as DLLs; the single-file build (`-Static`) links the same modules statically. Qt is used under the LGPL-3.0-only option of its licensing. `scripts/qt_notices.ps1` writes `qt-THIRD-PARTY-NOTICES.txt` from the Qt SDK's SBOM for the Qt files each build actually ships: the libraries the executable links and the Qt DLLs deployed beside it, with every third-party component they contain (image codecs, font rendering, PCRE2, zlib and others) and its copyright lines. A license it names that has no text in licenses/qt stops the build; copy the text from the Qt sources' LICENSES folder.

Qt source: https://code.qt.io/cgit/qt/qtbase.git/tree/?h=v6.11.0

Qt licensing information: https://www.qt.io/licensing/open-source-lgpl-obligations

Qt Core uses the ICU that ships with Windows (`icuuc.dll` in System32); the build does not redistribute it.

Microsoft Visual C++ runtime DLLs, copied beside the executable in the default build, are supplied by the Microsoft toolchain and retain their applicable Microsoft redistribution terms; the single-file build links the runtime statically.

aqtinstall is a development-only downloader, not part of the runtime application.
