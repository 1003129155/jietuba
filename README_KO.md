<div align="center">

<img src="svg/%E6%89%98%E7%9B%98.svg" width="128" alt="jietuba logo" />

# jietuba

**Windows 스크린샷, 화면 그리기, OCR, 고정, 번역 및 클립보드 도구**

[![build](https://img.shields.io/github/actions/workflow/status/1003129155/jietuba/cpp.yml?branch=master2&label=build&style=flat-square)](https://github.com/1003129155/jietuba/actions/workflows/cpp.yml) [![license](https://img.shields.io/github/license/1003129155/jietuba?style=flat-square)](LICENSE) [![release](https://img.shields.io/github/v/release/1003129155/jietuba?style=flat-square)](https://github.com/1003129155/jietuba/releases/latest) ![platform](https://img.shields.io/badge/Windows-x64%20%7C%20ARM64-0078D4?style=flat-square)

[Windows용 다운로드](https://github.com/1003129155/jietuba/releases/latest) · [소스에서 빌드](#source-setup) · [개발 및 테스트](#development)

[简体中文](README_zh-CN.md) | [繁體中文](README_zh-TW.md) | [English](README.md) | [日本語](README_JA.md) | **한국어**

</div>

## 개요

jietuba는 무료 오픈소스 Windows 스크린샷 및 생산성 도구로, 스크린샷 주석, 화면 그리기, OCR, 번역, 무엇이든 고정, 화면 녹화, 클립보드 히스토리를 하나로 모았습니다. 스크린샷, 고정, 클립보드가 연결되어 캡처한 내용을 언제든 다시 찾아 재사용할 수 있습니다.

## 스크린샷 ⇄ 고정 ⇄ 클립보드, 삼방향 연결

캡처한 내용을 히스토리에 저장하고, 히스토리에서 한 번의 클릭으로 화면에 다시 고정합니다. 고정한 이미지에서 계속 주석을 달고 텍스트를 추출해 클립보드로 다시 복사하면, 세 도구가 한 흐름으로 맞물려 움직입니다.

- **풍부한 스크린샷 도구, 매끄러운 조작**: 창과 컨트롤에 스마트하게 맞추며, 10여 가지 주석 도구를 손쉽게 사용할 수 있습니다. 네 방향 긴 스크린샷, 자동 스크롤, 멀티 모니터 및 혼합 DPI, HDR 캡처를 지원합니다.
- **무엇이든 고정**: 필요한 내용을 한 번의 클릭으로 화면에 고정합니다. 텍스트, 이미지, 파일을 보면서 사용하며 확대/축소, 주석, 텍스트 추출, 번역을 그 자리에서 해결합니다.
- **클립보드가 모든 것을 기록**: 내 스크린샷뿐 아니라 어디서 복사했든 기록됩니다. 그룹 즐겨찾기, 검색, 필터, 다중 선택 드래그 아웃으로 중요한 내용을 언제든 찾고, 원하는 창에 끌어 넣거나 바탕 화면에 고정할 수 있습니다.
- **빠르고 가벼운 메모리 사용**: 응답 속도와 원활한 동작을 위해 깊이 최적화했으며, 저사양 PC의 복잡한 작업에서도 매끄럽게 실행됩니다. 실제 상주 메모리(워킹 셋)는 유휴 시 약 3 MB까지 내려가며, 고강도로 사용할 때도 메모리 사용량이 낮습니다. 설치 없이 압축만 풀면 바로 사용할 수 있습니다.

C++(Qt6)로 구현되었으며 이미지 처리, 클립보드, PP-OCR 엔진 등 핵심 부분은 Rust로 구현됩니다. Windows x86_64 및 ARM64를 지원합니다. 초기 Python(PySide6) 구현은 [`archive/python`](https://github.com/1003129155/jietuba/tree/archive/python) 브랜치에 보관되어 있으며 더 이상 업데이트되지 않습니다.

## 주요 기능

<table>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="캡처 및 주석" src="https://github.com/user-attachments/assets/397d6a3a-51a6-4860-9d05-0aed2c7c0429" />
<p><b>캡처 및 주석</b><br>창과 컨트롤을 스마트하게 인식합니다. 펜, 화살표, 번호, 모자이크, 스포트라이트, 워터마크 등 10여 가지 주석을 언제든 실행 취소/다시 실행할 수 있습니다.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="클립보드 히스토리" src="https://github.com/user-attachments/assets/d34ff233-fe7f-49dd-89ae-c5978fa25b5a" />
<p><b>클립보드 히스토리</b><br>모든 것을 기록하고 무엇이든 고정합니다. 그룹 즐겨찾기, 검색, 필터, 다중 선택 드래그 아웃까지. 복사했던 내용을 필요할 때 바로 찾습니다.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="스크롤 캡처" src="https://github.com/user-attachments/assets/a179b3e7-d201-4208-b518-81cecc4e9661" />
<p><b>스크롤 긴 스크린샷</b><br>상하좌우 네 방향 자동 이어 붙이기와 자동 스크롤을 지원합니다. 뒤로 스크롤해도 위치만 이동할 뿐 잘리지 않으며, 한 번의 클릭으로 불필요한 부분을 잘라냅니다.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="OCR" src="https://github.com/user-attachments/assets/60a16100-5edc-4543-9a35-daf05b1e244e" />
<p><b>OCR 텍스트 인식</b><br>Windows 11 캡처 도구 OCR과 PP-OCR 듀얼 엔진으로 오프라인 인식합니다. 줄바꿈을 자동으로 병합하고 문장 부호를 정리합니다.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<img width="100%" alt="고정" src="https://github.com/user-attachments/assets/d03e74e7-576b-46eb-8aea-21cc96d6a020" />
<p><b>무엇이든 고정</b><br>한 번의 클릭으로 화면에 고정해 두고 보며 사용합니다. 확대/축소, 주석, 텍스트 추출, 번역을 흐름을 끊지 않고 해결합니다.</p>
</td>
<td width="50%" valign="top">
<img width="100%" alt="GIF 및 MP4 녹화" src="https://github.com/user-attachments/assets/284f09b1-24b2-4569-9bc7-278c3bcd34da" />
<p><b>GIF / MP4 녹화</b><br>녹화 중 주석을 남길 수 있고 GIF는 재생 후 편집할 수 있습니다. MP4는 프레임 속도와 비트레이트를 조절하며 시스템 사운드와 마우스 커서를 담습니다. 마이크는 녹음하지 않습니다.</p>
</td>
</tr>
<tr>
<td width="50%" valign="top">
<p><b>번역</b><br>선택한 텍스트를 한 번의 단축키로 번역합니다. DeepL, Google, Azure, Amazon, Baidu, Volcengine, Tencent Cloud, Youdao, DeepSeek 및 OpenAI 호환 API를 지원하며, 결과를 소리 내어 읽어 줍니다.</p>
</td>
<td width="50%" valign="top">
<p><b>화면 그리기</b><br>화면에 직접 쓰고 주석을 달며 설명합니다. 화이트보드와 커서 강조로 발표와 수업이 더 직관적입니다.</p>
</td>
</tr>
</table>

또한:

- **코드 스캔**: 선택 영역의 QR 코드와 바코드를 한 번에 여러 개 인식합니다.
- **전역 마우스 단축키**: 보조 키를 누른 채 드래그하면 바로 스크린샷, 고정, 텍스트 인식, 번역 또는 코드 스캔(QR 코드 / 바코드)을 실행합니다.
- **더 보기**: PDF 내보내기, 스크린샷 워터마크, 지연 캡처, 다섯 가지 인터페이스 언어(간체 중국어, 번체 중국어, 영어, 일본어, 한국어).

---

## 다운로드 및 실행

1. [Releases 페이지](https://github.com/1003129155/jietuba/releases/latest)를 열고 기기에 맞는 `-x64.zip` 또는 `-arm64.zip` 패키지를 다운로드합니다. 런타임 설치가 필요 없습니다.
2. 압축을 모두 풀고 안의 exe를 더블 클릭해 실행합니다.

|  | 풀 버전 `jietuba_pp-…zip` | 라이트 버전 `jietuba_lite-…zip` |
|---|---|---|
| 패키지 내용 | `jietuba_pp.exe`와 같은 위치의 `models/` 폴더(둘 다 유지) | 단일 `jietuba_lite.exe` |
| OCR 엔진 | 캡처 도구 OCR + 내장 PP-OCR, 시스템 캡처 도구가 없어도 인식 가능 | 기본적으로 Windows 11 캡처 도구 OCR 사용, 설정에서 [PP-OCR 구성 요소 팩](#ppocr-pack) 다운로드 가능 |
| 추천 대상 | Windows 10 또는 바로 쓸 수 있는 OCR이 필요한 경우 | Windows 11, 작은 용량 선호 |

두 버전 모두 기본적으로 사용 가능한 엔진을 자동 선택합니다. Windows 11 캡처 도구 OCR을 우선 사용하고, 없으면 PP-OCR을 시도합니다. 설정의「OCR 설정」에서 수동으로 전환할 수 있습니다. 인식 언어는 엔진과 모델에 따라 다르며, 풀 버전에 내장된 PP-OCRv6 small은 중국어, 영어, 일본어를 지원합니다. 캡처 도구 버전이 11.2308 미만(「텍스트 작업」없음)이거나 캡처 도구가 설치되지 않은 경우, 라이트 버전에서 텍스트를 인식하려면 사용 가능한 캡처 도구 OCR 또는 PP-OCR 구성 요소 팩이 필요합니다.

> [!NOTE]
> 브라우저로 다운로드하면 Windows가 실행 경고를 표시할 수 있습니다. 안내가 나타나면「추가 정보」를 누른 뒤「실행」을 선택하면 시작됩니다.

> [!WARNING]
> 두 버전은 설정과 클립보드 히스토리를 공유하므로 서로 바꿔 쓸 수 있지만 동시에 실행하지 마세요. 기본 단축키가 충돌합니다.

> [!TIP]
> 설정은 기본적으로 `%APPDATA%\Jietuba\settings.ini`에 저장됩니다. exe 옆에 `settings.ini`(빈 파일도 가능)를 두면 프로그램이 그것을 사용하므로, 프로그램 폴더만 복사하면 설정도 함께 이동합니다. 스크린샷 저장 폴더, 클립보드 히스토리 위치, 자동 시작처럼 이 PC에서만 의미 있는 항목은 레지스트리에 남습니다. 클립보드 히스토리는 기본적으로 `%APPDATA%\pyclipboard`에 있으며, 설정의「클립보드」페이지에서 위치를 바꿀 수 있습니다.

### 업데이트

업데이터가 포함된 첫 릴리스를 설치한 후에는 **정보 → 업데이트 확인 → 원클릭 업데이트 후 다시 시작**에서 업그레이드할 수 있습니다. 업데이터는 아키텍처에 맞는 GitHub Release ZIP을 다운로드하고 메인 EXE만 교체한 뒤 다시 시작합니다. 설정, 클립보드 히스토리, 모델 및 기타 파일은 유지됩니다. 녹화, 내보내기 또는 백그라운드 저장이 끝나지 않았다면 다운로드 결과를 보존했다가 작업을 마친 뒤 다시 시도하세요. 가장 최근의 메인 프로그램 백업은 `.jietuba-update/`에 저장됩니다. 자세한 내용은 [업데이터 복구 설명](rust_libs/updater/README.md)을 참조하세요. 2.1.8 이전의 Python 버전은 원클릭 업데이트 후 C++ 구현으로 전환되며 설정, 클립보드 히스토리, 모델은 그대로 유지됩니다.

<a id="ppocr-pack"></a>

### PP-OCR 구성 요소 팩(라이트 버전)

라이트 버전에는 PP-OCR의 전처리/후처리가 포함되지만 ONNX Runtime과 모델은 포함되지 않습니다. 설정의「PP-OCR 모델 관리」에서 다운로드할 수 있습니다. 모델은 ModelScope에서 가져오고, ONNX Runtime은 Microsoft가 배포하는 `onnxruntime` wheel에서 추출하며, 다운로드 소스는 PyPI와 미러를 포함합니다. 다운로드 후에는 내장 카탈로그로 파일 크기와 해시를 검증합니다. PP-OCRv6, PP-OCRv5 및 다양한 언어 인식 모델을 지원하며 로컬 ONNX 파일도 추가할 수 있습니다. 다운로드, 추가, 전환 모두 다시 시작할 필요가 없습니다.

exe 옆의 `models/` 폴더에 직접 넣을 수도 있습니다. `PP-OCRv6_det_small.onnx`, `PP-OCRv6_rec_small.onnx`(저장소의 [models/](models/)에 있음)와 ONNX Runtime 1.24 이상의 CPU 전용 `onnxruntime.dll`입니다.

## 코드 서명 정책

Windows 릴리스 패키지의 `jietuba_pp.exe`와 `jietuba_lite.exe`는 모두 GitHub Actions가 이 저장소 [cpp/](cpp/)의 C++ 소스 코드로 빌드합니다. 릴리스 워크플로는 SignPath를 통한 코드 서명을 지원합니다. 서명 여부는 해당 빌드 구성에 따라 달라지므로, 구체적인 릴리스 패키지의 EXE 디지털 서명과 릴리스 노트를 기준으로 삼으세요.

Free code signing provided by [SignPath.io](https://about.signpath.io/), certificate by [SignPath Foundation](https://signpath.org/).

- 커미터, 리뷰어, 승인자: [@1003129155](https://github.com/1003129155)
- 외부 기여는 Pull Request로 제출되며 유지 관리자의 리뷰를 거친 뒤에만 병합됩니다.

<a id="privacy"></a>

### 개인정보 보호

스크린샷, OCR, 주석, 화면 녹화, 클립보드 히스토리는 이 PC에서 처리되고 저장됩니다. 모든 네트워크 기능은 직접 실행해야 하며 프로그램이 자동으로 연결하지 않습니다. 구체적으로:

- 일상적인 스크린샷, OCR 인식, 클립보드 기록에는 콘텐츠 업로드가 필요하지 않습니다. PP-OCR을 사용하려면 로컬 모델과 런타임을 준비해야 합니다.
- 번역 시 번역할 텍스트는 설정에서 선택한 서비스로 전송되며, 직접 구성한 자격 증명이 사용됩니다. 해당 서비스의 개인정보 처리방침이 적용됩니다. 사용자 지정 OpenAI 호환 인터페이스는 이 PC의 서비스를 가리킬 수도 있으며, 이 경우 텍스트는 해당 로컬 엔드포인트로 전송됩니다.
- **정보 → 업데이트 확인**에서 업데이트를 확인하거나 설치하면 프로그램이 GitHub에 접속해 버전 정보를 읽고 릴리스 패키지를 다운로드합니다.
- 「PP-OCR 모델 관리」에서 모델이나 런타임을 다운로드하면 ModelScope, PyPI 또는 내장 카탈로그의 미러에 접속합니다. 이 요청은 구성 요소를 다운로드하기 위한 것이며 인식할 이미지를 업로드하지 않습니다.
- 번역 설정에서 연결을 테스트하거나 모델 목록을 가져오면 구성한 서비스 엔드포인트에 요청합니다. 연결 테스트는 고정된 샘플 텍스트를 보내고, 모델 목록 조회는 클립보드 콘텐츠를 보내지 않습니다.

---

<a id="source-setup"></a>

## 소스에서 빌드

Visual Studio의 "C++를 사용한 데스크톱 개발"(CMake 포함), Rust MSVC 툴체인, Qt 6.11 MSVC SDK가 필요합니다. `cpp/` 디렉터리에서 PowerShell로 실행하세요:

```powershell
python -m venv .tools
.\.tools\Scripts\python.exe -m pip install py7zr==1.1.4
.\.tools\Scripts\python.exe scripts\install_qt.py   # Qt SDK를 .sdk/에 다운로드
.\build.cmd                                         # 빌드, 테스트 후 dist/Release에 배포
```

이미 Qt6 MSVC x64 SDK가 있다면 `.\scripts\build.ps1 -QtRoot C:\Qt\6.11.0\msvc2022_64`를 사용하세요. `-SkipTests`는 테스트를 건너뛰고, `-Configuration Debug`는 Debug를 빌드합니다. 빌드가 끝나면 `run.cmd`를 실행하면 됩니다.

릴리스용 단일 파일 버전은 Qt와 MSVC 런타임을 정적으로 링크하므로 이 둘을 따로 설치할 필요가 없습니다. OCR에 필요한 모델과 ONNX Runtime은 위의 구성 요소 팩 설명을 참조하세요. 먼저 `.\scripts\build_static_qt.ps1`로 Qt 공식 소스에서 정적 SDK를 빌드한 뒤:

```powershell
.\build.cmd -Static          # 라이트 버전: dist/Release-static/jietuba_lite.exe
.\build.cmd -Static -Full    # 풀 버전: dist/Release-static-full/jietuba_pp.exe, ONNX Runtime 내장, 기본 PP-OCR 모델 포함
```

ARM64 버전은 x64에서 크로스 컴파일합니다. 먼저 x64 정적 Qt를 빌드한 다음 `.\scripts\build_static_qt.ps1 -Arch arm64`와 `.\build.cmd -Static -Arch arm64`를 실행하세요. Visual Studio의 MSVC ARM64 빌드 도구와 `rustup target add aarch64-pc-windows-msvc`가 필요합니다. 버전 번호는 저장소 루트의 `VERSION` 파일에서 가져옵니다.

PP-OCR 모델은 저장소의 [models/](models/) 디렉터리에 있으며 `PP-OCRv6_det_small.onnx`와 `PP-OCRv6_rec_small.onnx`가 포함됩니다. 캡처 도구 OCR에 필요한 파일은 이 PC의 Windows 11 캡처 도구에서 가져오며, 처음 사용할 때 `%LOCALAPPDATA%\Jietuba\oneocr`로 자동 복사됩니다. 별도로 다운로드할 필요가 없습니다.

---

<a id="development"></a>

## 개발 및 테스트

`.\build.cmd`는 빌드 후 모든 테스트를 실행합니다. 이름이 `_desktop`으로 끝나는 테스트는 실제 데스크톱에서 키보드·마우스를 주입하고 전역 단축키를 등록하며 화면을 캡처하므로, 실행 중에는 마우스와 키보드를 조작하지 마세요. 데스크톱이 필요 없는 테스트만 실행하려면:

```powershell
$env:PATH = "$PWD\.sdk\6.11.0\msvc2022_64\bin;$env:PATH"
ctest --test-dir build\msvc -C Release --output-on-failure -E "_desktop$|_windows_parity$"
```

테스트 프로그램은 모두 임시 샌드박스에서 실행되며 이 PC의 설정, 클립보드 히스토리, 로그를 읽거나 쓰지 않습니다. 분석 빌드(`scripts\check.ps1`: AddressSanitizer, 코드 분석, 누수 감지)와 커버리지(`scripts\coverage.ps1`)도 있습니다.

[C++ CI](.github/workflows/cpp.yml)는 모든 PR에서 빌드하고 오프스크린 테스트를 실행하며 라인 커버리지를 확인합니다. 데스크톱 테스트는 별도로 실행되며 실패해도 이 검사를 막지 않습니다. `rust_libs/`를 수정하면 [Rust CI](.github/workflows/rust.yml)가 Rust 단위 테스트를 실행하고 서드파티 라이선스를 재확인합니다.

릴리스는 [build.yml](.github/workflows/build.yml)이 수행합니다. 태그는 반드시 `release-<VERSION 파일의 버전>`이어야 합니다. [lite.yml](.github/workflows/lite.yml)은 라이트 버전과 풀 버전(x64 및 ARM64)을 빌드하고, 릴리스 워크플로가 구성에 따라 서명하고 패키징하여 `jietuba_lite-<tag>-<arch>.zip`과 `jietuba_pp-<tag>-<arch>.zip`을 생성합니다.

---

## 디렉터리 구조

```text
# 프로젝트 루트
├── README.md / README_zh-CN.md / README_zh-TW.md / README_JA.md / README_KO.md   # 영어, 간체 중국어, 번체 중국어, 일본어, 한국어 문서
├── VERSION                  # 버전 번호, CMake와 릴리스 워크플로가 읽음
├── app.ico                  # 프로그램 및 업데이터 아이콘
├── licenses/                # 릴리스 패키지에 포함되는 서드파티 라이선스
│
├── cpp/                     # C++(Qt6) 구현
│   ├── src/                 # 소스 코드, <패키지>/<모듈> 구조
│   │   ├── annotation/      # 화면 실시간 주석 — 화면마다 한 레이어, 클릭 통과 전환, 화면 간 실행 취소
│   │   ├── barcode/         # QR 코드/바코드 인식(zxing-cpp)
│   │   ├── canvas/          # 캔버스 — 그래픽 편집 코어, 실행 취소, 레이어
│   │   ├── capture/         # 캡처 — 화면 스크린샷, 창과 요소 인식
│   │   ├── clipboard/       # 클립보드 관리 — 히스토리, 그룹/빠른 실행, 가져오기/내보내기, 검색
│   │   ├── core/            # 코어 — 부트스트랩, 로그, 리소스, 테마, 국제화, 단축키
│   │   ├── gif/             # GIF/MP4 녹화, GIF 편집, 재생, 내보내기
│   │   ├── ocr/             # 캡처 도구 OCR 및 PP-OCR
│   │   ├── pin/             # 고정 — 항상 위, 편집, OCR, 번역
│   │   ├── settings/        # 통합 설정 관리
│   │   ├── stitch/          # 긴 스크린샷 이어 붙이기
│   │   ├── text_recognition/ # OCR 결과 레이아웃
│   │   ├── tools/           # 그리기 도구 — 펜, 사각형, 화살표, 텍스트, 모자이크 등
│   │   ├── translation/     # 멀티 프로바이더 번역과 소리 내어 읽기
│   │   ├── ui/              # UI 컴포넌트, 스크린샷 창, 도구 모음, 설정 창
│   │   └── video/           # MP4 녹화 자식 프로세스
│   ├── native/              # rust_libs의 C 인터페이스, 정적 링크
│   ├── resources/           # 기본 설정, UI 번역(XML 및 .qm), 로그 문구
│   ├── scripts/             # 빌드, Qt SDK 준비, 번역 컴파일
│   └── tests/               # QtTest 테스트와 대조 데이터
│
├── rust_libs/               # Rust 라이브러리 소스, C++이 cpp/native로 정적 링크
│   ├── gifrecorder/         # GIF 프레임 저장, 재생, 합성 인코딩
│   ├── longstitch/          # 긴 스크린샷 이어 붙이기 알고리즘
│   ├── pyclipboard/         # 클립보드 저수준 작업
│   ├── ppocr_rust/          # PP-OCR(PaddleOCR) ONNX 인식 엔진
│   ├── hdrcapture/          # HDR 데스크톱 캡처
│   ├── updater/             # 메인 EXE만 교체하는 독립 업데이터
│   └── video_recorder/      # Media Foundation 녹화 및 WASAPI 시스템 사운드 캡처
│
├── models/                  # PP-OCR ONNX 모델(풀 버전에 포함되어 배포)
│   ├── PP-OCRv6_det_small.onnx   # 텍스트 감지 모델(DBNet)
│   └── PP-OCRv6_rec_small.onnx   # 텍스트 인식 모델(CRNN/CTC)
│
└── svg/                     # SVG 아이콘 리소스
```

## 모듈 상세 설명

각 모듈의 상세 설명은 [docs/MODULES.md](docs/MODULES.md)를 참조하세요.

---

## 라이선스

저작권 및 MIT 라이선스는 [LICENSE](LICENSE)를 참조하고, Qt 및 기타 서드파티 구성 요소는 [THIRD-PARTY-NOTICES.md](cpp/THIRD-PARTY-NOTICES.md)를 참조하세요.
