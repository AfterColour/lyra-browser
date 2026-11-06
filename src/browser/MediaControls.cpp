// Lyra — медиа-клавиши и плашка Windows для звука/видео со страниц (YouTube, музыка).
#include "browser/MediaControls.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "browser/SystemMedia.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"

#include <WebKit/WKPagePrivate.h>
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>

namespace {

// Injected into every page at document start (main frame). It remembers the page's Media Session
// action handlers (they cannot be read back otherwise) and answers Lyra's two calls:
// state() -> fields joined with \x01, run(action) -> the page's handler or the media element.
const char kScript[] = R"JS((function () {
  if (window.__lyraMedia) return;
  var handlers = {};
  var ms = navigator.mediaSession;
  if (ms && ms.setActionHandler) {
    var original = ms.setActionHandler.bind(ms);
    ms.setActionHandler = function (action, handler) { handlers[action] = handler; return original(action, handler); };
  }
  function element() {
    var list = Array.prototype.slice.call(document.querySelectorAll('video, audio'));
    for (var i = 0; i < list.length; i++) if (!list[i].paused && !list[i].ended) return list[i];
    for (var i = 0; i < list.length; i++) if (list[i].currentSrc && list[i].duration > 0) return list[i];
    return null;
  }
  Object.defineProperty(window, '__lyraMedia', { value: {
    state: function () {
      var m = element(), md = ms && ms.metadata, art = '';
      if (md && md.artwork && md.artwork.length) {
        var best = md.artwork[0], bestSize = 0;
        for (var i = 0; i < md.artwork.length; i++) {
          var s = parseInt(String(md.artwork[i].sizes || '0').split(/[x ]/)[0], 10) || 0;
          if (s >= bestSize) { best = md.artwork[i]; bestSize = s; }
        }
        try { art = new URL(best.src, location.href).href; } catch (e) {}
      }
      var playing = m ? !m.paused && !m.ended : !!(ms && ms.playbackState === 'playing');
      return [m || md ? 1 : 0, playing ? 1 : 0, (md && md.title) || document.title || '',
              (md && md.artist) || location.hostname, (md && md.album) || '', art,
              handlers.nexttrack ? 1 : 0, handlers.previoustrack ? 1 : 0].join('\x01');
    },
    run: function (action) {
      var h = handlers[action];
      if (h) { try { h({ action: action }); return; } catch (e) {} }
      var m = element();
      if (!m) return;
      if (action === 'play') m.play(); else m.pause();
    }
  }});
})();)JS";

unsigned long long s_tabId = 0;       // the media tab
std::wstring s_lastState;             // what the flyout shows now
DWORD s_lastPoll = 0;
bool s_polling = false, s_shown = false, s_ok = false;

Tab* MediaTab() {
    if (!s_tabId) return nullptr;
    for (auto& t : g_tabs)
        if (t->id == s_tabId && t->state == TabState::Live && t->page && !t->lazyPending) return t.get();
    return nullptr;
}

bool PlaysAudio(const Tab& t) {
    return t.state == TabState::Live && t.page && !t.lazyPending &&
           (WKPageGetMediaState(t.page) & kWKMediaIsPlayingAudio);
}

void Hide() {
    if (s_shown) SystemMediaHide();
    s_shown = false;
    s_lastState.clear();
}

std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> out(1);
    for (wchar_t c : s) { if (c == 1) out.emplace_back(); else out.back() += c; }
    return out;
}

void OnState(WKTypeRef result, WKErrorRef, void* ctx) {
    s_polling = false;
    const unsigned long long tabId = (unsigned long long)(uintptr_t)ctx;
    std::wstring state;
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) state = WKStr((WKStringRef)result);
    const auto f = Split(state);
    const bool has = f.size() >= 8 && f[0] == L"1";
    if (tabId != s_tabId) {
        // A page the engine did not report as playing: taken when it really plays.
        if (!has || f[1] != L"1") return;
        Log("media: tab#%llu plays (found by asking the page)", tabId);
        s_tabId = tabId;
        s_lastState.clear();
    }
    if (!has) {                                       // nothing to control on this page any more
        Log("media: tab#%llu has no media any more", tabId);
        Hide();
        s_tabId = 0;
        return;
    }
    if (state == s_lastState) return;
    s_lastState = state;
    SystemMediaInfo info;
    info.playing = f[1] == L"1";
    info.title = f[2];
    info.artist = f[3];
    info.album = f[4];
    info.artworkUrl = f[5];
    info.canNext = f[6] == L"1";
    info.canPrevious = f[7] == L"1";
    Log("media: tab#%llu %s \"%s\" / %s", tabId, info.playing ? "playing" : "paused",
        ToUtf8(info.title.substr(0, 80)).c_str(), ToUtf8(info.artist.substr(0, 40)).c_str());
    SystemMediaShow(info);
    s_shown = true;
}

