// Lyra - installing extensions from the stores and from packages.
//   - Chrome Web Store and Firefox Add-ons: their "Add to Chrome" / "Add to Firefox" buttons only
//     work in those browsers, so on an extension's page Lyra shows its own "Add to Lyra" button
//     (a user script in a world of its own). Lyra downloads the .crx from Google's update server or
//     the .xpi named by the addons.mozilla.org API;
//   - a link to a .crx / .xpi (or a response of that type), and such a file opened in Lyra.
// Every install asks first, with the extension's name and what it will be able to do.
#pragma once

#include "common/Platform.h"
#include <WebKit/WKBase.h>

void ExtensionStoreAddScript(WKUserContentControllerRef controller);   // with the extensions' scripts
bool ExtensionIsPackage(const std::wstring& url, const std::wstring& mime = L"");  // .crx / .xpi
void ExtensionInstallFromUrl(const std::wstring& url);   // https: downloads, then asks (WM_APP_EXT_INSTALL)
void ExtensionInstallFromPath(const std::wstring& path); // a local file: asks, installs
void ExtensionStoreMessage(WPARAM wParam, LPARAM lParam); // main window: WM_APP_EXT_INSTALL
// The password manager: 0 Lyra's own saving, 1 Bitwarden, 2 Proton Pass, 3 none. An extension not
// installed yet is added from Chrome Web Store (asking first) and becomes the one once it is in;
// the other one is turned off (not removed). Choice: the one in use now.
void ExtensionSetPasswordManager(int which);
int ExtensionPasswordManagerChoice();
