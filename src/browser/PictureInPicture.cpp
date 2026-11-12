// Lyra — «картинка в картинке»: видео в маленьком окне поверх всех окон.
#include "browser/PictureInPicture.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/StringUtils.h"
#include "ui/Draw.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKMutableDictionary.h>
#include <WebKit/WKPagePrivate.h>                      // WKPageSetVideoPictureInPictureWindow
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKUserContentControllerRef.h>
#include <WebKit/WKUserScriptRef.h>
#include <WebKit/WKView.h>

namespace {

// Main frame, document start. __lyraPip.enter() marks the video (data-lyra-pip; returns "W x H"):
// the engine draws it into Lyra's window too (WKPageSetVideoPictureInPictureWindow), and the page
// stays as it is and keeps working. leave() unmarks it. A small button over videos and the standard
// requestPictureInPicture() ask Lyra through the lyraPip message handler.
const char kScript[] = R"JS((function () {
  if (window.__lyraPip) return;
  var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lyraPip;
  if (!h) return;
  var current = null, pending = null;
  function videos() { return Array.prototype.slice.call(document.querySelectorAll('video')); }
  function area(v) { var r = v.getBoundingClientRect(); return r.width * r.height; }
  function best() {
    var list = videos(), pick = null;
    list.forEach(function (v) { if (!v.paused && !v.ended && (!pick || area(v) > area(pick))) pick = v; });
    if (!pick) list.forEach(function (v) { if (v.currentSrc && (!pick || area(v) > area(pick))) pick = v; });
    return pick;
  }
  // Built with DOM calls, not innerHTML: pages with Trusted Types (YouTube, sometimes) refuse
  // innerHTML, and enter() failed halfway (the video marked, no window).
  var SVG = 'http://www.w3.org/2000/svg';
  function el(tag, attrs, kids) {
    var svg = tag === 'svg' || tag === 'rect';
    var e = svg ? document.createElementNS(SVG, tag) : document.createElement(tag);
    for (var k in attrs || {}) { if (k === 'text') e.textContent = attrs[k]; else e.setAttribute(k, attrs[k]); }
    (kids || []).forEach(function (c) { e.appendChild(c); });
    return e;
  }
  // A placeholder over the video in the page while it plays in the window (the engine stops drawing
  // it there), with a button back. Inserted right after the video, so the site's own controls,
  // which sit above the video, stay above it too.
  var holder = null, holderTexts = ['', ''], holderObserver = null;
  function makeHolder() {
    var host = document.createElement('div');
    host.setAttribute('data-lyra-pip-holder', '');
    var root = host.attachShadow({ mode: 'closed' });
    var css = ':host{all:initial}' +
      '.p{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:12px;' +
      'background:radial-gradient(ellipse at center,#25252b 0%,#0c0c0f 75%);color:#e9e9ee;font:500 14px "Segoe UI",system-ui,sans-serif;' +
      'text-align:center;user-select:none;cursor:default;padding:12px;box-sizing:border-box;overflow:hidden}' +
      'svg{opacity:.8;flex:none}.small span{display:none}.small svg{width:40px;height:32px}' +
      'b{all:unset;padding:7px 14px;border-radius:16px;background:rgba(255,255,255,.12);font:600 12px "Segoe UI",system-ui,sans-serif;cursor:pointer}' +
      'b:hover{background:rgba(255,255,255,.22)}';
    var back = el('b', { text: holderTexts[1] });
    var panel = el('div', { 'class': 'p' }, [
      el('svg', { width: '56', height: '44', viewBox: '0 0 56 44', fill: 'none', stroke: 'currentColor', 'stroke-width': '2.5' }, [
        el('rect', { x: '2', y: '2', width: '52', height: '40', rx: '5' }),
        el('rect', { x: '27', y: '21', width: '21', height: '15', rx: '3', fill: 'currentColor', stroke: 'none' })]),
      el('span', { text: holderTexts[0] }), back]);
    root.appendChild(el('style', { text: css }));
    root.appendChild(panel);
    back.addEventListener('click', function (e) {
      e.preventDefault(); e.stopPropagation();
      h.postMessage(['exit']).catch(function () {});
    }, true);
    // The page's player must not take clicks on it for its own (play / pause, full screen).
    ['click', 'dblclick', 'mousedown', 'mouseup', 'pointerdown', 'pointerup'].forEach(function (type) {
      host.addEventListener(type, function (e) { e.stopPropagation(); }, false);
    });
    host.__lyraPanel = panel;
    return host;
  }
  function placeHolder() {
    if (!current || !current.parentNode) return;
    if (!holder) holder = makeHolder();
    if (holder.previousSibling !== current) current.parentNode.insertBefore(holder, current.nextSibling);
    var st = 'position:absolute;display:block;margin:0;pointer-events:auto;left:' + current.offsetLeft + 'px;top:' + current.offsetTop +
      'px;width:' + current.offsetWidth + 'px;height:' + current.offsetHeight + 'px';
    if (holder.style.cssText !== st) holder.style.cssText = st;
    holder.__lyraPanel.className = 'p' + (current.offsetHeight < 150 || current.offsetWidth < 260 ? ' small' : '');
    if (!holderObserver && window.ResizeObserver) holderObserver = new ResizeObserver(placeHolder);
    if (holderObserver) { holderObserver.disconnect(); holderObserver.observe(current); }
  }
  function removeHolder() {
    if (holderObserver) holderObserver.disconnect();
    if (holder && holder.parentNode) holder.parentNode.removeChild(holder);
  }
  function enter(text, button) {
    var v = pending || best();
    pending = null;
    if (!v) return '';
    leave();
    current = v;
    v.setAttribute('data-lyra-pip', '');
    if (text) holderTexts = [text, button || ''];
    holder = null;
    try { placeHolder(); } catch (e) {}           // the window comes even without the placeholder
    hideButton();
    keepTimer = setInterval(keep, 250);
    try { v.dispatchEvent(new Event('enterpictureinpicture')); } catch (e) {}
    return (v.videoWidth || 16) + 'x' + (v.videoHeight || 9);
  }
  // The next video, or a site swapping its <video> element (YouTube's ads): follow the one playing.
  var keepTimer = 0;
  function keep() {
    if (!current) return;
    var b = best();
    if (b && b !== current && (!current.isConnected || (current.paused && !b.paused))) {
      current.removeAttribute('data-lyra-pip');
      current = b;
    }
    if (!current.hasAttribute('data-lyra-pip')) current.setAttribute('data-lyra-pip', '');
    placeHolder();
  }
  function leave() {
    if (!current) return;
    clearInterval(keepTimer);
    var v = current;
    current = null;
    v.removeAttribute('data-lyra-pip');
    removeHolder();
    try { v.dispatchEvent(new Event('leavepictureinpicture')); } catch (e) {}
  }
  // "Back to tab": leaves, brings the video into view if it was scrolled away, and tells where it is:
  // "left,top,width,height,viewport width,viewport height" in CSS pixels.
  function back() {
    var v = current;
    leave();
    if (!v || !v.isConnected) return Promise.resolve('');
    function where() {
      var r = v.getBoundingClientRect();
      return [r.left, r.top, r.width, r.height, innerWidth, innerHeight].map(function (n) { return Math.round(n * 10) / 10; }).join(',');
    }
    var r = v.getBoundingClientRect();
    if (r.bottom > 0 && r.top < innerHeight && r.right > 0 && r.left < innerWidth) return Promise.resolve(where());
    v.scrollIntoView({ block: 'center' });
    return new Promise(function (resolve) {
      requestAnimationFrame(function () { requestAnimationFrame(function () { resolve(where()); }); });
      setTimeout(function () { resolve(where()); }, 300);
    });
  }
  Object.defineProperty(window, '__lyraPip', { value: {
    enter: enter, leave: leave, back: back,
    pause: function () { if (current) current.pause(); },
    title: function () { return document.title || location.hostname; },
    // "paused|time|duration|muted|video size|volume" for the window's controls
    state: function () {
      var v = current;
      if (!v) return '';
      return [v.paused ? 1 : 0, v.currentTime || 0, isFinite(v.duration) ? v.duration : 0, v.muted ? 1 : 0,
              (v.videoWidth || 0) + 'x' + (v.videoHeight || 0), v.volume].join('|');
    },
    cmd: function (c, arg) {
      var v = current;
      if (!v) return;
      if (c === 'toggle') { if (v.paused) v.play(); else v.pause(); }
      else if (c === 'seek') v.currentTime = Math.max(0, Math.min((v.duration || 0) - 0.1, v.currentTime + arg));
      else if (c === 'seekTo') v.currentTime = arg;
      else if (c === 'mute') v.muted = !v.muted;
      else if (c === 'volume') { v.volume = Math.max(0, Math.min(1, arg)); if (arg > 0) v.muted = false; }
    }
  }});

