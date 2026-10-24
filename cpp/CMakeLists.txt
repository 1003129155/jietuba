cmake_minimum_required(VERSION 3.24)
project(JietubaCpp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets Svg)
qt_standard_project_setup()
set(CMAKE_AUTOUIC OFF)
if(MSVC)
    # cl.exe compiles one source at a time unless asked; the Visual Studio generator's parallel
    # build only spreads across targets. Set before the dependencies are added so they get it too.
    add_compile_options(/MP)
endif()
# The repository root holds the version number, the Rust libraries, the icons and svg/.
get_filename_component(JIETUBA_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)

# src/<package>/<module>.cpp, one folder per feature area.
file(GLOB_RECURSE JIETUBA_SOURCES CONFIGURE_DEPENDS
    src/*.cpp src/*.h src/*.inc)
list(REMOVE_ITEM JIETUBA_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(jietuba_lib STATIC ${JIETUBA_SOURCES} resources/settings.qrc)
set_target_properties(jietuba_lib PROPERTIES AUTORCC ON)
# The .qm files are compiled from resources/translations/*.xml by scripts/compile_translations.py.
file(GLOB JIETUBA_QM_FILES CONFIGURE_DEPENDS resources/translations/*.qm)
qt_add_resources(jietuba_lib "jietuba_runtime_resources" PREFIX "/" BASE resources
    FILES resources/core/log_translations.json ${JIETUBA_QM_FILES})
# The repository's svg/ is embedded as :/svg.
file(GLOB_RECURSE JIETUBA_SVG_FILES CONFIGURE_DEPENDS ${JIETUBA_REPO_ROOT}/svg/*.svg ${JIETUBA_REPO_ROOT}/svg/*.json)
qt_add_resources(jietuba_lib "jietuba_svg_resources" PREFIX "/" BASE ${JIETUBA_REPO_ROOT}
    FILES ${JIETUBA_SVG_FILES})
target_include_directories(jietuba_lib PUBLIC src)
# windows.h must not define min/max macros.
target_compile_definitions(jietuba_lib PUBLIC NOMINMAX)
target_link_libraries(jietuba_lib PUBLIC Qt6::Widgets Qt6::Svg)
if(WIN32)
    target_link_libraries(jietuba_lib PUBLIC user32 gdi32 shell32 shcore dwmapi ole32 oleaut32 oleacc winhttp version secur32)
endif()
# Barcode decoding: the same zxing-cpp release the reference's Python wheel is built from.
include(FetchContent)
set(ZXING_READERS ON CACHE BOOL "" FORCE)
set(ZXING_WRITERS OFF CACHE STRING "" FORCE)
set(ZXING_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZXING_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_BLACKBOX_TESTS OFF CACHE BOOL "" FORCE)
set(ZXING_PYTHON_MODULE OFF CACHE BOOL "" FORCE)
set(ZXING_C_API OFF CACHE BOOL "" FORCE)
FetchContent_Declare(zxing_cpp
    GIT_REPOSITORY https://github.com/zxing-cpp/zxing-cpp.git
    GIT_TAG v3.1.1  # 287c85df6f961c8efbfb5ffd736cd9457b8b890e
    GIT_SHALLOW TRUE)
set(JIETUBA_SAVED_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(zxing_cpp)
set(BUILD_SHARED_LIBS ${JIETUBA_SAVED_BUILD_SHARED_LIBS})
# zxing-cpp 3 needs C++20; only it and the one source including its headers get it.
set_target_properties(ZXing PROPERTIES CXX_STANDARD 20 AUTOMOC OFF)
set_source_files_properties(src/barcode/reader.cpp PROPERTIES COMPILE_OPTIONS /std:c++20)
target_link_libraries(jietuba_lib PUBLIC ZXing::ZXing)
# Shipped by scripts/build.ps1; the source directory may be a local override of the fetch.
configure_file(${zxing_cpp_SOURCE_DIR}/LICENSE ${CMAKE_BINARY_DIR}/zxing-cpp-LICENSE.txt COPYONLY)
# The Snipping Tool engine's model key is read from the oneocr package the reference pins
# (oneocr==1.0.12) instead of being kept in this repository.
FetchContent_Declare(oneocr
    GIT_REPOSITORY https://github.com/AuroraWright/oneocr.git
    GIT_TAG d1d401c3776b8cda9af71334a0bea2a7d1fe1305)  # 1.0.12
FetchContent_MakeAvailable(oneocr)
file(STRINGS ${oneocr_SOURCE_DIR}/oneocr.py JIETUBA_ONEOCR_KEY_LINE REGEX "^MODEL_KEY = b\".*\"\r?$")
if(NOT JIETUBA_ONEOCR_KEY_LINE)
    message(FATAL_ERROR "MODEL_KEY not found in ${oneocr_SOURCE_DIR}/oneocr.py")
endif()
# A Python bytes literal of printable characters reads the same as a C++ string literal.
string(REGEX REPLACE "^MODEL_KEY = b(\".*\")\r?$" "\\1" JIETUBA_ONEOCR_MODEL_KEY "${JIETUBA_ONEOCR_KEY_LINE}")
configure_file(src/ocr/oneocr_model_key.h.in ${CMAKE_BINARY_DIR}/generated/ocr/oneocr_model_key.h @ONLY)
# The version is the one line of the repository's VERSION file, which the release workflow's tag
# check and signing request also read.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${JIETUBA_REPO_ROOT}/VERSION)
file(STRINGS ${JIETUBA_REPO_ROOT}/VERSION JIETUBA_APP_VERSION LIMIT_COUNT 1)
string(STRIP "${JIETUBA_APP_VERSION}" JIETUBA_APP_VERSION)
if(NOT JIETUBA_APP_VERSION MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "${JIETUBA_REPO_ROOT}/VERSION must hold a dotted version such as 2.1.8, found \"${JIETUBA_APP_VERSION}\"")
endif()
# The version resource takes four numbers: the leading numeric part, padded with zeros.
string(REGEX MATCH "^[0-9]+(\\.[0-9]+)*" JIETUBA_APP_VERSION_NUMBERS "${JIETUBA_APP_VERSION}")
string(REPLACE "." ";" JIETUBA_APP_VERSION_NUMBERS "${JIETUBA_APP_VERSION_NUMBERS}")
list(APPEND JIETUBA_APP_VERSION_NUMBERS 0 0 0 0)
list(SUBLIST JIETUBA_APP_VERSION_NUMBERS 0 4 JIETUBA_APP_VERSION_NUMBERS)
list(JOIN JIETUBA_APP_VERSION_NUMBERS "," JIETUBA_APP_VERSION_NUMBERS)
configure_file(src/app_version.h.in ${CMAKE_BINARY_DIR}/generated/app_version.h @ONLY)
target_include_directories(jietuba_lib PUBLIC ${CMAKE_BINARY_DIR}/generated)
# native/ is a C ABI over the repository's Rust libraries (../rust_libs), built by cargo as a
# static library.
set(JIETUBA_RUST_LIBS ${JIETUBA_REPO_ROOT}/rust_libs)
find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(JIETUBA_NATIVE_TARGET_DIR ${CMAKE_BINARY_DIR}/native)
set(JIETUBA_NATIVE_LIB ${JIETUBA_NATIVE_TARGET_DIR}/release/jietuba_native.lib)
set(JIETUBA_NATIVE_CARGO_TARGET)
# An ARM64 build is cross-compiled from x64; cargo then needs the target triple.
if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(JIETUBA_NATIVE_CARGO_TARGET --target aarch64-pc-windows-msvc)
    set(JIETUBA_NATIVE_LIB ${JIETUBA_NATIVE_TARGET_DIR}/aarch64-pc-windows-msvc/release/jietuba_native.lib)
endif()
file(GLOB_RECURSE JIETUBA_NATIVE_INPUTS CONFIGURE_DEPENDS
    native/src/*.rs native/Cargo.toml native/Cargo.lock native/*/src/*.rs native/*/Cargo.toml)
