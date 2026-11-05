// Lyra — поле ввода адреса.
#include "ui/AddressBar.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

HWND   g_urlEdit = nullptr;

WNDPROC g_oldEditProc = nullptr;

HFONT  g_editFont = nullptr;

HBRUSH g_editBrush = nullptr;

int    g_editTextH = 18;

bool IsEditing() { return g_urlEdit && IsWindowVisible(g_urlEdit); }

void PositionEdit() {
    if (!g_urlEdit) return;
    const float padX = S(14);
    int x = (int)(g_L.pill.X + padX);
    int w = (int)std::max(10.f, g_L.pill.Width - padX * 2);
    int h = g_editTextH;
    int y = (int)(g_L.pill.Y + (g_L.pill.Height - (float)h) / 2);
    MoveWindow(g_urlEdit, x, y, w, h, TRUE);
}

void ShowUrlEdit() {
    if (!g_urlEdit) return;
    Tab* t = ActiveTab();
    std::wstring text = (t && t->url != L"about:blank") ? t->url : L"";
    SetWindowTextW(g_urlEdit, text.c_str());
    PositionEdit();
    ShowWindow(g_urlEdit, SW_SHOW);
    SetFocus(g_urlEdit);
    SendMessageW(g_urlEdit, EM_SETSEL, 0, -1);
    InvalidateChrome();
}

// ----------------------------------------------------------------------------
//  Address field subclass
// ----------------------------------------------------------------------------
LRESULT CALLBACK UrlEditProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_KEYDOWN:
        if (w == VK_RETURN) {
            wchar_t buf[2048] = {};
            GetWindowTextW(h, buf, ARRAYSIZE(buf));
            NavigateActive(buf);
            FocusWebView();
            return 0;
        }
        if (w == VK_ESCAPE) { FocusWebView(); return 0; }
        if (w == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SendMessageW(h, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_CHAR:
        if (w == VK_RETURN || w == VK_ESCAPE || w == 1) return 0;  // no beep
        break;
    }
    return CallWindowProcW(g_oldEditProc, h, msg, w, l);
}