  // Sites' own picture-in-picture buttons (the standard API).
  if (!('pictureInPictureEnabled' in document) && window.HTMLVideoElement) {
    Object.defineProperty(document, 'pictureInPictureEnabled', { get: function () { return true; }, configurable: true });
    Object.defineProperty(document, 'pictureInPictureElement', { get: function () { return current; }, configurable: true });
    document.exitPictureInPicture = function () { return h.postMessage(['exit']); };
    HTMLVideoElement.prototype.requestPictureInPicture = function () {
      pending = this;
      var v = this;
      return h.postMessage(['request']).then(function () { return { width: v.videoWidth, height: v.videoHeight }; });
    };
  }

  // The button over a video (as in Firefox), while the pointer is on it.
  var button = null, over = null, hideTimer = 0;
  function makeButton() {
    var host = document.createElement('div');
    host.style.cssText = 'position:fixed;z-index:2147483646;top:0;left:0;display:none';
    var root = host.attachShadow({ mode: 'closed' });
    var css = 'b{all:initial;display:flex;align-items:center;gap:6px;height:28px;padding:0 10px;border-radius:14px;' +
      'background:rgba(20,20,24,.82);color:#fff;font:600 12px "Segoe UI",system-ui,sans-serif;cursor:pointer;' +
      'box-shadow:0 2px 8px rgba(0,0,0,.35)}b:hover{background:rgba(70,70,80,.92)}svg{display:block}';
    var b = el('b', { title: 'Picture in picture' }, [
      el('svg', { width: '16', height: '14', viewBox: '0 0 16 14', fill: 'none', stroke: '#fff', 'stroke-width': '1.4' }, [
        el('rect', { x: '1', y: '1', width: '14', height: '12', rx: '2' }),
        el('rect', { x: '8', y: '7', width: '5', height: '4', rx: '1', fill: '#fff' })])]);
    root.appendChild(el('style', { text: css }));
    root.appendChild(b);
    b.addEventListener('click', function (e) {
      e.preventDefault(); e.stopPropagation();
      if (!over) return;
      pending = over;
      hideButton();
      h.postMessage(['request']).catch(function () {});
    }, true);
    (document.body || document.documentElement).appendChild(host);
    return host;
  }
  function hideButton() { if (button) button.style.display = 'none'; over = null; }
  var raf = 0, lastX = 0, lastY = 0;
  document.addEventListener('mousemove', function (e) {
    lastX = e.clientX; lastY = e.clientY;
    if (raf || current) return;                     // (in picture-in-picture: no button)
    raf = requestAnimationFrame(function () {
      raf = 0;
      var found = null;
      videos().forEach(function (v) {
        var r = v.getBoundingClientRect();
        if (r.width >= 240 && r.height >= 135 && lastX >= r.left && lastX <= r.right && lastY >= r.top && lastY <= r.bottom &&
            (v.currentSrc || v.srcObject) && !v.disablePictureInPicture) found = v;
      });
      if (!found) { if (over) hideButton(); return; }
      if (!button || !button.isConnected) button = makeButton();
      var r = found.getBoundingClientRect();
      over = found;
      button.style.left = Math.round(r.right - 52) + 'px';
      button.style.top = Math.round(r.top + r.height / 2 - 14) + 'px';
      button.style.display = 'block';
      clearTimeout(hideTimer);
      hideTimer = setTimeout(hideButton, 2500);
    });
  }, true);
})();)JS";

HWND s_window = nullptr;                  // the picture-in-picture window (the engine draws the video into it)
HWND s_overlay = nullptr;                 // the controls over it (layered, shown while the pointer is on it)
unsigned long long s_tabId = 0;
// A start waits for the page's answer (enter()). A fast double click asks twice before the window
// exists: the second ask must not make a second window (the first one stayed, black), and an exit
// before the answer cancels the start. Each start has its own number; a stale answer changes nothing.
unsigned s_startSeq = 0;                  // the start in flight (0: none)
unsigned long long s_startTab = 0;
struct StartContext { unsigned long long tabId; unsigned seq; };
float s_aspect = 16.f / 9.f;              // video width / height
std::wstring s_title;
std::unique_ptr<Font> s_fGlyph, s_fBig, s_fText;

