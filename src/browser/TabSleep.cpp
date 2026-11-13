// Lyra — усыпление вкладок: давно не открывавшиеся вкладки выгружаются из памяти.
#include "browser/TabSleep.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "browser/Session.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/SplitView.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/FullScreen.h"
#include "ui/MainWindow.h"

namespace {

// Text typed by the user into the page (trusted input events) since it loaded; a form sent
// clears it. Asked for before a tab is put to sleep.
const char kScript[] = R"JS((function () {
  if (window.__lyraTyped !== undefined) return;
  window.__lyraTyped = false;
  addEventListener('input', function (e) { if (e.isTrusted) window.__lyraTyped = true; }, true);
  addEventListener('submit', function () { window.__lyraTyped = false; }, true);
})();)JS";

constexpr int kAfterMinutes[] = { 5, 15, 30, 60, 120 };       // g_prefs.sleepAfter
constexpr ULONGLONG kLowMemoryAfterMs = 2 * 60 * 1000;        // when Windows is short of memory
constexpr DWORD kLowMemoryLoad = 85;                          // MEMORYSTATUSEX::dwMemoryLoad, %

std::vector<unsigned long long> s_asking;                    // tabs whose page is being asked
// Tabs with typed text: asked again 5 minutes later, or once they were shown again.
struct Typed { ULONGLONG hiddenSince, retryAt; };
std::map<unsigned long long, Typed> s_typed;
constexpr ULONGLONG kTypedRetryMs = 5 * 60 * 1000;

bool LowMemory() {
    MEMORYSTATUSEX m = { sizeof(m) };
    return GlobalMemoryStatusEx(&m) && m.dwMemoryLoad >= kLowMemoryLoad;
}

void Unload(Tab& t, const char* why) {
    const std::string state = SessionPageState(t.page);
    const std::wstring url = t.url;
    if (UnloadTab(t, state)) {
        Log("tab#%llu put to sleep (%s): %s", t.id, why, ToUtf8(HostOf(url)).c_str());
        InvalidateChrome();
    }
}

struct AskCtx { unsigned long long tabId; bool manual; };

void OnTypedAnswer(WKTypeRef result, WKErrorRef, void* ctx) {
    std::unique_ptr<AskCtx> c((AskCtx*)ctx);
    s_asking.erase(std::remove(s_asking.begin(), s_asking.end(), c->tabId), s_asking.end());
    Tab* t = FindTab(c->tabId);
    if (!t || !TabSleepBlocker(*t).empty()) return;
    const bool typed = result && WKGetTypeID(result) == WKBooleanGetTypeID() && WKBooleanGetValue((WKBooleanRef)result);
    if (typed) {
        if (!c->manual) {
            Log("tab#%llu stays awake: text was typed into it", t->id);
            s_typed[t->id] = { t->hiddenSince, GetTickCount64() + kTypedRetryMs };
            return;
        }
        std::wstring q = Tr(L"Text typed into this tab will be lost. Put it to sleep anyway?",
                            L"Введённый на этой вкладке текст пропадёт. Всё равно усыпить её?");
        if (MessageBoxW(g_hWnd, q.c_str(), L"Lyra", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        t = FindTab(c->tabId);
        if (!t || !TabSleepBlocker(*t).empty()) return;
    }
    Unload(*t, c->manual ? "from the menu" : "not used for a while");
}

void Ask(Tab& t, bool manual) {
    if (std::find(s_asking.begin(), s_asking.end(), t.id) != s_asking.end()) return;
    s_asking.push_back(t.id);
    WKStringRef js = WKStringCreateWithUTF8CString("window.__lyraTyped === true");
    WKPageEvaluateJavaScriptInMainFrame(t.page, js, new AskCtx { t.id, manual }, OnTypedAnswer);
    WKRelease(js);
}

} // namespace

void TabSleepInit() {
    AdBlockAddUserScript(kScript, true);
}

std::wstring TabSleepBlocker(const Tab& t) {
    if (t.state != TabState::Live || !t.page) return L"-";
    if (t.sleeping || t.lazyPending) return Tr(L"It is not loaded", L"Она не загружена");
    if (&t == ActiveTab()) return Tr(L"It is the open tab", L"Это открытая вкладка");
    if (SplitVisible(t)) return Tr(L"It is open in split view", L"Она открыта в разделённом экране");
    if (t.privateTab) return Tr(L"Private tabs do not sleep", L"Приватные вкладки не усыпляются");
    if (t.url.empty() || t.url == L"about:blank" || IsInternalPageUrl(t.url)) return Tr(L"A Lyra page", L"Страница Lyra");
    if (t.audible) return Tr(L"It plays sound", L"На ней играет звук");
    if (t.pipHwnd) return Tr(L"It plays in picture-in-picture", L"Она в режиме «картинка в картинке»");
    if (t.hwnd && t.hwnd == g_fullScreenView) return Tr(L"It is in full screen", L"Она во весь экран");
    if (t.loading) return Tr(L"It is loading", L"Она загружается");
    return L"";
}

void TabSleepTick() {
    if (!g_prefs.sleepTabs || g_closing || g_tabOp) return;
    const bool low = LowMemory();
    const int idx = std::min(std::max(g_prefs.sleepAfter, 0), (int)std::size(kAfterMinutes) - 1);
    ULONGLONG after = (ULONGLONG)kAfterMinutes[idx] * 60 * 1000;
    if (low) after = std::min(after, kLowMemoryAfterMs);
    // For tests: LYRA_SLEEP_AFTER_MS sets the time directly.
    static const ULONGLONG testAfter = [] { wchar_t v[32] = {}; return GetEnvironmentVariableW(L"LYRA_SLEEP_AFTER_MS", v, 32) ? (ULONGLONG)_wtoi64(v) : 0ULL; }();
    if (testAfter) after = testAfter;
    const ULONGLONG now = GetTickCount64();
    // One tab per tick, the one hidden longest.
    Tab* pick = nullptr;
    for (auto& tp : g_tabs) {
        Tab& t = *tp;
        if (!t.hiddenSince || now - t.hiddenSince < after || !TabSleepBlocker(t).empty()) continue;
        if (std::find(s_asking.begin(), s_asking.end(), t.id) != s_asking.end()) continue;
        if (auto ty = s_typed.find(t.id); ty != s_typed.end()) {
            if (ty->second.hiddenSince == t.hiddenSince && now < ty->second.retryAt) continue;
            s_typed.erase(ty);
        }
        if (!pick || t.hiddenSince < pick->hiddenSince) pick = &t;
    }
    if (!pick) return;
    if (low) Log("tab sleep: Windows is short of memory, tabs sleep after %llu s", after / 1000);
    Ask(*pick, false);
}

void SleepTabNow(Tab& t) {
    if (!TabSleepBlocker(t).empty()) { MessageBeep(MB_ICONWARNING); return; }
    Ask(t, true);
}

void SleepOtherTabs() {
    for (auto& tp : g_tabs)
        if (TabSleepBlocker(*tp).empty()) Ask(*tp, false);
}
