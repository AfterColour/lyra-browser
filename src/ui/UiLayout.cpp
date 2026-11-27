// Lyra — раскладки интерфейса (см. UiLayout.h).
#include "ui/UiLayout.h"
#include "app/Log.h"
#include "core/Json.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/Customize.h"
#include "ui/settings/SettingsWindow.h"

static std::string ReadFileUtf8(const std::wstring& path) {
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart > 0 && size.QuadPart < (4 << 20)) {
        data.resize((size_t)size.QuadPart);
        DWORD read = 0;
        ReadFile(f, data.data(), (DWORD)data.size(), &read, nullptr);
        data.resize(read);
    }
    CloseHandle(f);
    return data;
}

static bool WriteFileAtomic(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
void ThemeReapplyUiColors();                                           // ui/Theme.cpp

static std::vector<UiLayoutDef> s_custom;
static std::wstring s_activeId;                      // "" = the built-in layout g_prefs.tabLayout
static UiLayoutDef s_resolved;                       // UiActive()
static bool s_resolvedValid = false;

static std::wstring LayoutsFile() { return g_settingsDir + L"\\layouts.json"; }

// ---- items ---------------------------------------------------------------------------------------
const wchar_t* UiItemName(int item) {
    switch (item) {
    case kItemBack:       return Tr(L"Back", L"Назад");
    case kItemForward:    return Tr(L"Forward", L"Вперёд");
    case kItemReload:     return Tr(L"Reload", L"Обновить");
    case kItemHome:       return Tr(L"Home page", L"Домашняя страница");
    case kItemNewTab:     return Tr(L"New tab", L"Новая вкладка");
    case kItemDownloads:  return Tr(L"Downloads", L"Загрузки");
    case kItemExtensions: return Tr(L"Extensions", L"Расширения");
    case kItemSidebar:    return Tr(L"Sidebar", L"Боковая панель");
    case kItemMenu:       return Tr(L"Menu", L"Меню");
    case kItemWorkspaces: return Tr(L"Workspaces", L"Пространства");
    }
    return L"";
}

const wchar_t* UiItemGlyph(int item) {
    switch (item) {
    case kItemBack:       return L"";
    case kItemForward:    return L"";
    case kItemReload:     return L"";
    case kItemHome:       return L"";
    case kItemNewTab:     return L"";
    case kItemDownloads:  return L"";
    case kItemExtensions: return L"";
    case kItemSidebar:    return L"";
    case kItemMenu:       return L"";
    case kItemWorkspaces: return L"\uE8A9";
    }
    return L"";
}

const wchar_t* UiColorName(int c) {
    switch (c) {
    case kColFrame:       return Tr(L"Panels", L"Панели");
    case kColField:       return Tr(L"Address field", L"Адресная строка");
    case kColFieldBorder: return Tr(L"Borders", L"Рамки");
    case kColTabActive:   return Tr(L"Active tab", L"Активная вкладка");
    case kColText:        return Tr(L"Text", L"Текст");
    case kColText2:       return Tr(L"Secondary text", L"Второстепенный текст");
    case kColIcon:        return Tr(L"Icons", L"Значки");
    case kColAccent:      return Tr(L"Accent", L"Акцент");
    case kColHover:       return Tr(L"Hover", L"Подсветка");
    }
    return L"";
}

// The extensions' buttons are a row of their own: not inside the address field.
bool UiItemAllowedIn(int item, int zone) {
    return !(zone == kZoneField && item == kItemExtensions);
}

bool UiZoneInMode(int zone, int mode) {
    if (zone == kZoneStripLeft || zone == kZoneStripRight) return mode == kLayoutSeparate;
    if (zone == kZoneSideTop) return mode == kLayoutVertical;
    return true;
}

int UiItemZone(const UiLayoutDef& d, int item, int* index) {
    for (int z = 0; z < kZoneCount; ++z) {
        if (!UiZoneInMode(z, d.mode)) continue;
        for (size_t i = 0; i < d.zones[z].size(); ++i)
            if (d.zones[z][i] == item) { if (index) *index = (int)i; return z; }
    }
    return -1;
}

// ---- built-in layouts ----------------------------------------------------------------------------
UiLayoutDef UiBuiltin(int mode) {
    mode = std::max(0, std::min(2, mode));
    UiLayoutDef d;
    d.id = L"builtin:" + std::to_wstring(mode);
    d.mode = mode;
    static const wchar_t* const kNames[3][2] = {
        { L"Compact", L"Компактная" }, { L"Separate tab bar", L"Отдельная строка вкладок" }, { L"Vertical tabs", L"Вертикальные вкладки" } };
    d.name = Tr(kNames[mode][0], kNames[mode][1]);
    //                         bar strip btn  tab icon font sideW
    static const float kDims[3][7] = {
        { 36, 34, 28, 26, 14, 12, 220 },             // compact
        { 42, 38, 32, 30, 15, 13, 232 },             // normal
        { 48, 42, 34, 34, 16, 13, 244 },             // large
    };
    const float* k = kDims[std::max(0, std::min(2, g_prefs.density))];
    UiStyle& s = d.style;
    s.bar = k[0]; s.strip = k[1]; s.btn = k[2]; s.tab = k[3]; s.icon = k[4]; s.font = k[5]; s.sideW = k[6];
    s.tabMaxW = mode == kLayoutSeparate ? 200.f : 150.f;
    s.fieldMaxW = mode == kLayoutCompact ? 860.f : mode == kLayoutVertical ? 640.f : 0.f;
    d.zones[kZoneRight] = { kItemExtensions, kItemDownloads, kItemMenu };
    d.zones[kZoneTabEnd] = { kItemNewTab };
    if (mode == kLayoutSeparate) {
        d.zones[kZoneLeft] = { kItemBack, kItemForward, kItemReload };
        d.zones[kZoneStripLeft] = { kItemWorkspaces };
    } else {
        d.zones[kZoneLeft] = { kItemBack, kItemForward };
        d.zones[kZoneField] = { kItemReload };
    }
    if (mode == kLayoutCompact) d.zones[kZoneLeft].push_back(kItemWorkspaces);   // left of the tabs
    if (mode == kLayoutVertical) d.zones[kZoneSideTop] = { kItemWorkspaces };    // above the list
    return d;
}

// ---- JSON ----------------------------------------------------------------------------------------
static std::string ColorHex(const Color& c) {
    if (!c.GetA()) return "";
    char s[12];
    snprintf(s, sizeof(s), "#%02X%02X%02X", c.GetR(), c.GetG(), c.GetB());
    return s;
}

static Color ParseColor(const std::string& s) {
    if (s.size() != 7 || s[0] != '#') return Color(0, 0, 0, 0);
    const unsigned v = (unsigned)strtoul(s.c_str() + 1, nullptr, 16);
    return Color(255, (BYTE)(v >> 16), (BYTE)(v >> 8), (BYTE)v);
}

static const char* const kZoneKeys[kZoneCount] = { "left", "right", "field", "tabEnd", "stripLeft", "stripRight", "sideTop" };
static const char* const kItemKeys[kItemCount] = { "back", "forward", "reload", "home", "newTab", "downloads", "extensions", "sidebar", "menu", "workspaces" };
static const char* const kColorKeys[kColCount] = { "frame", "field", "fieldBorder", "tabActive", "text", "text2", "icon", "accent", "hover" };

static int ItemByKey(const std::string& k) {
    for (int i = 0; i < kItemCount; ++i) if (k == kItemKeys[i]) return i;
    return -1;
}

static std::string Num(float v) {
    char s[32];
    snprintf(s, sizeof(s), "%g", v);
    return s;
}

static std::string LayoutJson(const UiLayoutDef& d) {
    const UiStyle& s = d.style;
    std::string j = "{\"id\":" + JsonQuote(ToUtf8(d.id)) + ",\"name\":" + JsonQuote(ToUtf8(d.name)) +
                    ",\"mode\":" + std::to_string(d.mode) + ",\"zones\":{";
    for (int z = 0; z < kZoneCount; ++z) {
        if (z) j += ",";
        j += std::string("\"") + kZoneKeys[z] + "\":[";
        for (size_t i = 0; i < d.zones[z].size(); ++i) {
            if (i) j += ",";
            j += std::string("\"") + kItemKeys[d.zones[z][i]] + "\"";
        }
        j += "]";
    }
    j += "},\"style\":{\"bar\":" + Num(s.bar) + ",\"strip\":" + Num(s.strip) + ",\"btn\":" + Num(s.btn) +
         ",\"tab\":" + Num(s.tab) + ",\"icon\":" + Num(s.icon) + ",\"font\":" + Num(s.font) + ",\"sideW\":" + Num(s.sideW) +
         ",\"tabMaxW\":" + Num(s.tabMaxW) + ",\"fieldMaxW\":" + Num(s.fieldMaxW) +
         ",\"tabRadius\":" + Num(s.tabRadius) + ",\"fieldRadius\":" + Num(s.fieldRadius) + ",\"btnRadius\":" + Num(s.btnRadius) +
         ",\"fieldBorder\":" + Num(s.fieldBorder) + ",\"tabBorder\":" + Num(s.tabBorder) +
         ",\"separators\":" + std::string(s.separators ? "true" : "false") +
         ",\"fontFamily\":" + JsonQuote(ToUtf8(s.fontFamily)) + ",\"colors\":{";
    bool first = true;
    for (int c = 0; c < kColCount; ++c) {
        if (!s.colors[c].GetA()) continue;
        if (!first) j += ",";
        first = false;
        j += std::string("\"") + kColorKeys[c] + "\":" + JsonQuote(ColorHex(s.colors[c]));
    }
    j += "}},\"items\":{";
    first = true;
    for (int i = 0; i < kItemCount; ++i) {
        const UiItemStyle& is = d.items[i];
        if (is.size <= 0 && !is.color.GetA()) continue;
        if (!first) j += ",";
        first = false;
        j += std::string("\"") + kItemKeys[i] + "\":{\"size\":" + Num(is.size) + ",\"color\":" + JsonQuote(ColorHex(is.color)) + "}";
    }
    return j + "}}";
}

static float NumOf(const JsonValue* o, const char* key, float fallback) {
    const JsonValue* v = o ? o->get(key) : nullptr;
    return v && v->type == JsonValue::Number ? (float)v->n : fallback;
}

static bool ParseLayout(const JsonValue& o, UiLayoutDef& d) {
    if (!o.isObject()) return false;
    d.id = ToWide(o.str("id"));
    if (d.id.empty() || d.id.compare(0, 8, L"builtin:") == 0) return false;
    d.name = ToWide(o.str("name"));
    d.mode = std::max(0, std::min(2, (int)NumOf(&o, "mode", 0)));
    const UiLayoutDef base = UiBuiltin(d.mode);
    d.style = base.style;
    if (const JsonValue* zones = o.get("zones")) {
        std::vector<char> used(kItemCount, 0);
        for (int z = 0; z < kZoneCount; ++z) {
            const JsonValue* a = zones->get(kZoneKeys[z]);
            if (!a || !a->isArray()) continue;
            for (const JsonValue& it : a->items) {
                const int item = it.isString() ? ItemByKey(it.s) : -1;
                if (item < 0 || used[item] || !UiItemAllowedIn(item, z)) continue;
                used[item] = 1;
                d.zones[z].push_back(item);
            }
        }
    } else {
        for (int z = 0; z < kZoneCount; ++z) d.zones[z] = base.zones[z];
    }
    if (const JsonValue* s = o.get("style")) {
        UiStyle& st = d.style;
        st.bar = NumOf(s, "bar", st.bar); st.strip = NumOf(s, "strip", st.strip); st.btn = NumOf(s, "btn", st.btn);
        st.tab = NumOf(s, "tab", st.tab); st.icon = NumOf(s, "icon", st.icon); st.font = NumOf(s, "font", st.font);
        st.sideW = NumOf(s, "sideW", st.sideW);
        st.tabMaxW = NumOf(s, "tabMaxW", st.tabMaxW); st.fieldMaxW = NumOf(s, "fieldMaxW", st.fieldMaxW);
        st.tabRadius = NumOf(s, "tabRadius", st.tabRadius); st.fieldRadius = NumOf(s, "fieldRadius", st.fieldRadius);
        st.btnRadius = NumOf(s, "btnRadius", st.btnRadius);
        st.fieldBorder = NumOf(s, "fieldBorder", st.fieldBorder); st.tabBorder = NumOf(s, "tabBorder", st.tabBorder);
        st.separators = s->flag("separators", true);
        st.fontFamily = ToWide(s->str("fontFamily"));
        if (const JsonValue* c = s->get("colors"))
            for (int k = 0; k < kColCount; ++k) st.colors[k] = ParseColor(c->str(kColorKeys[k]));
    }
    if (const JsonValue* items = o.get("items"))
        for (int i = 0; i < kItemCount; ++i)
            if (const JsonValue* it = items->get(kItemKeys[i])) {
                d.items[i].size = NumOf(it, "size", 0);
                d.items[i].color = ParseColor(it->str("color"));
            }
    return true;
}

static void SaveNow() {
    std::string j = "{\"active\":" + JsonQuote(ToUtf8(s_activeId)) + ",\"layouts\":[";
    for (size_t i = 0; i < s_custom.size(); ++i) {
        if (i) j += ",\n";
        j += LayoutJson(s_custom[i]);
    }
    j += "]}\n";
    if (!WriteFileAtomic(LayoutsFile(), j)) Log("layouts: could not write %ls", LayoutsFile().c_str());
}

static UINT_PTR s_saveTimer = 0;
static void CALLBACK SaveTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    s_saveTimer = 0;
    SaveNow();
}