// What the page reported last (VideoState) and the controls' state.
struct VideoState { bool paused = true, muted = false; double time = 0, duration = 0, volume = 1; };
VideoState s_state;
int s_hover = 0, s_down = 0;
bool s_overlayShown = false, s_polling = false;
DWORD s_lastInside = 0, s_lastPoll = 0, s_lastEnsure = 0, s_volumeHoverAt = 0;
bool s_volumeDrawn = false;
bool s_pressed = false;                   // a press on the video: a click plays / pauses, a drag moves the window
POINT s_pressAt = {};
constexpr UINT_PTR kTimer = 1;

enum Button { kNone, kBack10, kPlay, kFwd10, kMute, kToTab, kClose, kProgress, kVolume, kDrag };

// The volume slider pops up above the mute button while the pointer is on either (and a moment after).
bool VolumeShown() {
    return s_hover == kMute || s_hover == kVolume || s_down == kVolume || GetTickCount() - s_volumeHoverAt < 450;
}

Tab* PipTab() {
    for (auto& t : g_tabs) if (t->id == s_tabId && t->state == TabState::Live) return t.get();
    return nullptr;
}

RECT WindowRc() { RECT rc; GetWindowRect(s_window, &rc); return rc; }

// Overlay layout, in its own pixels (it covers the window exactly).
struct OverlayLayout { RectF back10, play, fwd10, mute, toTab, close, progress, volume; float w, h; };
OverlayLayout Layout() {
    const RECT rc = WindowRc();
    OverlayLayout L;
    L.w = (float)(rc.right - rc.left);
    L.h = (float)(rc.bottom - rc.top);
    const float big = S(52), sm = S(40), top = S(36);
    const float cy = L.h / 2;
    L.play = RectF(L.w / 2 - big / 2, cy - big / 2, big, big);
    L.back10 = RectF(L.play.X - S(18) - sm, cy - sm / 2, sm, sm);
    L.fwd10 = RectF(L.play.GetRight() + S(18), cy - sm / 2, sm, sm);
    L.close = RectF(L.w - S(8) - top, S(8), top, top);
    L.toTab = RectF(L.close.X - S(4) - top, S(8), top, top);
    L.mute = RectF(L.w - S(8) - top, L.h - S(8) - top, top, top);
    L.progress = RectF(S(14), L.h - S(8) - top / 2 - S(10), L.mute.X - S(28), S(20));   // hit area; the bar is thin
    const float vw = S(30), vh = std::min(S(118), L.mute.Y - S(6) - S(8));
    L.volume = vh >= S(48) ? RectF(L.mute.X + (top - vw) / 2, L.mute.Y - S(6) - vh, vw, vh) : RectF();
    return L;
}

// Volume (0..1) at a height on the slider's track.
double VolumeAt(const OverlayLayout& L, float y) {
    const float top = L.volume.Y + S(13), bottom = L.volume.GetBottom() - S(13);
    if (bottom <= top) return s_state.volume;
    return std::min(1.0, std::max(0.0, (double)(bottom - y) / (bottom - top)));
}

int HitAt(float x, float y) {
    const OverlayLayout L = Layout();
    if (L.play.Contains(x, y)) return kPlay;
    if (L.back10.Contains(x, y)) return kBack10;
    if (L.fwd10.Contains(x, y)) return kFwd10;
    if (L.close.Contains(x, y)) return kClose;
    if (L.toTab.Contains(x, y)) return kToTab;
    if (L.mute.Contains(x, y)) return kMute;
    if (L.volume.Height > 0 && VolumeShown() && L.volume.Contains(x, y)) return kVolume;
    if (L.progress.Contains(x, y) && s_state.duration > 0) return kProgress;
    return kDrag;
}

std::wstring Clock(double s) {
    if (!(s >= 0)) s = 0;
    const int t = (int)s, h = t / 3600, m = t / 60 % 60, sec = t % 60;
    wchar_t buf[32];
    if (h) swprintf(buf, 32, L"%d:%02d:%02d", h, m, sec); else swprintf(buf, 32, L"%d:%02d", m, sec);
    return buf;
}

