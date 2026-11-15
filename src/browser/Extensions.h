// Lyra — расширения браузера (стандарт WebExtensions: те же, что у Chrome и Firefox).
//
// Lyra runs extensions itself on top of WebKit (WebKit's own extension support is macOS-only).
// An extension is kept unpacked in %LOCALAPPDATA%\Lyra\Extensions\<id> (from a .crx, .zip or
// .xpi file, or copied from a folder). What works so far:
//   - content scripts and styles (manifest "content_scripts": matches / exclude_matches, js,
//     css, run_at, all_frames), each extension in an isolated world of its own
//     (WKUserScriptCreateWithSourceInWorld, an addition to Lyra's WebKit);
//   - their chrome.* / browser.* API (resources/extensions/api.js): runtime, storage, i18n.
//   - a background page (or service worker, run as a page) per extension, kept running
//     hidden, and options pages in a window of their own: the full chrome.* API there, with
//     messages between the parts (runtime / tabs.sendMessage), tabs, windows, scripting,
//     alarms, storage.onChanged everywhere; tab events.
// The toolbar button and its pop-up, ports (runtime.connect) and web requests come next.
#pragma once

#include "common/Platform.h"

struct ExtensionInfo {
    std::wstring id, dir, name, version, description;
    std::wstring icon;                      // path of the largest icon ("" if none)
    bool enabled = true;
    int manifestVersion = 2;
    int contentScripts = 0;                 // how many content_scripts entries
    std::vector<std::wstring> permissions;  // as listed (permissions + host_permissions)
};

void ExtensionsInit();                                  // after AdBlockInit (the shared user content controller)
const std::vector<ExtensionInfo>& ExtensionsList();
void ExtensionSetEnabled(const std::wstring& id, bool enabled);
void ExtensionRemove(const std::wstring& id);
bool ExtensionInstallFromFile();                        // Settings: .crx / .zip / .xpi (file dialog)
bool ExtensionInstallFromFolder();                      // Settings: an unpacked extension (folder dialog)
bool ExtensionInstall(const std::wstring& path, std::wstring& err);   // file or folder
void ExtensionsShutdown();
// What the extensions' tabs API reports (tabs.onCreated / onUpdated / onActivated / onRemoved).
struct Tab;
void ExtensionsOnTabCreated(Tab* t);
void ExtensionsOnTabUpdated(Tab* t);                     // loading, address or title changed
void ExtensionsOnTabActivated(Tab* t);
void ExtensionsOnTabRemoved(unsigned long long id);
