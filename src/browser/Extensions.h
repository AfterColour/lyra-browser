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
//   - ports (runtime.connect / tabs.connect) between all of these;
//   - the toolbar button: icon, title, badge (per tab or for all), its pop-up (sized to its
//     content, like Chrome's) or action.onClicked.
// Installing from Chrome Web Store / Firefox Add-ons and .crx / .xpi links: browser/ExtensionStore.h.
// Web requests (webRequest, declarativeNetRequest) come next.
#pragma once

#include "common/Platform.h"
#include <WebKit/WKBase.h>
#include <functional>

struct ExtensionInfo {
    std::wstring id, dir, name, version, description;
    std::wstring icon;                      // path of the largest icon ("" if none)
    bool enabled = true;
    int manifestVersion = 2;
    int contentScripts = 0;                 // how many content_scripts entries
    std::vector<std::wstring> permissions;  // as listed (permissions + host_permissions)
    bool hasAction = false;                 // a toolbar button (manifest action / browser_action / page_action)
    bool pinned = true;                     // ...shown on the toolbar
};

void ExtensionsSetupPageConfiguration(WKPageConfigurationRef conf);   // tabs: extensions' files their pages may load
void ExtensionsInit();                                  // after AdBlockInit (the shared user content controller)
const std::vector<ExtensionInfo>& ExtensionsList();
void ExtensionSetEnabled(const std::wstring& id, bool enabled);
void ExtensionRemove(const std::wstring& id);
bool ExtensionInstallFromFile();                        // Settings: .crx / .zip / .xpi (file dialog)
bool ExtensionInstallFromFolder();                      // Settings: an unpacked extension (folder dialog)
bool ExtensionInstall(const std::wstring& path, std::wstring& err,  // a folder, .crx, .xpi or .zip
                      const std::function<bool(const ExtensionInfo&)>& confirm = {});  // false: not installed, err empty
void ExtensionsShutdown();
// What the extensions' tabs API reports (tabs.onCreated / onUpdated / onActivated / onRemoved).
struct Tab;
void ExtensionsOnTabCreated(Tab* t);
void ExtensionsOnTabUpdated(Tab* t);                     // loading, address or title changed
void ExtensionsOnTabActivated(Tab* t);
void ExtensionsOnTabCommitted(Tab* t);            // a new page: its scripts' ports are gone
void ExtensionsOnTabRemoved(unsigned long long id);
void ExtensionsDnrReady(LPARAM lParam);    // WM_APP_DNR: an extension's network rules are converted

// The extensions' toolbar buttons (left of the downloads button), in order, for the open tab.
struct ExtensionButton {
    std::wstring id, title, badge;
    Bitmap* icon = nullptr;                 // kept by Extensions.cpp; null: no icon it could read
    Color badgeColor = Color(0, 0, 0, 0);   // alpha 0: the accent colour
    Color badgeTextColor = Color(255, 255, 255, 255);
    bool enabled = true;
    bool open = false;                      // its pop-up is showing
};
std::vector<ExtensionButton> ExtensionButtons();
void ExtensionButtonClick(int index);       // index into ExtensionButtons() and g_L.ext
void ExtensionButtonMenu(int index);        // right click: options, hide, manage, remove
void ExtensionSetPinned(const std::wstring& id, bool pinned);

// Context menu items of extensions (chrome.contextMenus): what the page's menu shows, and a choice.
struct ExtensionMenuContext {
    std::wstring pageUrl, link, linkText, image, media, selectionText;
    bool editable = false, selection = false;
    bool action = false;                 // the menu of the extension's toolbar button
};
struct ExtensionMenuEntry {
    std::wstring extId, title;
    std::string itemId;
    bool separator = false, checkable = false, checked = false, enabled = true;
    std::vector<ExtensionMenuEntry> children;
};
struct ExtensionMenuGroup { std::wstring extId, name; std::vector<ExtensionMenuEntry> items; };
bool ExtensionsHaveMenus();
std::vector<ExtensionMenuGroup> ExtensionMenus(const ExtensionMenuContext& c);
void ExtensionMenuClicked(const std::wstring& extId, const std::string& itemId, const ExtensionMenuContext& c, unsigned long long tabId);
void ExtensionsFrameSeen(WKPageRef page, WKFrameInfoRef frame);   // a subframe's response (for executeScript allFrames)
void ExtensionsSetupPage(WKPageRef page);        // a tab's new page, before it loads anything (webRequest)