// Draws the controls into a premultiplied 32-bit bitmap and hands it to the layered window.
// Fully transparent pixels let clicks through to the video (a click there plays / pauses).
void RenderOverlay() {
    if (!s_overlay) return;
    const OverlayLayout L = Layout();
    const int w = (int)L.w, h = (int)L.h;
    if (w <= 0 || h <= 0) return;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) { ReleaseDC(nullptr, screen); return; }
    {
        Bitmap canvas(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)bits);
        Graphics g(&canvas);
        g.SetSmoothingMode(SmoothingModeHighQuality);
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.Clear(Color(0, 0, 0, 0));
        // Shades at the top and bottom keep the buttons readable on bright video; a barely visible
        // frame lets the edges be grabbed for resizing.
        const float shade = std::min(S(64), L.h / 3);
        LinearGradientBrush topShade(PointF(0, -1), PointF(0, shade), Color(150, 0, 0, 0), Color(0, 0, 0, 0));
        g.FillRectangle(&topShade, 0.f, 0.f, L.w, shade);
        LinearGradientBrush bottomShade(PointF(0, L.h - shade - 1), PointF(0, L.h), Color(0, 0, 0, 0), Color(170, 0, 0, 0));
        g.FillRectangle(&bottomShade, 0.f, L.h - shade, L.w, shade);
        SolidBrush edge(Color(2, 0, 0, 0));
        const float e = S(6);
        g.FillRectangle(&edge, 0.f, shade, e, L.h - 2 * shade);
        g.FillRectangle(&edge, L.w - e, shade, e, L.h - 2 * shade);

        const Color white(255, 255, 255, 255);
        auto circle = [&](const RectF& r, int id, BYTE alpha) {
            SolidBrush b(Color(s_hover == id ? (BYTE)std::min(255, alpha + 60) : alpha, 20, 20, 24));
            g.FillEllipse(&b, r);
        };
        circle(L.play, kPlay, 150);
        DrawGlyph(g, s_state.paused ? L"" : L"", L.play, white, s_fBig.get());
        for (int id : { (int)kBack10, (int)kFwd10 }) {
            const RectF& r = id == kBack10 ? L.back10 : L.fwd10;
            circle(r, id, 120);
            DrawGlyph(g, id == kBack10 ? L"" : L"", RectF(r.X, r.Y + S(2), r.Width, r.Height * 0.55f), white, s_fGlyph.get());
            StringFormat cf;
            cf.SetAlignment(StringAlignmentCenter);
            cf.SetLineAlignment(StringAlignmentCenter);
            SolidBrush tb(white);
            g.DrawString(L"10", -1, s_fText.get(), RectF(r.X, r.Y + r.Height * 0.45f, r.Width, r.Height * 0.45f), &cf, &tb);
        }
        circle(L.toTab, kToTab, 110);
        DrawGlyph(g, L"", L.toTab, white, s_fGlyph.get());
        if (s_hover == kClose) { SolidBrush red(Color(230, 196, 43, 28)); g.FillEllipse(&red, L.close); }
        else circle(L.close, kClose, 110);
        DrawGlyph(g, L"", L.close, white, s_fGlyph.get());
        circle(L.mute, kMute, 110);
        const double level = s_state.muted ? 0 : s_state.volume;
        DrawGlyph(g, (s_state.muted || level <= 0) ? L"\uE74F" : level < 0.5 ? L"\uE993" : L"\uE767", L.mute, white, s_fGlyph.get());
        s_volumeDrawn = L.volume.Height > 0 && VolumeShown();
        if (s_volumeDrawn) {
            const RectF& v = L.volume;
            FillRound(g, Color(185, 20, 20, 24), v, v.Width / 2);
            const float x = v.X + v.Width / 2, top = v.Y + S(13), bottom = v.GetBottom() - S(13), barW = S(4);
            const float y = bottom - (float)level * (bottom - top);
            FillRound(g, Color(90, 255, 255, 255), RectF(x - barW / 2, top, barW, bottom - top), barW / 2);
            FillRound(g, white, RectF(x - barW / 2, y, barW, std::max(barW, bottom - y)), barW / 2);
            SolidBrush knob(white);
            const float k = s_down == kVolume || s_hover == kVolume ? S(7) : S(6);
            g.FillEllipse(&knob, x - k, y - k, 2 * k, 2 * k);
        }

        if (s_state.duration > 0) {
            const RectF& p = L.progress;
            const float barH = s_hover == kProgress ? S(5) : S(3), y = p.Y + p.Height / 2 - barH / 2;
            const float frac = (float)std::min(1.0, std::max(0.0, s_state.time / s_state.duration));
            const RectF track(p.X, y + S(14), p.Width, barH);
            FillRound(g, Color(90, 255, 255, 255), track, barH / 2);
            FillRound(g, white, RectF(track.X, track.Y, std::max(barH, track.Width * frac), barH), barH / 2);
            SolidBrush tb(Color(235, 255, 255, 255));
            StringFormat sf;
            sf.SetLineAlignment(StringAlignmentCenter);
            const std::wstring t = Clock(s_state.time) + L" / " + Clock(s_state.duration);
            g.DrawString(t.c_str(), -1, s_fText.get(), RectF(p.X, p.Y - S(6), p.Width, S(18)), &sf, &tb);
        }
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    const RECT rc = WindowRc();
    POINT dst = { rc.left, rc.top }, src = { 0, 0 };
    SIZE size = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(s_overlay, screen, &dst, &size, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    ReleaseDC(nullptr, screen);
}

void RunJs(WKPageRef page, const std::string& js) {
    if (!page) return;
    WKStringRef s = WKStringCreateWithUTF8CString(js.c_str());
    WKPageEvaluateJavaScriptInMainFrame(page, s, nullptr, nullptr);
    WKRelease(s);
}

void Command(const char* cmd, double arg = 0) {
    Tab* t = PipTab();
    if (!t) return;
    char js[160];
    snprintf(js, sizeof(js), "window.__lyraPip && __lyraPip.cmd('%s', %.3f)", cmd, arg);
    RunJs(t->page, js);
    s_lastPoll = 0;                                      // show the change at once
}

void SetVolume(double v) {
    s_state.volume = v;
    if (v > 0) s_state.muted = false;
    Command("volume", v);
    RenderOverlay();
}

void OnState(WKTypeRef result, WKErrorRef, void*) {
    s_polling = false;
    std::wstring s;
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) s = WKStr((WKStringRef)result);
    if (s.empty()) return;
    VideoState v;
    int paused = 1, muted = 0;
    if (swscanf_s(s.c_str(), L"%d|%lf|%lf|%d", &paused, &v.time, &v.duration, &muted) >= 3) {
        v.paused = paused != 0;
        v.muted = muted != 0;
        if (size_t bar = s.rfind(L'|'); bar != std::wstring::npos) swscanf_s(s.c_str() + bar + 1, L"%lf", &v.volume);
        if (s_down == kVolume) v.volume = s_state.volume;    // dragging: what the pointer says
        s_state = v;
        if (s_overlayShown) RenderOverlay();
    }
}

void Poll() {
    Tab* t = PipTab();
    if (!t || s_polling) return;
    s_polling = true;
    s_lastPoll = GetTickCount();
    WKStringRef js = WKStringCreateWithUTF8CString("window.__lyraPip ? __lyraPip.state() : ''");
    WKPageEvaluateJavaScriptInMainFrame(t->page, js, nullptr, OnState);
    WKRelease(js);
}

void ShowOverlay(bool show) {
    if (!s_overlay || show == s_overlayShown) return;
    s_overlayShown = show;
    if (show) {
        RenderOverlay();
        ShowWindow(s_overlay, SW_SHOWNOACTIVATE);
        SetWindowPos(s_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(s_overlay, SW_HIDE);
        s_hover = 0;
    }
}

// Tells the engine (again) to draw the tab's marked video into the window. Repeated from time to time:
// the marked video or its player change with the next video. `redraw`: the window was resized.
void EnsureVideoInWindow(bool redraw) {
    Tab* t = PipTab();
    if (!t || !t->page || !s_window) return;
    s_lastEnsure = GetTickCount();
    WKPageSetVideoPictureInPictureWindow(t->page, (uint64_t)(uintptr_t)s_window, redraw);
}

void PlaceView() {
    if (s_overlayShown) RenderOverlay();
}

// The window keeps the video's shape (its whole client is the video).
void KeepAspect(WPARAM edge, RECT* r) {
    const int minW = (int)S(200);
    if (r->right - r->left < minW) {
        if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT) r->left = r->right - minW;
        else r->right = r->left + minW;
    }
    if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
        r->right = r->left + std::max(minW, (int)((r->bottom - r->top) * s_aspect));
    } else {
        const int hgt = (int)((r->right - r->left) / s_aspect);
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT) r->top = r->bottom - hgt;
        else r->bottom = r->top + hgt;
    }
}

LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        const RECT rc = WindowRc();
        const int x = p.x - rc.left, y = p.y - rc.top, e = (int)S(6), w = rc.right - rc.left, hh = rc.bottom - rc.top;
        const bool l = x < e, r = x >= w - e, t = y < e, b = y >= hh - e;
        LPCWSTR cur = IDC_ARROW;
        if ((l && t) || (r && b)) cur = IDC_SIZENWSE;
        else if ((r && t) || (l && b)) cur = IDC_SIZENESW;
        else if (l || r) cur = IDC_SIZEWE;
        else if (t || b) cur = IDC_SIZENS;
        else if (HitAt((float)x, (float)y) != kDrag) cur = IDC_HAND;
        SetCursor(LoadCursor(nullptr, cur));
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        if (s_down == kVolume) { SetVolume(VolumeAt(Layout(), (float)GET_Y_LPARAM(lParam))); return 0; }
        const int hit = HitAt((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
        if (hit == kMute || hit == kVolume) s_volumeHoverAt = GetTickCount();
        if (hit != s_hover) { s_hover = hit; RenderOverlay(); }
        return 0;
    }
    case WM_MOUSEWHEEL: {                                // volume
        const double step = GET_WHEEL_DELTA_WPARAM(wParam) / (double)WHEEL_DELTA * 0.05;
        SetVolume(std::min(1.0, std::max(0.0, (s_state.muted ? 0 : s_state.volume) + step)));
        s_volumeHoverAt = GetTickCount();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam), y = GET_Y_LPARAM(lParam);
        const RECT rc = WindowRc();
        const int e = (int)S(6), w = rc.right - rc.left, hh = rc.bottom - rc.top;
        const bool l = x < e, r = x >= w - e, t = y < e, b = y >= hh - e;
        int size = 0;
        if (l && t) size = WMSZ_TOPLEFT; else if (r && t) size = WMSZ_TOPRIGHT;
        else if (l && b) size = WMSZ_BOTTOMLEFT; else if (r && b) size = WMSZ_BOTTOMRIGHT;
        else if (l) size = WMSZ_LEFT; else if (r) size = WMSZ_RIGHT; else if (t) size = WMSZ_TOP; else if (b) size = WMSZ_BOTTOM;
        if (size) {                                      // resize the window by the overlay's edge
            ReleaseCapture();
            SendMessageW(s_window, WM_SYSCOMMAND, SC_SIZE | size, 0);
            return 0;
        }
        s_down = HitAt((float)x, (float)y);
        if (s_down == kVolume) {
            SetCapture(h);
            SetVolume(VolumeAt(Layout(), (float)y));
            return 0;
        }
        if (s_down == kDrag) {                           // move the window by any empty part
            s_down = 0;
            ReleaseCapture();
            SendMessageW(s_window, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
            return 0;
        }
        if (s_down == kProgress) {
            const OverlayLayout L = Layout();
            const double frac = std::min(1.0, std::max(0.0, (double)(x - L.progress.X) / L.progress.Width));
            Command("seekTo", frac * s_state.duration);
            s_down = 0;
            return 0;
        }
        SetCapture(h);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (s_down == kVolume) {
            s_down = 0;
            if (GetCapture() == h) ReleaseCapture();
            s_volumeHoverAt = GetTickCount();
            RenderOverlay();
            return 0;
        }
        const int hit = HitAt((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
        const int down = s_down;
        s_down = 0;
        if (GetCapture() == h) ReleaseCapture();
        if (hit != down) return 0;
        switch (hit) {
        case kPlay:   Command("toggle"); break;
        case kBack10: Command("seek", -10); break;
        case kFwd10:  Command("seek", 10); break;
        case kMute:   Command("mute"); break;
        case kToTab:  PipExit(false, true); break;
        case kClose:  PipExit(true, false); break;
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wParam) return 0;                            // no frame at all
        break;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_SIZING:
        KeepAspect(wParam, (RECT*)lParam);
        return TRUE;
    case WM_SIZE:
        PlaceView();
        EnsureVideoInWindow(true);
        return 0;
    case WM_MOVE:
        PlaceView();
        return 0;
    case WM_LBUTTONDOWN:                                 // the video itself (under the overlay's clear parts)
        s_pressed = true;
        s_pressAt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE:
        if (s_pressed && (std::abs(GET_X_LPARAM(lParam) - s_pressAt.x) > 4 || std::abs(GET_Y_LPARAM(lParam) - s_pressAt.y) > 4)) {
            s_pressed = false;
            ReleaseCapture();
            SendMessageW(h, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s_pressed) {
            s_pressed = false;
            ReleaseCapture();
            Command("toggle");
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (s_overlay) SendMessageW(s_overlay, WM_MOUSEWHEEL, wParam, lParam);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect((HDC)wParam, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_TIMER: {
        if (wParam != kTimer) break;
        POINT p;
        GetCursorPos(&p);
        const RECT rc = WindowRc();
        const bool inside = PtInRect(&rc, p) != 0;
        const DWORD now = GetTickCount();
        if (inside) s_lastInside = now;
        const bool busy = GetCapture() == s_overlay || (GetKeyState(VK_LBUTTON) & 0x8000);
        ShowOverlay(inside || busy || now - s_lastInside < 400);
        if (s_overlayShown && now - s_lastPoll > 500) Poll();
        if (s_overlayShown && VolumeShown() != s_volumeDrawn) RenderOverlay();   // the slider's moment after
        if (now - s_lastEnsure > 500) EnsureVideoInWindow(false);
        return 0;
    }
    case WM_CLOSE:
        PipExit(true, false);
        return 0;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

bool CreatePipWindow(const std::wstring& title) {
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraPictureInPicture";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = (HICON)SendMessageW(g_hWnd, WM_GETICON, ICON_BIG, 0);
        if (!RegisterClassExW(&wc)) return false;
        WNDCLASSEXW oc = {};
        oc.cbSize = sizeof(oc);
        oc.lpfnWndProc = OverlayProc;
        oc.hInstance = hi;
        oc.lpszClassName = L"LyraPictureInPictureControls";
        oc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&oc)) return false;
        registered = true;
    }
    auto icons = [](float px) {
        auto f = std::make_unique<Font>(L"Segoe Fluent Icons", px, FontStyleRegular, UnitPixel);
        if (!f->IsAvailable()) f = std::make_unique<Font>(L"Segoe MDL2 Assets", px, FontStyleRegular, UnitPixel);
        return f;
    };
    s_fGlyph = icons(S(14));
    s_fBig = icons(S(20));
    s_fText = std::make_unique<Font>(L"Segoe UI", S(11), FontStyleBold, UnitPixel);
    s_title = title;
    s_state = VideoState();

    // Bottom right of the screen Lyra is on; the whole window is the video.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(g_hWnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int w = (int)S(480), hgt = (int)(w / s_aspect);
    const int x = mi.rcWork.right - w - (int)S(24), y = mi.rcWork.bottom - hgt - (int)S(24);
    s_window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"LyraPictureInPicture", title.c_str(),
                               WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN, x, y, w, hgt, nullptr, nullptr, hi, nullptr);
    if (!s_window) return false;
    const DWORD round = 2;                               // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(s_window, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(s_window, kDwmDarkMode, &dark, sizeof(dark));
    s_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                L"LyraPictureInPictureControls", L"", WS_POPUP, x, y, w, hgt, s_window, nullptr, hi, nullptr);
    SetTimer(s_window, kTimer, 60, nullptr);
    return true;
}

// "Back to tab": a picture of the window flies to where the video is in the tab and fades there,
// over the page laying out again underneath (a layered, click-through, topmost window).
HWND s_fly = nullptr;
HBITMAP s_flyBitmap = nullptr;
SIZE s_flySize = {};
RECT s_flyFrom = {}, s_flyTo = {}, s_flyNow = {};
RECT s_flyClip = {};                   // the tab's view: the picture lands inside it (0: no clip yet)
DWORD s_flyCreated = 0, s_flyStart = 0;
bool s_flyHasTarget = false;
DWORD s_flyReadyAt = 0;                // the page has laid out again (0: not yet)
DWORD s_flyFadeStart = 0;
constexpr DWORD kFlyHoldMaxMs = 1200;  // longest wait over the landed video for the page
constexpr DWORD kFlyFadeMs = 160;
constexpr UINT_PTR kFlyTimer = 2;
constexpr DWORD kFlyMs = 300;          // the flight
constexpr DWORD kFlyWaitMs = 500;      // longest wait for the page to say where the video is

void DestroyFly() {
    if (s_fly) { HWND f = s_fly; s_fly = nullptr; KillTimer(f, kFlyTimer); DestroyWindow(f); }
    if (s_flyBitmap) { DeleteObject(s_flyBitmap); s_flyBitmap = nullptr; }
    s_flyHasTarget = false;
    s_flyReadyAt = s_flyFadeStart = 0;
}

void RenderFly(const RECT& r, BYTE alpha, const RECT& clip) {
    const int w = std::max(1L, r.right - r.left), hgt = std::max(1L, r.bottom - r.top);
    RECT shown = { r.left, r.top, r.left + w, r.top + hgt };
    if (!IsRectEmpty(&clip) && !IntersectRect(&shown, &shown, &clip)) shown = { r.left, r.top, r.left + 1, r.top + 1 };
    HDC screen = GetDC(nullptr);
    HDC src = CreateCompatibleDC(screen);
    HDC dst = CreateCompatibleDC(screen);
    HBITMAP frame = CreateCompatibleBitmap(screen, w, hgt);
    HGDIOBJ oldSrc = SelectObject(src, s_flyBitmap);
    HGDIOBJ oldDst = SelectObject(dst, frame);
    SetStretchBltMode(dst, HALFTONE);
    SetBrushOrgEx(dst, 0, 0, nullptr);
    StretchBlt(dst, 0, 0, w, hgt, src, 0, 0, s_flySize.cx, s_flySize.cy, SRCCOPY);
    POINT pos = { shown.left, shown.top };
    SIZE size = { shown.right - shown.left, shown.bottom - shown.top };
    POINT from = { shown.left - r.left, shown.top - r.top };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, alpha, 0 };
    UpdateLayeredWindow(s_fly, screen, &pos, &size, dst, &from, 0, &blend, ULW_ALPHA);
    SelectObject(src, oldSrc);
    SelectObject(dst, oldDst);
    DeleteObject(frame);
    DeleteDC(src);
    DeleteDC(dst);
    ReleaseDC(nullptr, screen);
    s_flyNow = r;
}

void SetFlyTarget(const RECT& to) {
    s_flyFrom = s_flyNow;
    s_flyTo = to;
    s_flyStart = GetTickCount();
    s_flyHasTarget = true;
}

// Nowhere to land (the video is off screen, or the page did not answer): shrink a little and fade.
void SetFlyFallbackTarget() {
    const RECT& r = s_flyNow;
    const int w = r.right - r.left, hgt = r.bottom - r.top;
    RECT to = { r.left + w / 8, r.top + hgt / 8, r.right - w / 8, r.bottom - hgt / 8 };
    SetFlyTarget(to);
}

void FlyStep() {
    if (!s_fly) return;
    const DWORD now = GetTickCount();
    if (!s_flyHasTarget) {
        if (now - s_flyCreated > kFlyWaitMs) SetFlyFallbackTarget();
        else return;
    }
    const float t = std::min(1.f, (float)(now - s_flyStart) / kFlyMs);
    const float e = 1.f - (1.f - t) * (1.f - t) * (1.f - t);          // ease out
    auto lerp = [e](LONG a, LONG b) { return (LONG)std::lround(a + (b - a) * e); };
    const RECT r = { lerp(s_flyFrom.left, s_flyTo.left), lerp(s_flyFrom.top, s_flyTo.top),
                     lerp(s_flyFrom.right, s_flyTo.right), lerp(s_flyFrom.bottom, s_flyTo.bottom) };
    // Landed: stay over the video until the page shows it again (it lays out at its own size
    // first), then fade into it.
    if (!s_flyFadeStart && t >= 1.f && ((s_flyReadyAt && now >= s_flyReadyAt) || now - s_flyCreated > kFlyHoldMaxMs))
        s_flyFadeStart = now;
    const float fade = s_flyFadeStart ? 1.f - std::min(1.f, (float)(now - s_flyFadeStart) / kFlyFadeMs) : 1.f;
    // The clip closes in on the tab's view as the picture lands (it starts outside the window).
    RECT clip = {};
    if (!IsRectEmpty(&s_flyClip)) {
        const RECT screen = { GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                              GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                              GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN) };
        clip = { lerp(screen.left, s_flyClip.left), lerp(screen.top, s_flyClip.top), lerp(screen.right, s_flyClip.right), lerp(screen.bottom, s_flyClip.bottom) };
    }
    RenderFly(r, (BYTE)std::lround(255 * fade), clip);
    if (s_flyFadeStart && fade <= 0.f) DestroyFly();
}

