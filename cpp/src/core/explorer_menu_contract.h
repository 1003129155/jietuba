#pragma once
// What the application and the packaged Explorer command agree on. Plain C++ without Qt: the
// command is a DLL loaded into Explorer's COM surrogate.
namespace jietuba::explorer_menu {
// Each file to pin follows its own --pin; Explorer starts one process per file for the
// registry menu, the packaged command one process for the whole selection.
inline constexpr const wchar_t *PIN_ARGUMENT = L"--pin";

// Lower case, without the dot. Pictures pin as images or animations; text files as text pins.
inline constexpr const wchar_t *const IMAGE_EXTENSIONS[] = {L"png", L"jpg", L"jpeg", L"jpe", L"jfif", L"bmp", L"dib",
                                                            L"gif", L"webp", L"tif", L"tiff", L"ico", L"svg"};
inline constexpr const wchar_t *const TEXT_EXTENSIONS[] = {L"txt", L"md", L"markdown", L"log", L"csv",
                                                           L"json", L"xml", L"html", L"htm"};

enum class Mode { Off, Images, ImagesAndText };
// The stored words, indexed by Mode.
inline constexpr const char *const MODE_WORDS[] = {"off", "images", "images_text"};

// The packaged build cannot write the registry menu; it keeps the choice in this file in its
// LocalState folder (%LOCALAPPDATA%\Packages\<family name>\LocalState), UTF-8 without BOM:
// line 1 the mode word, line 2 the menu title in the interface language, line 3 the
// executable's file name, which sits in the same folder as the command DLL.
inline constexpr const wchar_t *PREFERENCE_FILE = L"explorer_menu.txt";
}
