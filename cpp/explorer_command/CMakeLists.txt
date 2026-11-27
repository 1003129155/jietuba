# The Explorer command of the packaged build (MSIX): a COM DLL loaded by the package's surrogate process.
# It must not load Qt, so it is a plain Win32 target with a static runtime whatever the application uses.
set(JIETUBA_EXPLORER_COMMAND_SOURCES explorer_command_core.cpp explorer_command_core.h)

# The same logic for the tests, built like the application (the DLL's static runtime would not link
# into a test executable that uses the shared one).
add_library(jietuba_explorer_command_core STATIC ${JIETUBA_EXPLORER_COMMAND_SOURCES})
target_include_directories(jietuba_explorer_command_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(jietuba_explorer_command_core PRIVATE UNICODE _UNICODE)
set_target_properties(jietuba_explorer_command_core PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

add_library(jietuba_explorer_command SHARED ${JIETUBA_EXPLORER_COMMAND_SOURCES} explorer_command.cpp explorer_command.def)
target_include_directories(jietuba_explorer_command PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(jietuba_explorer_command PRIVATE UNICODE _UNICODE)
target_link_libraries(jietuba_explorer_command PRIVATE ole32 shell32 shlwapi uuid user32 kernel32)
set_target_properties(jietuba_explorer_command PROPERTIES
    AUTOMOC OFF AUTOUIC OFF AUTORCC OFF
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
    target_compile_options(jietuba_explorer_command_core PRIVATE /W4 /utf-8)
    target_compile_options(jietuba_explorer_command PRIVATE /W4 /utf-8)
endif()