LRESULT CALLBACK FlyProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_TIMER:
        if (wParam == kFlyTimer) { FlyStep(); return 0; }
        break;
    }
    return DefWindowProcW(h, msg, wParam, lParam);
}

// A picture of the picture-in-picture window, shown where it is.
bool StartFly() {
    // A previous flight, if any (the picture of the tab stays: it is for this one).
    if (s_fly) { HWND f = s_fly; s_fly = nullptr; KillTimer(f, kFlyTimer); DestroyWindow(f); }
    if (s_flyBitmap) { DeleteObject(s_flyBitmap); s_flyBitmap = nullptr; }
    s_flyHasTarget = false;
    s_flyReadyAt = s_flyFadeStart = 0;
    if (!s_window) return false;
    static bool registered = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = FlyProc;
        wc.hInstance = hi;
        wc.lpszClassName = L"LyraPictureInPictureFly";
        if (!RegisterClassExW(&wc)) return false;
        registered = true;
    }
    RECT rc = WindowRc();
    s_flySize = { rc.right - rc.left, rc.bottom - rc.top };
    if (s_flySize.cx <= 0 || s_flySize.cy <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    s_flyBitmap = CreateCompatibleBitmap(screen, s_flySize.cx, s_flySize.cy);
    HGDIOBJ old = SelectObject(mem, s_flyBitmap);
    // What is on screen there (the video is drawn by the GPU: PrintWindow sees black), without the
    // controls over it.
    if (s_overlay && IsWindowVisible(s_overlay)) {
        ShowWindow(s_overlay, SW_HIDE);
        s_overlayShown = false;
        DwmFlush();
    }
    BitBlt(mem, 0, 0, s_flySize.cx, s_flySize.cy, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    s_fly = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"LyraPictureInPictureFly", L"", WS_POPUP, rc.left, rc.top, s_flySize.cx, s_flySize.cy,
                            nullptr, nullptr, hi, nullptr);
    if (!s_fly) { DestroyFly(); return false; }
    const DWORD round = 2;                               // DWMWCP_ROUND, like the window it pictures
    DwmSetWindowAttribute(s_fly, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
    s_flyClip = {};
    RenderFly(rc, 255, {});
    ShowWindow(s_fly, SW_SHOWNOACTIVATE);
    s_flyCreated = GetTickCount();
    s_flyHasTarget = false;
    SetTimer(s_fly, kFlyTimer, 7, nullptr);
    return true;
}

// The page's answer to __lyraPip.leaveMeasured(): where the video is in the tab.
// Where the video is (CSS rect and viewport, as the page reports) on the screen, in the tab's view.
bool VideoScreenRect(Tab& t, const float* v, RECT& out) {
    RECT view;
    if (!t.hwnd || !IsWindowVisible(t.hwnd) || !GetWindowRect(t.hwnd, &view)) return false;
    if (v[2] < 1 || v[3] < 1 || v[4] < 1 || v[5] < 1) return false;
    const float sx = (view.right - view.left) / v[4], sy = (view.bottom - view.top) / v[5];
    out = { view.left + (LONG)std::lround(v[0] * sx), view.top + (LONG)std::lround(v[1] * sy),
            view.left + (LONG)std::lround((v[0] + v[2]) * sx), view.top + (LONG)std::lround((v[1] + v[3]) * sy) };
    RECT visible;
    if (!IntersectRect(&visible, &out, &view)) return false;
    s_flyClip = view;
    return true;
}

void OnLeaveMeasured(WKTypeRef result, WKErrorRef, void* ctx) {
    if (!s_fly) return;
    s_flyReadyAt = GetTickCount() + 60;                  // a frame or two (the page did not change)
    const unsigned long long tabId = (unsigned long long)(uintptr_t)ctx;
    std::wstring s;
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) s = WKStr((WKStringRef)result);
    Tab* t = nullptr;
    for (auto& tp : g_tabs) if (tp->id == tabId && tp->state == TabState::Live) t = tp.get();
    float v[6] = {};
    RECT to;
    if (s_flyHasTarget && !IsRectEmpty(&s_flyClip)) return;
    if (!t || s.empty() || swscanf_s(s.c_str(), L"%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6
        || !VideoScreenRect(*t, v, to)) {
        if (!s_flyHasTarget) SetFlyFallbackTarget();
        return;
    }
    // Already flying to where the video was when it left (the page is scrolled back there): keep
    // that. The answer only says the page is ready; it may come while the page still lays out.
    if (s_flyHasTarget) return;
    SetFlyTarget(to);
}

void DestroyPipWindow() {
    if (s_overlay) { HWND o = s_overlay; s_overlay = nullptr; DestroyWindow(o); }
    if (s_window) { HWND h = s_window; s_window = nullptr; KillTimer(h, kTimer); DestroyWindow(h); }
    s_tabId = 0;
    s_hover = s_down = 0;
    s_overlayShown = s_polling = s_pressed = false;
}

void OnEntered(WKTypeRef result, WKErrorRef, void* ctx) {
    std::unique_ptr<StartContext> start((StartContext*)ctx);
    const unsigned long long tabId = start->tabId;
    std::wstring size;
    if (result && WKGetTypeID(result) == WKStringGetTypeID()) size = WKStr((WKStringRef)result);
    Tab* t = nullptr;
    for (auto& tp : g_tabs) if (tp->id == tabId && tp->state == TabState::Live) t = tp.get();
    if (start->seq != s_startSeq) {
        // Cancelled (exit before the answer) or replaced by a start on another tab. The page marked
        // its video: unmark it, unless that tab is in picture-in-picture now or starts again (its
        // enter() comes later and marks the video anew; a leave() now would run after it).
        Log("pip: stale start #%u for tab#%llu", start->seq, tabId);
        const bool inUse = (s_window && s_tabId == tabId) || (s_startSeq && s_startTab == tabId);
        if (t && !size.empty() && !inUse) RunJs(t->page, "window.__lyraPip && __lyraPip.leave()");
        return;
    }
    s_startSeq = 0;
    if (!t || !t->hwnd || !t->view) return;
    if (size.rfind(L"ERR ", 0) == 0) { Log("pip: enter() failed: %s", ToUtf8(size).c_str()); size.clear(); }
    if (size.empty()) { Log("pip: no video on tab#%llu", tabId); MessageBeep(MB_ICONWARNING); return; }
    if (s_window) DestroyPipWindow();     // (cannot happen: starts wait for each other) never two windows
    int vw = 16, vh = 9;
    swscanf_s(size.c_str(), L"%dx%d", &vw, &vh);
    s_aspect = (vw > 0 && vh > 0) ? std::min(3.f, std::max(0.5f, (float)vw / (float)vh)) : 16.f / 9.f;
    if (!CreatePipWindow(t->title)) { RunJs(t->page, "window.__lyraPip && __lyraPip.leave()"); return; }
    s_tabId = t->id;
    ShowWindow(s_window, SW_SHOWNOACTIVATE);
    EnsureVideoInWindow(false);           // the tab stays as it is, usable
    Log("pip: tab#%llu in picture-in-picture (%dx%d)", t->id, vw, vh);
}

void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
    std::wstring what;
    WKTypeRef body = WKScriptMessageGetBody(message);
    if (body && WKGetTypeID(body) == WKArrayGetTypeID() && WKArrayGetSize((WKArrayRef)body) > 0) {
        WKTypeRef item = WKArrayGetItemAtIndex((WKArrayRef)body, 0);
        if (item && WKGetTypeID(item) == WKStringGetTypeID()) what = WKStr((WKStringRef)item);
    }
    WKPageRef page = nullptr;
    if (WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message)) page = WKFrameInfoGetPage(frame);
    for (auto& t : g_tabs)
        if (t->page == page && t->state == TabState::Live) {
            // Not inside WebKit's callback: windows are created and views moved on the main loop.
            PostMessageW(g_hWnd, WM_APP_PIP, (WPARAM)t->id, what == L"request" ? 1 : 0);
            break;
        }
    WKStringRef r = WKStringCreateWithUTF8CString("");
    WKCompletionListenerComplete(reply, r);
    WKRelease(r);
}

} // namespace