static void SaveSoon() {
    if (s_saveTimer) KillTimer(nullptr, s_saveTimer);
    s_saveTimer = SetTimer(nullptr, 0, 700, SaveTimerProc);
    if (!s_saveTimer) SaveNow();
}

void UiLoad() {
    s_custom.clear();
    s_activeId.clear();
    s_resolvedValid = false;
    const std::string text = ReadFileUtf8(LayoutsFile());
    if (text.empty()) return;
    JsonValue root;
    std::string err;
    if (!JsonParse(text, root, &err) || !root.isObject()) { Log("layouts.json: %s", err.c_str()); return; }
    if (const JsonValue* a = root.get("layouts"); a && a->isArray())
        for (const JsonValue& o : a->items) {
            UiLayoutDef d;
            if (ParseLayout(o, d)) s_custom.push_back(std::move(d));
        }
    s_activeId = ToWide(root.str("active"));
    bool found = false;
    for (const UiLayoutDef& d : s_custom) if (d.id == s_activeId) found = true;
    if (!found) s_activeId.clear();
}

// ---- the active layout ---------------------------------------------------------------------------
static UiLayoutDef* FindCustom(const std::wstring& id) {
    for (UiLayoutDef& d : s_custom) if (d.id == id) return &d;
    return nullptr;
}