# Crate by crate: a glob over the whole of rust_libs would also walk its target directory.
foreach(crate longstitch gifrecorder video_recorder hdrcapture ppocr_rust pyclipboard)
    file(GLOB_RECURSE crate_inputs CONFIGURE_DEPENDS
        ${JIETUBA_RUST_LIBS}/${crate}/src/*.rs ${JIETUBA_RUST_LIBS}/${crate}/*.toml ${JIETUBA_RUST_LIBS}/${crate}/build.rs)
    list(APPEND JIETUBA_NATIVE_INPUTS ${crate_inputs})
endforeach()
# PP-OCR is always built; ONNX Runtime is not linked but loaded from the PP-OCR pack at run time.
add_custom_command(OUTPUT ${JIETUBA_NATIVE_LIB}
    COMMAND ${CARGO_EXECUTABLE} build --release --locked ${JIETUBA_NATIVE_CARGO_TARGET}
        --manifest-path ${CMAKE_CURRENT_SOURCE_DIR}/native/Cargo.toml
        --target-dir ${JIETUBA_NATIVE_TARGET_DIR}
    DEPENDS ${JIETUBA_NATIVE_INPUTS}
    COMMENT "Building native Rust libraries"
    VERBATIM)
add_custom_target(jietuba_native DEPENDS ${JIETUBA_NATIVE_LIB})
# jietuba_lib is static and compiles without the Rust library, so only the executables linking it
# wait for cargo (below); the C++ sources build alongside it.
# The Rust standard library's system dependencies (cargo --print native-static-libs).
target_link_libraries(jietuba_lib PUBLIC ${JIETUBA_NATIVE_LIB} ntdll userenv ws2_32 dbghelp)
# Media Foundation is used only by the MP4 recorder worker; delay loading lets the app start on
# Windows editions without the Media Feature Pack (see video/worker.cpp).
target_link_libraries(jietuba_lib PUBLIC delayimp)
target_link_options(jietuba_lib INTERFACE /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll /IGNORE:4199)
if(MSVC)
    # C4828: rcc writes non-ASCII resource names into comments of generated sources.
    target_compile_options(jietuba_lib PUBLIC /utf-8 /W4 /wd4828)
    # Release also writes a PDB so crash stacks resolve to functions and lines; the
    # optimisation flags keep /DEBUG from growing the executable.
    target_compile_options(jietuba_lib PUBLIC $<$<CONFIG:Release>:/Zi>)
    target_link_options(jietuba_lib INTERFACE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

# Analysis builds; use a separate build directory (scripts/check.ps1).
# AddressSanitizer: out-of-bounds, use-after-free and null dereference reports with stacks.
option(JIETUBA_SANITIZE "Build with AddressSanitizer" OFF)
# MSVC code analysis (C6011 and related) plus clang-tidy through MSBuild; the checks come from .clang-tidy.
option(JIETUBA_ANALYZE "Run static analysis during the build" OFF)
# Debug builds only: QtTest mains run three times and report what the second run leaks (tests/leak_check.cpp).
option(JIETUBA_LEAK_CHECK "Check the tests for heap leaks" OFF)
if(JIETUBA_SANITIZE AND MSVC)
    # The vector/string annotations produce false reports across the uninstrumented Qt DLLs.
    target_compile_options(jietuba_lib PUBLIC /fsanitize=address /Zi)
    target_compile_definitions(jietuba_lib PUBLIC _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    target_link_options(jietuba_lib INTERFACE /INCREMENTAL:NO /DEBUG)
endif()
if(JIETUBA_ANALYZE AND MSVC)
    set_target_properties(jietuba_lib PROPERTIES
        VS_GLOBAL_RunCodeAnalysis true
        VS_GLOBAL_EnableMicrosoftCodeAnalysis true
        VS_GLOBAL_EnableClangTidyCodeAnalysis true
        VS_GLOBAL_CodeAnalysisRuleSet NativeRecommendedRules.ruleset)
endif()

# The single-file build (build.ps1 -Static) is released as the lite package, whose updater
# replaces the package's jietuba_lite.exe; build.ps1 reads the name from the cache.
option(JIETUBA_LITE_EXE "Name the exe jietuba_lite.exe, as the released lite package" OFF)
# The released full package: jietuba_pp.exe, the name its updater replaces, with the ONNX Runtime at
# JIETUBA_ORT_DLL embedded as a resource (ocr/bundled_runtime puts it into models/).
option(JIETUBA_FULL_EXE "Name the exe jietuba_pp.exe, as the released full package, and embed onnxruntime.dll" OFF)
set(JIETUBA_ORT_DLL "" CACHE FILEPATH "onnxruntime.dll to embed for JIETUBA_FULL_EXE (scripts/fetch_ort_runtime.ps1)")
set(JIETUBA_ORT_RC "")
if(JIETUBA_FULL_EXE AND JIETUBA_LITE_EXE)
    message(FATAL_ERROR "JIETUBA_LITE_EXE and JIETUBA_FULL_EXE are exclusive")
endif()
if(JIETUBA_FULL_EXE)
    if(NOT EXISTS "${JIETUBA_ORT_DLL}")
        message(FATAL_ERROR "JIETUBA_FULL_EXE needs JIETUBA_ORT_DLL: ${JIETUBA_ORT_DLL}")
    endif()
    set(JIETUBA_EXE_NAME jietuba_pp CACHE INTERNAL "")
    target_compile_definitions(jietuba_lib PUBLIC JIETUBA_FULL_VARIANT)
    file(TO_CMAKE_PATH "${JIETUBA_ORT_DLL}" JIETUBA_ORT_DLL_PATH)
    set(JIETUBA_ORT_RC "ORT_RUNTIME RCDATA \"${JIETUBA_ORT_DLL_PATH}\"")
elseif(JIETUBA_LITE_EXE)
    set(JIETUBA_EXE_NAME jietuba_lite CACHE INTERNAL "")
else()
    set(JIETUBA_EXE_NAME jietuba_cpp CACHE INTERNAL "")
endif()
# The exe icon and the version resource; SignPath checks the product name and version.
# The icon is the repository's app.ico, copied next to the generated rc and qrc that embed it.
configure_file(${JIETUBA_REPO_ROOT}/app.ico ${CMAKE_BINARY_DIR}/generated/app.ico COPYONLY)
set(JIETUBA_ICON_PATH ${CMAKE_BINARY_DIR}/generated/app.ico)
set(JIETUBA_ICON_QRC ${CMAKE_BINARY_DIR}/generated/app_icon.qrc)
file(CONFIGURE OUTPUT ${JIETUBA_ICON_QRC} CONTENT "<RCC><qresource prefix=\"/\"><file alias=\"app.ico\">app.ico</file></qresource></RCC>\n")
configure_file(resources/app.rc.in ${CMAKE_BINARY_DIR}/generated/app.rc @ONLY)
qt_add_executable(jietuba_cpp WIN32 src/main.cpp ${JIETUBA_ICON_QRC} ${CMAKE_BINARY_DIR}/generated/app.rc)
set_target_properties(jietuba_cpp PROPERTIES AUTORCC ON OUTPUT_NAME ${JIETUBA_EXE_NAME})
if(JIETUBA_FULL_EXE)
    set_source_files_properties(${CMAKE_BINARY_DIR}/generated/app.rc PROPERTIES OBJECT_DEPENDS "${JIETUBA_ORT_DLL}")
endif()
target_link_libraries(jietuba_cpp PRIVATE jietuba_lib)
add_dependencies(jietuba_cpp jietuba_native)
# One-click updates need the updater helper from rust_libs inside the exe, copied out to run (see
# core/updater_process). build.ps1 turns it on for the single-file lite build only.
option(JIETUBA_UPDATES "Bundle the updater helper and offer one-click updates" OFF)
if(JIETUBA_UPDATES)
    set(JIETUBA_UPDATER_TARGET_DIR ${CMAKE_BINARY_DIR}/updater)
    set(JIETUBA_UPDATER_EXE ${JIETUBA_UPDATER_TARGET_DIR}/release/jietuba_updater.exe)
    if(JIETUBA_NATIVE_CARGO_TARGET)
        set(JIETUBA_UPDATER_EXE ${JIETUBA_UPDATER_TARGET_DIR}/aarch64-pc-windows-msvc/release/jietuba_updater.exe)
    endif()
    file(GLOB_RECURSE JIETUBA_UPDATER_INPUTS CONFIGURE_DEPENDS
        ${JIETUBA_RUST_LIBS}/updater/src/* ${JIETUBA_RUST_LIBS}/updater/Cargo.toml ${JIETUBA_RUST_LIBS}/updater/build.rs)
    # A static CRT, so the copy runs on its own.
    add_custom_command(OUTPUT ${JIETUBA_UPDATER_EXE}
        COMMAND ${CARGO_EXECUTABLE} rustc --release --locked -p jietuba-updater --bin jietuba_updater ${JIETUBA_NATIVE_CARGO_TARGET}
            --manifest-path ${JIETUBA_RUST_LIBS}/Cargo.toml
            --target-dir ${JIETUBA_UPDATER_TARGET_DIR} -- -C target-feature=+crt-static
        DEPENDS ${JIETUBA_UPDATER_INPUTS} ${JIETUBA_RUST_LIBS}/Cargo.lock
        COMMENT "Building the updater helper"
        VERBATIM)
    get_filename_component(JIETUBA_UPDATER_DIR ${JIETUBA_UPDATER_EXE} DIRECTORY)
    qt_add_resources(jietuba_cpp jietuba_updater PREFIX /updater BASE ${JIETUBA_UPDATER_DIR} FILES ${JIETUBA_UPDATER_EXE})
endif()

include(CTest)
if(BUILD_TESTING)
    # QtTest output never reaches ctest's pipe; each test also writes its report here, read by
    # scripts/show_failed_tests.ps1, so a failure that does not recur on a rerun is still reported.
    set(JIETUBA_TEST_REPORTS ${CMAKE_BINARY_DIR}/test_reports)
    file(MAKE_DIRECTORY ${JIETUBA_TEST_REPORTS})
    # Network only for the local server in the HTTP tests; the application uses WinHTTP.
    find_package(Qt6 REQUIRED COMPONENTS Test Network)
    qt_add_executable(jietuba_core_tests tests/test_core.cpp)
    target_link_libraries(jietuba_core_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_core_tests PRIVATE JIETUBA_CORE_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/core.json")
    add_test(NAME python_core_parity COMMAND jietuba_core_tests -o ${JIETUBA_TEST_REPORTS}/python_core_parity.txt,txt)
    set_tests_properties(python_core_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_translation_tests tests/test_translation.cpp)
    target_link_libraries(jietuba_translation_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_translation_tests PRIVATE JIETUBA_TRANSLATION_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/translation.json")
    add_test(NAME python_translation_parity COMMAND jietuba_translation_tests -o ${JIETUBA_TEST_REPORTS}/python_translation_parity.txt,txt)
    set_tests_properties(python_translation_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_barcode_tests tests/test_barcode.cpp)
    target_link_libraries(jietuba_barcode_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_barcode_tests PRIVATE JIETUBA_BARCODE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME python_barcode_parity COMMAND jietuba_barcode_tests -o ${JIETUBA_TEST_REPORTS}/python_barcode_parity.txt,txt)
    set_tests_properties(python_barcode_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_text_recognition_tests tests/test_text_recognition.cpp)
    target_link_libraries(jietuba_text_recognition_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME text_recognition_layout COMMAND jietuba_text_recognition_tests -o ${JIETUBA_TEST_REPORTS}/text_recognition_layout.txt,txt)
    set_tests_properties(text_recognition_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_input_engine_tests tests/test_input_engine.cpp)
    target_link_libraries(jietuba_input_engine_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME input_engine COMMAND jietuba_input_engine_tests -o ${JIETUBA_TEST_REPORTS}/input_engine.txt,txt)
    set_tests_properties(input_engine PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_gif_playback_tests tests/test_gif_playback.cpp)
    target_link_libraries(jietuba_gif_playback_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME gif_playback COMMAND jietuba_gif_playback_tests -o ${JIETUBA_TEST_REPORTS}/gif_playback.txt,txt)
    set_tests_properties(gif_playback PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_stitch_tests tests/test_stitch.cpp)
    target_link_libraries(jietuba_stitch_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_stitch_tests PRIVATE JIETUBA_STITCH_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME python_stitch_parity COMMAND jietuba_stitch_tests -o ${JIETUBA_TEST_REPORTS}/python_stitch_parity.txt,txt)
    set_tests_properties(python_stitch_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # Real desktop capture; skips where no HDR session can be built.
        qt_add_executable(jietuba_hdr_capture_tests tests/test_hdr_capture.cpp)
        target_link_libraries(jietuba_hdr_capture_tests PRIVATE jietuba_lib Qt6::Test)
        add_test(NAME hdr_capture_desktop COMMAND jietuba_hdr_capture_tests -o ${JIETUBA_TEST_REPORTS}/hdr_capture_desktop.txt,txt -platform windows)
        set_tests_properties(hdr_capture_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()
    if(WIN32)
        # Recording windows and the real MP4 worker process; needs a desktop.
        qt_add_executable(jietuba_gif_tests tests/test_gif.cpp)
        target_link_libraries(jietuba_gif_tests PRIVATE jietuba_lib Qt6::Test)
        target_compile_definitions(jietuba_gif_tests PRIVATE JIETUBA_APP_PATH="$<TARGET_FILE:jietuba_cpp>")
        add_dependencies(jietuba_gif_tests jietuba_cpp)
        add_test(NAME gif_recording_desktop COMMAND jietuba_gif_tests -o ${JIETUBA_TEST_REPORTS}/gif_recording_desktop.txt,txt -platform windows)
        set_tests_properties(gif_recording_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        # The application object with its tray, hotkeys and capture on the real desktop.
        qt_add_executable(jietuba_main_app_tests tests/test_main_app.cpp ${JIETUBA_ICON_QRC})
        set_target_properties(jietuba_main_app_tests PROPERTIES AUTORCC ON)
        target_link_libraries(jietuba_main_app_tests PRIVATE jietuba_lib Qt6::Test)
        target_compile_definitions(jietuba_main_app_tests PRIVATE JIETUBA_BARCODE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
        add_test(NAME main_app_desktop COMMAND jietuba_main_app_tests -o ${JIETUBA_TEST_REPORTS}/main_app_desktop.txt,txt -platform windows)
        set_tests_properties(main_app_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        # Long screenshots of a page window on the real screen.
        qt_add_executable(jietuba_scroll_capture_tests tests/test_scroll_capture.cpp)
        target_link_libraries(jietuba_scroll_capture_tests PRIVATE jietuba_lib Qt6::Test)
        add_test(NAME scroll_capture_desktop COMMAND jietuba_scroll_capture_tests -o ${JIETUBA_TEST_REPORTS}/scroll_capture_desktop.txt,txt -platform windows)
        set_tests_properties(scroll_capture_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        # Window, control and pointer lookup for smart selection, on real windows.
        qt_add_executable(jietuba_capture_finder_tests tests/test_capture_finders.cpp)
        target_link_libraries(jietuba_capture_finder_tests PRIVATE jietuba_lib Qt6::Test)
        add_test(NAME capture_finders_desktop COMMAND jietuba_capture_finder_tests -o ${JIETUBA_TEST_REPORTS}/capture_finders_desktop.txt,txt -platform windows)
        set_tests_properties(capture_finders_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        # System hotkeys, side buttons and hook combinations through real input.
        qt_add_executable(jietuba_shortcut_manager_tests tests/test_shortcut_manager.cpp)
        target_link_libraries(jietuba_shortcut_manager_tests PRIVATE jietuba_lib Qt6::Test)
        add_test(NAME shortcut_manager_desktop COMMAND jietuba_shortcut_manager_tests -o ${JIETUBA_TEST_REPORTS}/shortcut_manager_desktop.txt,txt -platform windows)
        set_tests_properties(shortcut_manager_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        # The input hook thread on the real desktop, driven by injected input.
        qt_add_executable(jietuba_input_hooks_tests tests/test_input_hooks.cpp)
        target_link_libraries(jietuba_input_hooks_tests PRIVATE jietuba_lib Qt6::Test)
        add_test(NAME input_hooks_desktop COMMAND jietuba_input_hooks_tests -o ${JIETUBA_TEST_REPORTS}/input_hooks_desktop.txt,txt -platform windows)
        set_tests_properties(input_hooks_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        # Process, registry and window helpers, crash logs and the single-instance rule.
        qt_add_executable(jietuba_platform_tests tests/test_platform.cpp)
        target_link_libraries(jietuba_platform_tests PRIVATE jietuba_lib Qt6::Test)
        add_test(NAME platform_desktop COMMAND jietuba_platform_tests -o ${JIETUBA_TEST_REPORTS}/platform_desktop.txt,txt -platform windows)
        set_tests_properties(platform_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
        # Smart selection snapping to real windows and controls in a full-screen capture.
        qt_add_executable(jietuba_smart_selection_tests tests/test_smart_selection.cpp)
        target_link_libraries(jietuba_smart_selection_tests PRIVATE jietuba_lib Qt6::Test)
        add_test(NAME smart_selection_desktop COMMAND jietuba_smart_selection_tests -o ${JIETUBA_TEST_REPORTS}/smart_selection_desktop.txt,txt -platform windows)
        set_tests_properties(smart_selection_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()
    qt_add_executable(jietuba_clipboard_tests tests/test_clipboard.cpp)
    target_link_libraries(jietuba_clipboard_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_clipboard_tests PRIVATE JIETUBA_CLIPBOARD_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/clipboard.json")
    add_test(NAME python_clipboard_parity COMMAND jietuba_clipboard_tests -o ${JIETUBA_TEST_REPORTS}/python_clipboard_parity.txt,txt)
    set_tests_properties(python_clipboard_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_clipboard_ui_tests tests/test_clipboard_ui.cpp)
    target_link_libraries(jietuba_clipboard_ui_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_ui COMMAND jietuba_clipboard_ui_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_ui.txt,txt)
    set_tests_properties(clipboard_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_clipboard_controller_tests tests/test_clipboard_controller.cpp)
    target_link_libraries(jietuba_clipboard_controller_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_controller COMMAND jietuba_clipboard_controller_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_controller.txt,txt)
    set_tests_properties(clipboard_controller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_clipboard_selection_tests tests/test_clipboard_selection.cpp)
    target_link_libraries(jietuba_clipboard_selection_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_selection COMMAND jietuba_clipboard_selection_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_selection.txt,txt)
    set_tests_properties(clipboard_selection PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_clipboard_window_tests tests/test_clipboard_window.cpp)
    target_link_libraries(jietuba_clipboard_window_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_window COMMAND jietuba_clipboard_window_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_window.txt,txt)
    set_tests_properties(clipboard_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # Whether the selection bar's texts fit needs real fonts.
        add_test(NAME clipboard_window_desktop COMMAND jietuba_clipboard_window_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_window_desktop.txt,txt -platform windows theSelectionBarFitsEveryLanguageInTheNarrowestWindow)
        set_tests_properties(clipboard_window_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()
    qt_add_executable(jietuba_clipboard_manage_tests tests/test_clipboard_manage.cpp)
    target_link_libraries(jietuba_clipboard_manage_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_manage COMMAND jietuba_clipboard_manage_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_manage.txt,txt)
    set_tests_properties(clipboard_manage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_clipboard_backend_tests tests/test_clipboard_backend.cpp)
    target_link_libraries(jietuba_clipboard_backend_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_backend COMMAND jietuba_clipboard_backend_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_backend.txt,txt)
    set_tests_properties(clipboard_backend PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_clipboard_widgets_tests tests/test_clipboard_widgets.cpp)
    target_link_libraries(jietuba_clipboard_widgets_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_widgets COMMAND jietuba_clipboard_widgets_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_widgets.txt,txt)
    set_tests_properties(clipboard_widgets PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_clipboard_drag_tests tests/test_clipboard_drag.cpp)
    target_link_libraries(jietuba_clipboard_drag_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME clipboard_drag COMMAND jietuba_clipboard_drag_tests -o ${JIETUBA_TEST_REPORTS}/clipboard_drag.txt,txt)
    set_tests_properties(clipboard_drag PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_settings_window_tests tests/test_settings_window.cpp)
    target_link_libraries(jietuba_settings_window_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME settings_window COMMAND jietuba_settings_window_tests -o ${JIETUBA_TEST_REPORTS}/settings_window.txt,txt)
    set_tests_properties(settings_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_data_folder_tests tests/test_data_folder.cpp)
    target_link_libraries(jietuba_data_folder_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME data_folder COMMAND jietuba_data_folder_tests -o ${JIETUBA_TEST_REPORTS}/data_folder.txt,txt)
    set_tests_properties(data_folder PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_canvas_view_tests tests/test_canvas_view.cpp)
    target_link_libraries(jietuba_canvas_view_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME canvas_view COMMAND jietuba_canvas_view_tests -o ${JIETUBA_TEST_REPORTS}/canvas_view.txt,txt)
    set_tests_properties(canvas_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_canvas_items_tests tests/test_canvas_items.cpp)
    target_link_libraries(jietuba_canvas_items_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME canvas_items COMMAND jietuba_canvas_items_tests -o ${JIETUBA_TEST_REPORTS}/canvas_items.txt,txt)
    set_tests_properties(canvas_items PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_screenshot_window_tests tests/test_screenshot_window.cpp)
    qt_add_executable(jietuba_msaa_tests tests/test_msaa_element_finder.cpp)
    target_link_libraries(jietuba_msaa_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_msaa_tests PRIVATE JIETUBA_MSAA_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/msaa.json")
    add_test(NAME msaa_elements COMMAND jietuba_msaa_tests -o ${JIETUBA_TEST_REPORTS}/msaa_elements.txt,txt)
    set_tests_properties(msaa_elements PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    target_link_libraries(jietuba_screenshot_window_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME screenshot_window COMMAND jietuba_screenshot_window_tests -o ${JIETUBA_TEST_REPORTS}/screenshot_window.txt,txt)
    set_tests_properties(screenshot_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_translation_ui_tests tests/test_translation_ui.cpp)
    target_link_libraries(jietuba_translation_ui_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME translation_ui COMMAND jietuba_translation_ui_tests -o ${JIETUBA_TEST_REPORTS}/translation_ui.txt,txt)
    set_tests_properties(translation_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_translation_speech_tests tests/test_translation_speech.cpp)
    target_link_libraries(jietuba_translation_speech_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_translation_speech_tests PRIVATE JIETUBA_TRANSLATION_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/translation.json")
    add_test(NAME translation_speech COMMAND jietuba_translation_speech_tests -o ${JIETUBA_TEST_REPORTS}/translation_speech.txt,txt)
    set_tests_properties(translation_speech PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_result_windows_tests tests/test_result_windows.cpp)
    target_link_libraries(jietuba_result_windows_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_result_windows_tests PRIVATE JIETUBA_BARCODE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME result_windows COMMAND jietuba_result_windows_tests -o ${JIETUBA_TEST_REPORTS}/result_windows.txt,txt)
    set_tests_properties(result_windows PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_ui_widgets_tests tests/test_ui_widgets.cpp)
    target_link_libraries(jietuba_ui_widgets_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_ui_widgets_tests PRIVATE JIETUBA_TRANSLATIONS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources/translations")
    add_test(NAME ui_widgets COMMAND jietuba_ui_widgets_tests -o ${JIETUBA_TEST_REPORTS}/ui_widgets.txt,txt)
    set_tests_properties(ui_widgets PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # The eyedropper's click catcher needs real windows and a real pointer.
        add_test(NAME ui_widgets_desktop COMMAND jietuba_ui_widgets_tests -o ${JIETUBA_TEST_REPORTS}/ui_widgets_desktop.txt,txt -platform windows eyedropperCatcherTakesTheClickOnTheDesktop)
        set_tests_properties(ui_widgets_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()
    qt_add_executable(jietuba_save_image_dialog_tests tests/test_save_image_dialog.cpp)
    target_link_libraries(jietuba_save_image_dialog_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME save_image_dialog COMMAND jietuba_save_image_dialog_tests -o ${JIETUBA_TEST_REPORTS}/save_image_dialog.txt,txt)
    set_tests_properties(save_image_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # The PP-OCR pack is not in the repository; the PP-OCR cases copy it from this folder when set.
    # CI names it in the environment.
    set(JIETUBA_PPOCR_MODELS "$ENV{JIETUBA_PPOCR_MODELS}" CACHE PATH "Folder holding the PP-OCR pack (models and onnxruntime.dll) for the OCR tests")
    qt_add_executable(jietuba_ocr_manager_tests tests/test_ocr_manager.cpp)
    target_link_libraries(jietuba_ocr_manager_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_ocr_manager_tests PRIVATE JIETUBA_OCR_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_test(NAME ocr_manager COMMAND jietuba_ocr_manager_tests -o ${JIETUBA_TEST_REPORTS}/ocr_manager.txt,txt)
    set_tests_properties(ocr_manager PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;JIETUBA_PPOCR_MODELS=${JIETUBA_PPOCR_MODELS}")
    # Stand-in oneocr.dll files: the variants lack one export each (see tests/fake_oneocr.cpp).
    foreach(variant IN ITEMS full legacy broken)
        add_library(fake_oneocr_${variant} SHARED tests/fake_oneocr.cpp)
        # The DLLs share one file name, so their import libraries and symbols get their own folders too.
        set(fake_dir "${CMAKE_CURRENT_BINARY_DIR}/fake_oneocr/${variant}")
        set_target_properties(fake_oneocr_${variant} PROPERTIES OUTPUT_NAME oneocr
            RUNTIME_OUTPUT_DIRECTORY "${fake_dir}" ARCHIVE_OUTPUT_DIRECTORY "${fake_dir}" PDB_OUTPUT_DIRECTORY "${fake_dir}")
        string(TOUPPER ${variant} variant_upper)
        target_compile_definitions(fake_oneocr_${variant} PRIVATE FAKE_ONEOCR_${variant_upper})
    endforeach()
    qt_add_executable(jietuba_snipping_tool_ocr_tests tests/test_snipping_tool_ocr.cpp)
    target_link_libraries(jietuba_snipping_tool_ocr_tests PRIVATE jietuba_lib Qt6::Test)
    add_dependencies(jietuba_snipping_tool_ocr_tests fake_oneocr_full fake_oneocr_legacy fake_oneocr_broken)
    target_compile_definitions(jietuba_snipping_tool_ocr_tests PRIVATE
        JIETUBA_FAKE_ONEOCR_FULL_DIR="$<TARGET_FILE_DIR:fake_oneocr_full>"
        JIETUBA_FAKE_ONEOCR_LEGACY_DIR="$<TARGET_FILE_DIR:fake_oneocr_legacy>"
        JIETUBA_FAKE_ONEOCR_BROKEN_DIR="$<TARGET_FILE_DIR:fake_oneocr_broken>")
    add_test(NAME snipping_tool_ocr COMMAND jietuba_snipping_tool_ocr_tests -o ${JIETUBA_TEST_REPORTS}/snipping_tool_ocr.txt,txt)
    # Loading an engine clears the shared test cache of other versions' copies, so the two do not run together.
    set_tests_properties(snipping_tool_ocr PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RESOURCE_LOCK oneocr_test_cache)
    set_tests_properties(ocr_manager PROPERTIES RESOURCE_LOCK oneocr_test_cache)
    add_dependencies(jietuba_ocr_manager_tests fake_oneocr_full)
    target_compile_definitions(jietuba_ocr_manager_tests PRIVATE JIETUBA_FAKE_ONEOCR_FULL_DIR="$<TARGET_FILE_DIR:fake_oneocr_full>")
    qt_add_executable(jietuba_ocr_bands_tests tests/test_ocr_bands.cpp)
    target_link_libraries(jietuba_ocr_bands_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME ocr_bands COMMAND jietuba_ocr_bands_tests -o ${JIETUBA_TEST_REPORTS}/ocr_bands.txt,txt)
    set_tests_properties(ocr_bands PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" RESOURCE_LOCK oneocr_test_cache)
    qt_add_executable(jietuba_ppocr_models_tests tests/test_ppocr_models.cpp)
    target_link_libraries(jietuba_ppocr_models_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME ppocr_models COMMAND jietuba_ppocr_models_tests -o ${JIETUBA_TEST_REPORTS}/ppocr_models.txt,txt)
    set_tests_properties(ppocr_models PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;JIETUBA_PPOCR_MODELS=${JIETUBA_PPOCR_MODELS}")
    qt_add_executable(jietuba_bundled_runtime_tests tests/test_bundled_runtime.cpp)
    target_link_libraries(jietuba_bundled_runtime_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME bundled_runtime COMMAND jietuba_bundled_runtime_tests -o ${JIETUBA_TEST_REPORTS}/bundled_runtime.txt,txt)
    set_tests_properties(bundled_runtime PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # The HTTP client and the providers with a configurable address, against a local server.
    qt_add_executable(jietuba_http_provider_tests tests/test_http_providers.cpp)
    target_link_libraries(jietuba_http_provider_tests PRIVATE jietuba_lib Qt6::Test Qt6::Network)
    add_test(NAME http_providers COMMAND jietuba_http_provider_tests -o ${JIETUBA_TEST_REPORTS}/http_providers.txt,txt)
    set_tests_properties(http_providers PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_logger_tests tests/test_logger.cpp)
    target_link_libraries(jietuba_logger_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME logger COMMAND jietuba_logger_tests -o ${JIETUBA_TEST_REPORTS}/logger.txt,txt)
    set_tests_properties(logger PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # A scripted stand-in for the updater helper (not *_tests: it runs in the test's sandbox).
    add_executable(jietuba_fake_updater tests/fake_updater.cpp)
    target_link_libraries(jietuba_fake_updater PRIVATE Qt6::Core)
    qt_add_executable(jietuba_update_tests tests/test_update.cpp)
    target_link_libraries(jietuba_update_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_update_tests PRIVATE JIETUBA_FAKE_UPDATER_PATH="$<TARGET_FILE:jietuba_fake_updater>")
    add_dependencies(jietuba_update_tests jietuba_fake_updater)
    if(JIETUBA_UPDATES)
        qt_add_resources(jietuba_update_tests jietuba_updater_test PREFIX /updater BASE ${JIETUBA_UPDATER_DIR} FILES ${JIETUBA_UPDATER_EXE})
    endif()
    add_test(NAME updater COMMAND jietuba_update_tests -o ${JIETUBA_TEST_REPORTS}/updater.txt,txt)
    set_tests_properties(updater PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_pin_tests tests/test_pin.cpp)
    target_link_libraries(jietuba_pin_tests PRIVATE jietuba_lib Qt6::Test)
    add_test(NAME pin_window COMMAND jietuba_pin_tests -o ${JIETUBA_TEST_REPORTS}/pin_window.txt,txt)
    set_tests_properties(pin_window PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # Window styles only exist on real windows; the translation window needs a real screen's width beside the pin;
        # only Windows shows a pin without activating it.
        add_test(NAME pin_window_desktop COMMAND jietuba_pin_tests -o ${JIETUBA_TEST_REPORTS}/pin_window_desktop.txt,txt -platform windows clickThroughStyleSurvivesStayOnTopToggles topmostSuppressionUsesTheRealWindowStyle
            translatingAPinRecognizesThenTranslates aNewPinTakesKeyboardFocus)
        set_tests_properties(pin_window_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()
    # The offscreen run has two screens and changes them at run time through the offscreen platform's
    # native interface, a private header.
    find_package(Qt6 QUIET COMPONENTS GuiPrivate)
    qt_add_executable(jietuba_screen_annotation_tests tests/test_screen_annotation.cpp)
    target_link_libraries(jietuba_screen_annotation_tests PRIVATE jietuba_lib Qt6::Test Qt6::GuiPrivate mfplat mfreadwrite mfuuid)
    # The recording test runs the real MP4 worker, which is the application itself.
    target_compile_definitions(jietuba_screen_annotation_tests PRIVATE JIETUBA_APP_PATH="$<TARGET_FILE:jietuba_cpp>")
    add_dependencies(jietuba_screen_annotation_tests jietuba_cpp)
    add_test(NAME screen_annotation COMMAND jietuba_screen_annotation_tests -o ${JIETUBA_TEST_REPORTS}/screen_annotation.txt,txt)
    set_tests_properties(screen_annotation PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # Window styles, focus, hit testing and screen capture of the real layers.
        add_test(NAME screen_annotation_desktop COMMAND jietuba_screen_annotation_tests -o ${JIETUBA_TEST_REPORTS}/screen_annotation_desktop.txt,txt -platform windows)
        set_tests_properties(screen_annotation_desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows")
    endif()
    qt_add_executable(jietuba_parity_tests tests/test_parity.cpp)
    target_link_libraries(jietuba_parity_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_parity_tests PRIVATE JIETUBA_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/parity.json")
    add_test(NAME python_behavior_parity COMMAND jietuba_parity_tests -o ${JIETUBA_TEST_REPORTS}/python_behavior_parity.txt,txt)
    set_tests_properties(python_behavior_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    qt_add_executable(jietuba_canvas_parity_tests tests/test_canvas_parity.cpp)
    target_link_libraries(jietuba_canvas_parity_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_canvas_parity_tests PRIVATE JIETUBA_CANVAS_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/canvas.json")
    add_test(NAME python_canvas_parity COMMAND jietuba_canvas_parity_tests -o ${JIETUBA_TEST_REPORTS}/python_canvas_parity.txt,txt)
    set_tests_properties(python_canvas_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        add_test(NAME python_canvas_windows_parity COMMAND jietuba_canvas_parity_tests -o ${JIETUBA_TEST_REPORTS}/python_canvas_windows_parity.txt,txt -platform windows)
        set_tests_properties(python_canvas_windows_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=windows;JIETUBA_CANVAS_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/canvas-windows.json")
    endif()
    qt_add_executable(jietuba_freehand_tests tests/test_freehand.cpp)
    target_link_libraries(jietuba_freehand_tests PRIVATE jietuba_lib Qt6::Test)
    target_compile_definitions(jietuba_freehand_tests PRIVATE JIETUBA_FREEHAND_FIXTURE_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/freehand.json")
    add_test(NAME python_freehand_parity COMMAND jietuba_freehand_tests -o ${JIETUBA_TEST_REPORTS}/python_freehand_parity.txt,txt)
    set_tests_properties(python_freehand_parity PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Settings and data are the user's real ones: every test executable starts in
    # the throwaway sandbox of tests/test_sandbox.cpp, however it is launched.
    add_library(jietuba_test_sandbox OBJECT tests/test_sandbox.cpp)
    target_link_libraries(jietuba_test_sandbox PRIVATE jietuba_lib)
    if(JIETUBA_LEAK_CHECK)
        target_sources(jietuba_test_sandbox PRIVATE tests/leak_check.cpp)
        target_compile_definitions(jietuba_test_sandbox PRIVATE JIETUBA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    get_property(jietuba_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS jietuba_targets)
        if(target MATCHES "_tests$")
            target_link_libraries(${target} PRIVATE jietuba_test_sandbox)
            add_dependencies(${target} jietuba_native)
            if(JIETUBA_LEAK_CHECK)
                target_compile_options(${target} PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/tests/leak_check.h")
            endif()
        endif()
    endforeach()
    # ctest -j: the clipboard tests share the system clipboard, so they take turns. So do these others
    # that write it; a write waits only briefly for another holder, and a paste is then dropped.
    set(jietuba_clipboard_writers python_core_parity translation_ui pin_window screenshot_window)
    get_property(jietuba_tests DIRECTORY PROPERTY TESTS)
    foreach(test IN LISTS jietuba_tests)
        if(test MATCHES "clipboard" OR test IN_LIST jietuba_clipboard_writers)
            set_tests_properties(${test} PROPERTIES RESOURCE_LOCK system_clipboard)
        endif()
    endforeach()
endif()
install(TARGETS jietuba_cpp RUNTIME DESTINATION .)