void PipInit() {
    WKUserContentControllerRef controller = AdBlockUserContentController();
    if (!controller) return;
    WKStringRef name = WKStringCreateWithUTF8CString("lyraPip");
    WKUserContentControllerAddScriptMessageHandler(controller, name, OnMessage, nullptr);
    WKRelease(name);
    AdBlockAddUserScript(kScript, true);
}

bool PipActive() { return s_window != nullptr; }

void PipStart(Tab& t) {
    if (!t.page || !t.hwnd) return;
    if (s_startSeq && s_startTab == t.id) { Log("pip: tab#%llu is already starting", t.id); return; }
    if (s_window && s_tabId == t.id) { Log("pip: tab#%llu is already in picture-in-picture", t.id); return; }
    DestroyFly();
    if (s_window) PipExit(false, false);                 // one at a time
    static unsigned counter = 0;
    if (++counter == 0) ++counter;
    s_startSeq = counter;
    s_startTab = t.id;
    // In a try: an exception in enter() reaches Lyra as "ERR ..." (this API drops script errors).
    const std::wstring call = L"(function(){try{return window.__lyraPip ? window.__lyraPip.enter(" +
        JsString(Tr(L"Playing in picture-in-picture", L"Воспроизводится в окне «картинка в картинке»")) + L", " +
        JsString(Tr(L"Back to tab", L"Вернуть во вкладку")) + L") : '';}catch(e){return 'ERR '+e+' '+(e&&e.stack);}})()";
    WKStringRef js = WKStringCreateWithUTF8CString(ToUtf8(call).c_str());
    WKPageEvaluateJavaScriptInMainFrame(t.page, js, new StartContext { t.id, s_startSeq }, OnEntered);
    WKRelease(js);
}

