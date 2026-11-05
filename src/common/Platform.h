// Lyra — системные заголовки, GDI+ и WebKit C API — подключаются всеми файлами.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// COM declarations must be visible BEFORE gdiplus.h. windows.h skips them when
// WIN32_LEAN_AND_MEAN is defined (project-wide or by a precompiled header), which
// causes "IUnknown undefined" / "STDMETHOD not found" inside gdiplus.h.
#include <objbase.h>   // STDMETHOD, IUnknown, COM base types
#include <unknwn.h>    // IUnknown
#include <objidl.h>    // IStream (used by GDI+ image/stream APIs)
#include <windowsx.h>
#include <shellapi.h>  // ShellExecuteW (also skipped by WIN32_LEAN_AND_MEAN)
#include <commdlg.h>   // GetOpenFileNameW (file chooser for <input type=file>)
#include <shlobj.h>    // SHGetKnownFolderPath (Downloads folder)
#include <shlwapi.h>   // UrlCreateFromPathW
#include <dwmapi.h>
#include <algorithm>
#include <map>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <memory>
#include <string>
#include <vector>
using std::min;   // gdiplus.h uses unqualified min/max
using std::max;
#include <gdiplus.h>

#include <WebKit/WebKit2_C.h>
#include <WebKit/WKContextConfigurationRef.h>
#include <WebKit/WKDownloadClient.h>
#include <WebKit/WKDownloadRef.h>
#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKInspector.h>
#include <WebKit/WKNavigationActionRef.h>
#include <WebKit/WKURLRequest.h>
#include <WebKit/WKNavigationResponseRef.h>
#include <WebKit/WKPreferencesRefPrivate.h>
#include <WebKit/WKSecurityOriginRef.h>
#include <WebKit/WKWebsiteDataStoreConfigurationRef.h>
#include <WebKit/WKWebsiteDataStoreRef.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uuid.lib")

using namespace Gdiplus;
