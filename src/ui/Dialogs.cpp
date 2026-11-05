// Lyra — модальные диалоги (ввод текста для prompt()).
#include "ui/Dialogs.h"
#include "core/Localization.h"

static INT_PTR CALLBACK PromptDlgProc(HWND h, UINT msg, WPARAM w, LPARAM l);

int g_modalDepth = 0;                          // >0 while a dialog runs inside a WebKit callback

static INT_PTR CALLBACK PromptDlgProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_INITDIALOG: {
        PromptState* st = (PromptState*)l;
        SetWindowLongPtrW(h, DWLP_USER, (LONG_PTR)st);
        SetDlgItemTextW(h, 100, st->message.c_str());
        SetDlgItemTextW(h, 101, st->value.c_str());
        SendDlgItemMessageW(h, 101, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(h, 101));
        return FALSE;   // focus was set manually
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            PromptState* st = (PromptState*)GetWindowLongPtrW(h, DWLP_USER);
            if (st && LOWORD(w) == IDOK) {
                int n = GetWindowTextLengthW(GetDlgItem(h, 101));
                std::wstring v(n + 1, L'\0');
                GetDlgItemTextW(h, 101, &v[0], n + 1);
                v.resize(n);
                st->value = v;
                st->ok = true;
            }
            EndDialog(h, LOWORD(w));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool PromptDialog(HWND owner, const std::wstring& title, PromptState& st) {
    std::vector<WORD> t;
    auto align = [&]() { if (t.size() & 1) t.push_back(0); };   // DWORD alignment (2 WORDs)
    auto str = [&](const wchar_t* s) { do { t.push_back((WORD)*s); } while (*s++); };
    auto item = [&](DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const wchar_t* text) {
        align();
        DLGITEMTEMPLATE it = {};
        it.style = style | WS_CHILD | WS_VISIBLE;
        it.x = x; it.y = y; it.cx = cx; it.cy = cy; it.id = id;
        const WORD* p = (const WORD*)&it;
        t.insert(t.end(), p, p + sizeof(it) / sizeof(WORD));
        t.push_back(0xFFFF); t.push_back(cls);   // predefined class atom
        str(text);
        t.push_back(0);                          // no creation data
    };
    DLGTEMPLATE d = {};
    d.style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d.cdit = 4; d.cx = 260; d.cy = 92;
    const WORD* p = (const WORD*)&d;
    t.insert(t.end(), p, p + sizeof(d) / sizeof(WORD));
    t.push_back(0); t.push_back(0);              // no menu, default class
    str(title.c_str());
    t.push_back(9); str(L"Segoe UI");            // DS_SETFONT: point size + face
    item(SS_LEFT | SS_NOPREFIX, 8, 7, 244, 34, 100, 0x0082, L"");
    item(ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 44, 244, 14, 101, 0x0081, L"");
    item(BS_DEFPUSHBUTTON | WS_TABSTOP, 148, 70, 50, 14, IDOK, 0x0080, L"OK");
    item(BS_PUSHBUTTON | WS_TABSTOP, 202, 70, 50, 14, IDCANCEL, 0x0080, Tr(L"Cancel", L"Отмена"));
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), (LPCDLGTEMPLATEW)t.data(), owner, PromptDlgProc, (LPARAM)&st);
    return st.ok;
}
