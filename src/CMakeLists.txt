# Source list for the Rift executable. See the root CMakeLists.txt for the
# project() declaration and STYLE.md for the naming/formatting convention every file
# here follows. This list grows as the rewrite (see the original f4 repo's
# FORK_WITH_CLASSES.md for the migration order) adds more of the widget tree.

set(RIFT_GFX_BACKEND "d3d11" CACHE STRING "Graphics backend implementing gfx/renderer.h's IRenderer")
set_property(CACHE RIFT_GFX_BACKEND PROPERTY STRINGS d3d11 gl)

if (RIFT_GFX_BACKEND STREQUAL "d3d11")
  set(RIFT_GFX_SOURCES "gfx/d3d11/renderer_d3d11.cpp")
elseif (RIFT_GFX_BACKEND STREQUAL "gl")
  message(FATAL_ERROR "RIFT_GFX_BACKEND=gl has no implementation yet")
else()
  message(FATAL_ERROR "Unknown RIFT_GFX_BACKEND: ${RIFT_GFX_BACKEND}")
endif()

add_executable (Rift
  "asset_manager.cpp"
  "font_manager.cpp"
  "core/memory_arena.cpp"
  "core/account.cpp"
  "core/animator.cpp"
  "core/atomic_file.cpp"
  "core/crash_handler.cpp"
  "core/crypto.cpp"
  "core/debug_log.cpp"
  "core/login_attempt.cpp"
  "core/master_key.cpp"
  "core/riot_client.cpp"
  "core/semver.cpp"
  "core/storage.cpp"
  "core/ui_automation.cpp"
  "core/update_manifest.cpp"
  "core/updater.cpp"
  "window.cpp"
  "ui/widget.cpp"
  "ui/widget_stack.cpp"
  "ui/scrollable.cpp"
  "ui/hoverable.cpp"
  "ui/draggable.cpp"
  "ui/title_bar.cpp"
  "ui/settings_menu.cpp"
  "ui/context_menu.cpp"
  "ui/text_input.cpp"
  "ui/color_picker.cpp"
  "ui/game_select_popup.cpp"
  "ui/unlock_screen.cpp"
  "ui/update_overlay.cpp"
  "tray.cpp"
  "gfx/texture.cpp"
  "gfx/font.cpp"
  "ui/draw_list.cpp"
  "ui/layout.cpp"
  "ui/text.cpp"
  "ui/carousel.cpp"
  "ui/settings_panel.cpp"
  "ui/account_modal.cpp"
  "main.cpp"
  "third_party/stb_impl.cpp"
  ${RIFT_GFX_SOURCES}
  # See the original f4 repo's CMakeLists.txt for why this must ride along as a
  # .manifest-suffixed source (CMake's Ninja+MSVC generator threads it into the
  # linker's manifest-merge pass) rather than a hand-authored resource script.
  "app.manifest"
  # The file-version resource (FileDescription/FileVersion/ProductVersion/Language) -
  # see its own header comment. A plain .rc source compiles through rc.exe and links
  # straight into Rift.exe like any other object file, no special CMake wiring needed
  # the way app.manifest's merge pass does.
  "app.rc"
)

target_include_directories(Rift PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_definitions(Rift PRIVATE UNICODE _UNICODE NOMINMAX)

# libsodium: prebuilt static lib, x64 Debug/Release only - the only platform this project
# targets. Trimmed from upstream's official libsodium-*-msvc.zip (which also ships Win32/
# ARM64, and a dynamic/DLL and an LTCG variant of every config) down to just those two
# static builds plus the public headers - see third_party/libsodium's own directory for
# what was kept. The LTCG variant was deliberately skipped too: it requires the consuming
# project to also build with /GL, which this one doesn't. SODIUM_STATIC tells sodium.h's
# own export.h to skip the __declspec(dllimport) it'd otherwise assume for the DLL variant
# this project isn't using, so Rift.exe stays a single self-contained binary like every
# other dependency here (no libsodium.dll to ship alongside it).
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(SODIUM_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsodium/lib/x64-debug")
else()
  set(SODIUM_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsodium/lib/x64-release")
endif()
target_include_directories(Rift PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsodium/include")
target_compile_definitions(Rift PRIVATE SODIUM_STATIC)
target_link_libraries(Rift PRIVATE "${SODIUM_LIB_DIR}/libsodium.lib")

# dwmapi: DwmExtendFrameIntoClientArea, keeps the native drop shadow / rounded corners on
# the custom-drawn title bar window.
# UIAutomationCore: the IUIAutomation client interface - core/ui_automation.cpp.
# ole32/oleaut32: CoInitializeEx/CoCreateInstance and BSTR/VARIANT handling that UI
# Automation's COM interface needs - also core/ui_automation.cpp.
# dbghelp: MiniDumpWriteDump - core/crash_handler.cpp's own crash-dump writer.
# comctl32: TaskDialogIndirect - also core/crash_handler.cpp, for its crash dialog.
# winhttp: WinHttpOpen/Connect/OpenRequest/SendRequest/ReadData - core/updater.cpp's own
# manifest fetch and exe download, chosen over pulling in libcurl since it's already part of
# every Windows install, validates TLS certificates by default, and honors the system proxy
# configuration out of the box.
target_link_libraries(Rift PRIVATE shell32 dwmapi UIAutomationCore ole32 oleaut32 dbghelp comctl32 winhttp)

# core/login_attempt.cpp uses std::thread.
find_package(Threads REQUIRED)
target_link_libraries(Rift PRIVATE Threads::Threads)

if (RIFT_GFX_BACKEND STREQUAL "d3d11")
  target_link_libraries(Rift PRIVATE d3d11 dxgi d3dcompiler)
endif()

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET Rift PROPERTY CXX_STANDARD 26)
endif()

# Release is a GUI-subsystem exe (no console window ever flashes up when launched, e.g.
# from a shortcut or the tray) - kept console-subsystem in Debug so the std::println
# startup-failure diagnostics in main.cpp still have somewhere to go. /ENTRY:mainCRTStartup
# overrides the linker default WinMain entry WIN32_EXECUTABLE otherwise implies, so main()
# stays the one real entry point either way - nothing above this needs its own WinMain.
if (CMAKE_BUILD_TYPE STREQUAL "Release")
  set_property(TARGET Rift PROPERTY WIN32_EXECUTABLE TRUE)
  target_link_options(Rift PRIVATE "/ENTRY:mainCRTStartup")
endif()