void PipToggleActive() {
    if (s_window) { PipExit(false, true); return; }
    if (s_startSeq) { Log("pip: start cancelled"); s_startSeq = 0; return; }   // pressed again before the window
    if (Tab* t = ActiveTab()) PipStart(*t);
}

void PipExit(bool pause, bool showTab) {
    Tab* t = PipTab();
    Log("pip: back%s", pause ? " (paused)" : "");
    // "Back to tab" with the main window on screen: the window's picture flies to the video (the
    // page is asked below where it is, once the tab is shown).
    const bool fly = t && showTab && !pause && !IsIconic(g_hWnd) && StartFly();
    if (t) {
        WKPageSetVideoPictureInPictureWindow(t->page, 0, false);
        if (pause) RunJs(t->page, "window.__lyraPip && (__lyraPip.pause(), __lyraPip.leave())");
        else if (!fly) RunJs(t->page, "window.__lyraPip && __lyraPip.leave()");
    }
    DestroyPipWindow();
    if (!t) return;
    int index = -1;
    for (int i = 0; i < (int)g_tabs.size(); ++i) if (g_tabs[i].get() == t) index = i;
    if (showTab && index >= 0) {
        if (IsIconic(g_hWnd)) ShowWindow(g_hWnd, SW_RESTORE);
        SetForegroundWindow(g_hWnd);
        SelectTab(index);
    } else if (index == g_active) {
        SelectTab(index);                                // shows the view again
    }
    RefreshLayout();
    InvalidateRect(g_hWnd, nullptr, FALSE);
    if (fly) {
        WKStringRef js = WKStringCreateWithUTF8CString("return window.__lyraPip ? await window.__lyraPip.back() : ''");
        WKMutableDictionaryRef args = WKMutableDictionaryCreate();
        WKPageCallAsyncJavaScript(t->page, js, (WKDictionaryRef)args, nullptr, (void*)(uintptr_t)t->id, OnLeaveMeasured);
        WKRelease(args);
        WKRelease(js);
    }
}

void PipDetach(Tab& t) {
    if (s_startSeq && s_startTab == t.id) s_startSeq = 0;
    if (!s_window || s_tabId != t.id) return;
    DestroyFly();
    if (t.page) WKPageSetVideoPictureInPictureWindow(t.page, 0, false);
    DestroyPipWindow();
}

void PipMessage(WPARAM tabId, LPARAM what) {
    Log("pip: %s for tab#%llu", what == 1 ? "request" : "exit", (unsigned long long)tabId);
    if (what == 1) {
        for (auto& t : g_tabs) if (t->id == (unsigned long long)tabId && t->state == TabState::Live) { PipStart(*t); return; }
    } else if (s_window && s_tabId == (unsigned long long)tabId) {
        PipExit(false, true);
    } else if (s_startSeq && s_startTab == (unsigned long long)tabId) {
        Log("pip: start cancelled");                     // exit before the window: no window at all
        s_startSeq = 0;
    }
}
