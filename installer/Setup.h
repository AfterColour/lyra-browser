// LyraSetup - Lyra's own small installer.
//
// One exe does everything:
//   LyraSetup.exe                 window: install / update (the files are inside it, see Payload.cpp)
//   LyraSetup.exe /S              install without a window
//   LyraSetup.exe /update         install without a window, then start Lyra again (auto-update)
//   LyraSetup.exe /update /norestart   the same without starting Lyra (it was closed by the user)
//   Uninstall.exe                 the same program without files inside: window to remove Lyra
//   Uninstall.exe /S              remove without a window
// Other switches: /dir=<folder> (where to install), /noregister (only copy the files: no
// shortcuts, no registry - used for testing), /nodesktop (no desktop shortcut).
// Everything goes into the user's own profile (%LOCALAPPDATA%\Programs\Lyra, HKCU): no
// administrator rights are needed.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// ---- text -------------------------------------------------------------------
bool UiRussian();
inline const wchar_t* Tr(const wchar_t* en, const wchar_t* ru) { return UiRussian() ? ru : en; }

// ---- payload: the files carried inside LyraSetup.exe -------------------------------
struct PayloadFile {
    std::wstring path;          // relative, with backslashes
    uint64_t offset = 0;        // into the data block
    uint64_t csize = 0, size = 0;
    uint8_t method = 0;         // 0 stored, 1 LZMS
};
struct Payload {
    std::wstring version;
    uint64_t totalSize = 0;     // unpacked bytes
    std::vector<PayloadFile> files;
    const uint8_t* data = nullptr;
    uint64_t dataSize = 0;
};
bool PayloadOpen(Payload& p);   // false: this exe carries no files (it is Uninstall.exe)
bool PayloadExtract(const Payload& p, const PayloadFile& f, const std::wstring& dest, std::wstring& err);

// ---- install / uninstall -----------------------------------------------------------
struct Options {
    std::wstring dir;
    bool desktopShortcut = true;
    bool registerApp = true;    // shortcuts + registry (off with /noregister)
    bool removeData = false;    // uninstall: also delete %LOCALAPPDATA%\Lyra
};
struct Progress {
    std::atomic<uint64_t> done{0};
    std::atomic<uint64_t> total{1};
};

std::wstring ExePath();
std::wstring DefaultInstallDir();
std::wstring InstalledDir();            // from the uninstall entry; empty if not installed
std::wstring InstalledVersion();
std::wstring UserDataDir();             // %LOCALAPPDATA%\Lyra
bool PathInside(const std::wstring& path, const std::wstring& dir);
bool LyraRunning(const std::wstring& dir);
bool CloseLyra(const std::wstring& dir, DWORD timeoutMs);   // asks Lyra to close, then ends what is left
bool Install(const Payload& p, const Options& o, Progress& pr, std::wstring& err);
bool Uninstall(const Options& o, Progress& pr, std::wstring& err);
void LaunchLyra(const std::wstring& dir);
void BuildMediaCache(const std::wstring& dir);   // Lyra.exe --gst-registry (GStreamer's plugin cache)
void OpenDefaultAppsSettings();

// ---- window ------------------------------------------------------------------------
enum class Mode { Install, Uninstall };
int RunUi(HINSTANCE hi, Mode mode, Payload* payload, const Options& opts);