const UiLayoutDef& UiActive() {
    // A built-in layout depends on the toolbar size and the language (its name): resolved each time,
    // cheaply; a custom one is returned as it is.
    if (UiLayoutDef* d = s_activeId.empty() ? nullptr : FindCustom(s_activeId)) return *d;
    s_resolved = UiBuiltin(g_prefs.tabLayout);
    return s_resolved;
}

bool UiActiveIsBuiltin() { return s_activeId.empty() || !FindCustom(s_activeId); }
const std::vector<UiLayoutDef>& UiCustomLayouts() { return s_custom; }

static void Notify(bool listChanged) {
    if (listChanged) SettingsRebuild(); else SettingsRefresh();
    CustomizeModelChanged();
}

static void ApplyAll() {
    ThemeReapplyUiColors();
    RebuildResources();
    RefreshLayout();
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

void UiSelectBuiltin(int mode) {
    g_prefs.tabLayout = std::max(0, std::min(2, mode));
    const bool was = !s_activeId.empty();
    s_activeId.clear();
    SaveSettings();
    if (was) SaveSoon();
    ApplyAll();
    Notify(false);
}

void UiSelectCustom(const std::wstring& id) {
    if (!FindCustom(id)) return;
    s_activeId = id;
    SaveSoon();
    ApplyAll();
    Notify(false);
}

UiLayoutDef& UiEdit() {
    if (UiLayoutDef* d = s_activeId.empty() ? nullptr : FindCustom(s_activeId)) return *d;
    UiLayoutDef copy = UiBuiltin(g_prefs.tabLayout);
    int n = 1;
    for (;;) {                                       // the first free id and name
        const std::wstring id = L"c" + std::to_wstring(n);
        if (!FindCustom(id)) { copy.id = id; break; }
        ++n;
    }
    for (int k = 1;; ++k) {
        std::wstring name = Tr(L"My layout", L"Моя раскладка");
        if (k > 1) name += L" " + std::to_wstring(k);
        bool taken = false;
        for (const UiLayoutDef& d : s_custom) if (d.name == name) taken = true;
        if (!taken) { copy.name = name; break; }
    }
    s_custom.push_back(std::move(copy));
    s_activeId = s_custom.back().id;
    Log("layouts: %ls made from built-in %d", s_activeId.c_str(), g_prefs.tabLayout);
    SettingsRebuild();
    return s_custom.back();
}

void UiChanged(UiChange what) {
    SaveSoon();
    if (what == kUiColors) ThemeReapplyUiColors();
    if (what == kUiFonts) RebuildResources();
    RefreshLayout();
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

void UiRename(const std::wstring& name) {
    UiLayoutDef* d = s_activeId.empty() ? nullptr : FindCustom(s_activeId);
    const std::wstring n = TrimWs(name);
    if (!d || n.empty()) return;
    d->name = n.substr(0, 60);
    SaveSoon();
    Notify(true);
}

void UiDelete(const std::wstring& id) {
    UiLayoutDef* d = FindCustom(id);
    if (!d) return;
    const int mode = d->mode;
    const bool active = s_activeId == id;
    s_custom.erase(s_custom.begin() + (d - s_custom.data()));
    if (active) {                                    // back to the built-in layout it came from
        s_activeId.clear();
        g_prefs.tabLayout = mode;
        SaveSettings();
        ApplyAll();
    }
    SaveSoon();
    Notify(true);
}

// ---- colours -------------------------------------------------------------------------------------
static Color WithAlpha(const Color& c, BYTE a) { return Color(a, c.GetR(), c.GetG(), c.GetB()); }
static Color MixC(const Color& a, const Color& b, float t) {
    auto m = [t](BYTE x, BYTE y) { return (BYTE)std::lround(x + ((int)y - (int)x) * t); };
    return Color(255, m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}

bool UiFrameColorSet() { return UiActive().style.colors[kColFrame].GetA() != 0; }

void UiApplyColors(Theme& t) {
    const Color* c = UiActive().style.colors;
    if (c[kColFrame].GetA()) { t.frame = t.toolbar = t.tabbar = c[kColFrame]; t.sep = MixC(c[kColFrame], t.text, 0.12f); }
    if (c[kColField].GetA()) {
        t.field = t.pill = t.pillFocus = c[kColField];
        t.pillHover = MixC(c[kColField], t.text, 0.06f);
    }
    if (c[kColFieldBorder].GetA()) t.fieldBorder = t.tabBorder = c[kColFieldBorder];
    if (c[kColTabActive].GetA()) t.tabActive = c[kColTabActive];
    if (c[kColText].GetA()) t.text = c[kColText];
    if (c[kColText2].GetA()) t.text2 = c[kColText2];
    if (c[kColIcon].GetA()) { t.icon = c[kColIcon]; t.iconDis = WithAlpha(c[kColIcon], 110); }
    if (c[kColAccent].GetA()) t.accent = c[kColAccent];
    if (c[kColHover].GetA()) { t.hover = WithAlpha(c[kColHover], 60); t.pressed = WithAlpha(c[kColHover], 100); }
}