void Poll(Tab& t) {
    if (s_polling) return;
    s_polling = true;
    s_lastPoll = GetTickCount();
    WKStringRef js = WKStringCreateWithUTF8CString("window.__lyraMedia ? window.__lyraMedia.state() : ''");
    WKPageEvaluateJavaScriptInMainFrame(t.page, js, (void*)(uintptr_t)t.id, OnState);
    WKRelease(js);
}

} // namespace

void MediaControlsInit() {
    s_ok = SystemMediaInit(g_hWnd);
    if (!s_ok) return;
    WKUserContentControllerRef controller = AdBlockUserContentController();
    if (!controller) { Log("media: no user content controller"); s_ok = false; return; }
    Log("media: Windows media controls ready");
    WKStringRef source = WKStringCreateWithUTF8CString(kScript);
    WKUserScriptRef script = WKUserScriptCreateWithSource(source, kWKInjectAtDocumentStart, true);
    WKUserContentControllerAddUserScript(controller, script);
    WKRelease(script);
    WKRelease(source);
}

void MediaControlsShutdown() {
    if (!s_ok) return;
    SystemMediaShutdown();
    s_ok = false;
}

void MediaControlsTick() {
    if (!s_ok) return;
    const DWORD now = GetTickCount();
    if (now - s_lastPoll < 1000) return;
    s_lastPoll = now;

    // The tab playing sound wins (the active one first); otherwise the last one keeps the flyout
    // (paused) while it still has something to play.
    Tab* playing = nullptr;
    if (Tab* a = ActiveTab()) if (PlaysAudio(*a)) playing = a;
    if (!playing) if (Tab* m = MediaTab()) if (PlaysAudio(*m)) playing = m;
    if (!playing) for (auto& t : g_tabs) if (PlaysAudio(*t)) { playing = t.get(); break; }
    if (playing && playing->id != s_tabId) {
        s_tabId = playing->id;
        s_lastState.clear();
        s_polling = false;
    }
    Tab* t = MediaTab();
    if (!t) {
        if (s_tabId || s_shown) { Hide(); s_tabId = 0; }
        // The engine may not report the sound: ask the active page itself.
        if (Tab* a = ActiveTab())
            if (a->page && !a->lazyPending && !a->url.empty() && !IsInternalPageUrl(a->url)) Poll(*a);
        return;
    }
    Poll(*t);
}

void MediaControlsAudioChanged() {
    s_lastPoll = GetTickCount() - 1000;                 // look at once
}

void MediaToggleMute(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    Tab& t = *g_tabs[index];
    if (!t.page) return;
    t.muted = !t.muted;
    WKPageSetMuted(t.page, t.muted ? kWKMediaAudioMuted : kWKMediaNoneMuted);
    InvalidateChrome();
}

void MediaControlsButton(WPARAM button) {
    Tab* t = MediaTab();
    if (!t) return;
    const char* action = nullptr;
    switch ((int)button) {
    case kMediaPlay: action = "play"; break;
    case kMediaPause: action = "pause"; break;
    case kMediaStop: action = "stop"; break;
    case kMediaNext: action = "nexttrack"; break;
    case kMediaPrevious: action = "previoustrack"; break;
    default: return;
    }
    std::string js = std::string("window.__lyraMedia && window.__lyraMedia.run('") + action + "')";
    WKStringRef s = WKStringCreateWithUTF8CString(js.c_str());
    WKPageEvaluateJavaScriptInMainFrame(t->page, s, nullptr, nullptr);
    WKRelease(s);
    s_lastPoll = GetTickCount() - 700;                  // show the new state soon
}
