cmake_minimum_required(VERSION 3.26)   # copy_directory_if_different
project(Lyra LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimised build with a .pdb, so crash offsets in tabs.log (Lyra.exe+0x...)
# can be resolved. Override with -DCMAKE_BUILD_TYPE=Release / Debug.
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif ()

# ----------------------------------------------------------------------------
#  WebKit (Windows port) build output
# ----------------------------------------------------------------------------
set(WEBKIT_BUILD_DIR "C:/WebKit/WebKit/WebKitBuild/ReleaseGst" CACHE PATH
    "WebKit build directory (contains bin/, lib/ and WebKit/Headers)")
set(WEBKIT_BIN_DIR "${WEBKIT_BUILD_DIR}/bin")
set(WEBKIT_LIB_DIR "${WEBKIT_BUILD_DIR}/lib")

foreach (required
        "${WEBKIT_BUILD_DIR}/WebKit/Headers/WebKit/WebKit2_C.h"
        "${WEBKIT_LIB_DIR}/WebKit2.lib"
        "${WEBKIT_LIB_DIR}/JavaScriptCore.lib"
        "${WEBKIT_BIN_DIR}/WebKit2.dll")
    if (NOT EXISTS "${required}")
        message(FATAL_ERROR "WebKit build not found: ${required}\n"
                            "Build WebKit first or pass -DWEBKIT_BUILD_DIR=<path>.")
    endif ()
endforeach ()

# Folder the browser runs from. The exe is written here directly and the WebKit
# runtime (DLLs, helper processes, resources) is kept in sync with WEBKIT_BIN_DIR,
# so run/ can never end up with DLLs from an older WebKit build.
set(LYRA_RUN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/run" CACHE PATH "Output / run folder")

# ----------------------------------------------------------------------------
#  Lyra.exe
# ----------------------------------------------------------------------------
# Sources are grouped by folder (see src/README.md):
#   app/      startup helpers: log, crash handler, environment
#   core/     settings, constants, string and URL helpers (no windows)
#   browser/  tabs and everything that talks to WebKit
#   ui/       main window, tab strip/toolbar, address bar, theme, dialogs
#   ui/settings/  the settings window
# A new .cpp/.h under src/ is picked up automatically on the next build.
file(GLOB_RECURSE LYRA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h"
)

add_executable(Lyra WIN32
    ${LYRA_SOURCES}
    src/Lyra.exe.manifest     # DPI awareness, Common Controls v6, Windows 10/11
)
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" FILES ${LYRA_SOURCES})

# Windows, GDI+ and WebKit headers are compiled once and reused by every source file.
target_precompile_headers(Lyra PRIVATE src/common/Platform.h)

target_include_directories(Lyra PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WEBKIT_BUILD_DIR}/WebKit/Headers"
    "${WEBKIT_BUILD_DIR}/JavaScriptCore/Headers"
    "${WEBKIT_BUILD_DIR}/WTF/Headers"
)

target_compile_definitions(Lyra PRIVATE
    UNICODE _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_compile_options(Lyra PRIVATE
    /utf-8      # source files are UTF-8 (needed once UI strings contain Cyrillic etc.)
    /W3
)

# Always the release DLL CRT, like WebKit itself, even for a Debug build of Lyra.
set_target_properties(Lyra PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreadedDLL"
    RUNTIME_OUTPUT_DIRECTORY "${LYRA_RUN_DIR}$<0:>"   # $<0:> = no per-config subfolder
)

target_link_libraries(Lyra PRIVATE
    "${WEBKIT_LIB_DIR}/WebKit2.lib"
    "${WEBKIT_LIB_DIR}/JavaScriptCore.lib"
    user32 gdi32 gdiplus dwmapi
    shell32 shlwapi comdlg32 comctl32 winspool
    ole32 oleaut32 uuid advapi32
)

# ----------------------------------------------------------------------------
#  WebKit runtime -> run/
# ----------------------------------------------------------------------------
file(GLOB WEBKIT_RUNTIME_FILES CONFIGURE_DEPENDS
    "${WEBKIT_BIN_DIR}/*.dll"
    "${WEBKIT_BIN_DIR}/WebKitWebProcess.exe"
    "${WEBKIT_BIN_DIR}/WebKitNetworkProcess.exe"
    "${WEBKIT_BIN_DIR}/WebKitGPUProcess.exe"
)
# Test harness / MiniBrowser bundles are not needed by the browser.
list(FILTER WEBKIT_RUNTIME_FILES EXCLUDE REGEX "/(Test|MiniBrowser|ImageDiff)[^/]*$")

add_custom_target(webkit_runtime ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${LYRA_RUN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${WEBKIT_RUNTIME_FILES} "${LYRA_RUN_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${WEBKIT_BIN_DIR}/WebKit.resources" "${LYRA_RUN_DIR}/WebKit.resources"
    COMMENT "Syncing WebKit runtime into ${LYRA_RUN_DIR}"
    VERBATIM
)
add_dependencies(Lyra webkit_runtime)

# ----------------------------------------------------------------------------
#  Lyra's own pages (start page, welcome screen) -> run/resources
# ----------------------------------------------------------------------------
add_custom_target(lyra_resources ALL
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/resources" "${LYRA_RUN_DIR}/resources"
    COMMENT "Copying Lyra resources into ${LYRA_RUN_DIR}/resources"
    VERBATIM
)
add_dependencies(Lyra lyra_resources)

# ----------------------------------------------------------------------------
#  GStreamer runtime (only when WEBKIT_BUILD_DIR is a WebKit built with
#  -DUSE_GSTREAMER=ON, see C:/WebKit/build-webkit-gst.bat)
# ----------------------------------------------------------------------------
# GStreamer loads its plugins at runtime, so neither vcpkg's app-local deployment
# nor the DLL glob above picks up the plugins or the DLLs only they need (FFmpeg...).
set(WEBKIT_VCPKG_DIR "${WEBKIT_BUILD_DIR}/vcpkg_installed/x64-windows-webkit")
if (EXISTS "${WEBKIT_VCPKG_DIR}/plugins/gstreamer")
    message(STATUS "WebKit has the GStreamer media backend: deploying GStreamer plugins")
    file(GLOB VCPKG_RUNTIME_DLLS CONFIGURE_DEPENDS "${WEBKIT_VCPKG_DIR}/bin/*.dll")
    set(_gst_commands
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${VCPKG_RUNTIME_DLLS} "${LYRA_RUN_DIR}"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                "${WEBKIT_VCPKG_DIR}/plugins/gstreamer" "${LYRA_RUN_DIR}/gstreamer-1.0"
    )
    if (EXISTS "${WEBKIT_VCPKG_DIR}/tools/gstreamer/gst-plugin-scanner.exe")
        list(APPEND _gst_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${WEBKIT_VCPKG_DIR}/tools/gstreamer/gst-plugin-scanner.exe" "${LYRA_RUN_DIR}")
    endif ()
    add_custom_target(gstreamer_runtime ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${LYRA_RUN_DIR}"
        ${_gst_commands}
        COMMENT "Syncing GStreamer runtime into ${LYRA_RUN_DIR}"
        VERBATIM
    )
    add_dependencies(Lyra gstreamer_runtime)
endif ()
