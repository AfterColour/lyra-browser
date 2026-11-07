// Lyra — инструмент разработчика: замеры на странице (LYRA_PROBE=<файл .js>), результат в tabs.log.
#include "browser/DevProbe.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "core/StringUtils.h"

#include <WebKit/WKMutableDictionary.h>
#include <fstream>
#include <iterator>

static void OnProbeDone(WKTypeRef result, WKErrorRef error, void* ctx) {
    std::string s;
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) s = ToUtf8(WKStr((WKStringRef)result));
    else if (error) s = "(error)";
    else s = "(no result)";
    Log("probe tab#%llu: %s", (unsigned long long)(uintptr_t)ctx, s.c_str());
}

void DevProbePageLoaded(Tab& t) {
    static int state = 0;                    // 0 unknown, 1 on, 2 off
    static std::string source;
    if (state == 0) {
        wchar_t path[MAX_PATH] = {};
        if (GetEnvironmentVariableW(L"LYRA_PROBE", path, MAX_PATH)) {
            std::ifstream f(path, std::ios::binary);
            source.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        }
        state = source.empty() ? 2 : 1;
        if (state == 1) Log("probe: running %ls on every page", path);
    }
    if (state != 1 || !t.page) return;
    if (t.url.compare(0, 4, L"http") != 0) return;
    WKStringRef js = WKStringCreateWithUTF8CString(source.c_str());
    WKMutableDictionaryRef args = WKMutableDictionaryCreate();
    WKPageCallAsyncJavaScript(t.page, js, (WKDictionaryRef)args, nullptr, (void*)(uintptr_t)t.id, OnProbeDone);
    WKRelease(args);
    WKRelease(js);
}
