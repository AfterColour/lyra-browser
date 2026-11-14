// Lyra - automatic updates.
//
// An installed Lyra (one with install.files next to Lyra.exe, put there by LyraSetup) looks at
// https://aftercolour.github.io/lyra-browser/update.json once a day. When it names a newer
// version and its ECDSA signature matches the key compiled in (app/UpdateKey.h), the installer
// is downloaded to %LOCALAPPDATA%\Lyra\Update and checked against the SHA-256 in the manifest.
// Then the update is "ready": the More menu and Settings > About offer to restart; otherwise it
// is installed quietly when Lyra closes. A development build (run\) never updates itself.
// LYRA_UPDATE_URL overrides the manifest address (a local file path works too, for testing).
#pragma once
#include <string>

void UpdaterInit();                    // after the main window exists: first check soon, then daily
void UpdaterTimer();                   // TIMER_UPDATE (hourly)
void UpdaterCheckNow();                // Settings > About "Check for updates"
void UpdaterChanged();                 // WM_APP_UPDATE: the state changed (repaints what shows it)
bool UpdaterSupported();               // an installed copy, not a development build
bool UpdaterReady();                   // an update is downloaded and checked
std::wstring UpdaterReadyVersion();
std::wstring UpdaterStatus();          // one line for Settings > About
bool UpdaterBusy();                    // checking or downloading
void UpdaterRestartAndInstall();       // closes Lyra, installs, starts Lyra again with the same tabs
void UpdaterOnExit();                  // Lyra is closing: install a ready update quietly
bool UpdaterJustRestarted();           // this start follows UpdaterRestartAndInstall (restore every tab)
