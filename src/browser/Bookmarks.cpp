// Lyra — закладки: «Избранное» (стартовая страница), папка «Закладки», импорт из других браузеров.
#include "browser/Bookmarks.h"
#include "app/Log.h"
#include "browser/History.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"

#include <map>

void (*g_bookmarksChanged)() = nullptr;
long long g_bookmarksVersion = 0;

static std::vector<Bookmark> s_items;         // tree order does not matter; order inside a folder does
static long long s_nextId = 3;
static bool s_loaded = false;

static std::wstring BookmarksPath() { return g_settingsDir + L"\\bookmarks.tsv"; }

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

// "https://www.example.com/a/" -> "example.com/a": two addresses of the same page compare equal.
static std::wstring Bare(const std::wstring& url) {
    std::wstring s = url;
    size_t p = s.find(L"://");
    if (p != std::wstring::npos) s = s.substr(p + 3);
    if (s.compare(0, 4, L"www.") == 0) s = s.substr(4);
    while (!s.empty() && s.back() == L'/') s.pop_back();
    return Lower(s);
}

static std::wstring Clean(std::wstring s) {
    for (auto& ch : s) if (ch == L'\t' || ch == L'\r' || ch == L'\n') ch = L' ';
    return TrimWs(s);
}

static int IndexOf(long long id) {
    for (int i = 0; i < (int)s_items.size(); ++i) if (s_items[i].id == id) return i;
    return -1;
}

static void Save() {
    std::string out = "LyraBookmarks\t1\n";
    for (const Bookmark& b : s_items) {
        // id \t parent \t F|U \t added \t url \t title
        out += std::to_string(b.id) + "\t" + std::to_string(b.parent) + "\t" + (b.folder ? "F" : "U") + "\t" +
               std::to_string(b.added) + "\t" + ToUtf8(Clean(b.url)) + "\t" + ToUtf8(Clean(b.title)) + "\n";
    }
    const std::wstring path = BookmarksPath(), tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Log("bookmarks: cannot write %ls", tmp.c_str()); return; }
    DWORD written = 0;
    const BOOL ok = WriteFile(f, out.data(), (DWORD)out.size(), &written, nullptr);
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

static void Changed() {
    ++g_bookmarksVersion;
    Save();
    if (g_bookmarksChanged) g_bookmarksChanged();
}

static void EnsureRoots() {
    for (long long id : { kFavoritesFolder, kBookmarksFolder }) {
        int i = IndexOf(id);
        if (i < 0) {
            Bookmark b;
            b.id = id;
            b.folder = true;
            b.added = NowUnixMs();
            s_items.insert(s_items.begin() + std::min<size_t>((size_t)(id - 1), s_items.size()), b);
        } else {
            s_items[i].parent = 0;
            s_items[i].folder = true;
        }
    }
}

static void AddDefaults() {
    struct Site { const wchar_t* title; const wchar_t* url; };
    static const Site en[] = {
        { L"YouTube", L"https://www.youtube.com/" }, { L"Wikipedia", L"https://en.wikipedia.org/" },
        { L"GitHub", L"https://github.com/" }, { L"Gmail", L"https://mail.google.com/" },
        { L"Maps", L"https://www.google.com/maps" }, { L"Reddit", L"https://www.reddit.com/" },
    };
    static const Site ru[] = {
        { L"YouTube", L"https://www.youtube.com/" }, { L"Википедия", L"https://ru.wikipedia.org/" },
        { L"GitHub", L"https://github.com/" }, { L"Хабр", L"https://habr.com/" },
        { L"Почта", L"https://mail.google.com/" }, { L"Карты", L"https://www.google.com/maps" },
    };
    const Site* list = UiRussian() ? ru : en;
    for (int i = 0; i < 6; ++i) {
        Bookmark b;
        b.id = s_nextId++;
        b.parent = kFavoritesFolder;
        b.title = list[i].title;
        b.url = list[i].url;
        b.added = NowUnixMs();
        s_items.push_back(b);
    }
}

void BookmarksLoad() {
    if (s_loaded) return;
    s_loaded = true;
    HANDLE f = CreateFileW(BookmarksPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        EnsureRoots();
        AddDefaults();                   // first run: the same sites the start page showed before
        Save();
        return;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    std::string data((size_t)std::min<long long>(size.QuadPart, 32LL << 20), '\0');
    DWORD read = 0;
    if (!data.empty()) ReadFile(f, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(f);
    data.resize(read);
    size_t pos = 0;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> col;
        for (size_t p = 0;;) {
            size_t t = line.find('\t', p);
            col.push_back(line.substr(p, t == std::string::npos ? std::string::npos : t - p));
            if (t == std::string::npos) break;
            p = t + 1;
        }
        if (col.size() < 6 || col[0] == "LyraBookmarks") continue;
        Bookmark b;
        b.id = _atoi64(col[0].c_str());
        b.parent = _atoi64(col[1].c_str());
        b.folder = col[2] == "F";
        b.added = _atoi64(col[3].c_str());
        b.url = ToWide(col[4]);
        b.title = ToWide(col[5]);
        if (b.id <= 0 || IndexOf(b.id) >= 0) continue;
        if (!b.folder && b.url.empty()) continue;
        s_items.push_back(b);
        s_nextId = std::max(s_nextId, b.id + 1);
    }
    EnsureRoots();
    // Items whose folder is gone go to Bookmarks.
    for (Bookmark& b : s_items) {
        if (b.id == kFavoritesFolder || b.id == kBookmarksFolder) continue;
        const int p = IndexOf(b.parent);
        if (p < 0 || !s_items[p].folder || b.parent == b.id) b.parent = kBookmarksFolder;
    }
    Log("bookmarks: %d items", (int)s_items.size());
}

const Bookmark* BookmarkGet(long long id) {
    BookmarksLoad();
    const int i = IndexOf(id);
    return i >= 0 ? &s_items[i] : nullptr;
}

long long BookmarkFind(const std::wstring& url) {
    BookmarksLoad();
    if (url.empty()) return 0;
    const std::wstring bare = Bare(url);
    for (const Bookmark& b : s_items)
        if (!b.folder && (b.url == url || Bare(b.url) == bare)) return b.id;
    return 0;
}

std::vector<Bookmark> BookmarkChildren(long long folder) {
    BookmarksLoad();
    std::vector<Bookmark> v;
    for (const Bookmark& b : s_items) if (b.parent == folder && b.id != folder) v.push_back(b);
    return v;
}

std::vector<Bookmark> BookmarksAll() {
    BookmarksLoad();
    std::vector<Bookmark> out;
    std::function<void(long long)> walk = [&](long long folder) {
        for (const Bookmark& b : s_items) {
            if (b.parent != folder || b.id == folder) continue;
            out.push_back(b);
            if (b.folder) walk(b.id);
        }
    };
    for (long long root : { kFavoritesFolder, kBookmarksFolder }) {
        out.push_back(*BookmarkGet(root));
        walk(root);
    }
    return out;
}

std::wstring BookmarkFolderName(long long id) {
    if (id == kFavoritesFolder) return Tr(L"Favorites", L"Избранное");
    if (id == kBookmarksFolder) return Tr(L"Bookmarks", L"Закладки");
    const Bookmark* b = BookmarkGet(id);
    return b ? b->title : std::wstring();
}

// Position in s_items for "before the `index`-th child of `folder`" (-1 = after the last).
static size_t InsertPos(long long folder, int index) {
    int seen = 0;
    size_t last = s_items.size();
    for (size_t i = 0; i < s_items.size(); ++i) {
        if (s_items[i].parent != folder || s_items[i].id == folder) continue;
        if (index >= 0 && seen == index) return i;
        ++seen;
        last = i + 1;
    }
    return last;
}

static bool IsFolder(long long id) {
    const int i = IndexOf(id);
    return i >= 0 && s_items[i].folder;
}

long long BookmarkAdd(long long folder, const std::wstring& title, const std::wstring& url, int index) {
    BookmarksLoad();
    if (url.empty()) return 0;
    if (!IsFolder(folder)) folder = kBookmarksFolder;
    Bookmark b;
    b.id = s_nextId++;
    b.parent = folder;
    b.url = Clean(url);
    b.title = Clean(title);
    if (b.title.empty()) b.title = HostOf(b.url);
    b.added = NowUnixMs();
    s_items.insert(s_items.begin() + InsertPos(folder, index), b);
    Changed();
    return b.id;
}

long long BookmarkAddFolder(long long parent, const std::wstring& title) {
    BookmarksLoad();
    if (!IsFolder(parent)) parent = kBookmarksFolder;
    Bookmark b;
    b.id = s_nextId++;
    b.parent = parent;
    b.folder = true;
    b.title = Clean(title);
    if (b.title.empty()) b.title = Tr(L"New Folder", L"Новая папка");
    b.added = NowUnixMs();
    s_items.insert(s_items.begin() + InsertPos(parent, -1), b);
    Changed();
    return b.id;
}

static void CollectSubtree(long long id, std::vector<long long>& out) {
    out.push_back(id);
    for (const Bookmark& b : s_items)
        if (b.parent == id && b.id != id) CollectSubtree(b.id, out);
}

void BookmarkRemove(long long id) {
    BookmarksLoad();
    if (id == kFavoritesFolder || id == kBookmarksFolder || IndexOf(id) < 0) return;
    std::vector<long long> gone;
    CollectSubtree(id, gone);
    s_items.erase(std::remove_if(s_items.begin(), s_items.end(), [&](const Bookmark& b) {
        return std::find(gone.begin(), gone.end(), b.id) != gone.end();
    }), s_items.end());
    Changed();
}

void BookmarkRename(long long id, const std::wstring& title) {
    BookmarksLoad();
    const int i = IndexOf(id);
    if (i < 0 || id == kFavoritesFolder || id == kBookmarksFolder) return;
    std::wstring t = Clean(title);
    if (t.empty()) t = s_items[i].folder ? std::wstring(Tr(L"New Folder", L"Новая папка")) : HostOf(s_items[i].url);
    if (t == s_items[i].title) return;
    s_items[i].title = t;
    Changed();
}

void BookmarkSetUrl(long long id, const std::wstring& url) {
    BookmarksLoad();
    const int i = IndexOf(id);
    const std::wstring u = Clean(url);
    if (i < 0 || s_items[i].folder || u.empty() || u == s_items[i].url) return;
    s_items[i].url = u;
    Changed();
}

void BookmarkMove(long long id, long long folder, int index) {
    BookmarksLoad();
    int i = IndexOf(id);
    if (i < 0 || id == kFavoritesFolder || id == kBookmarksFolder || !IsFolder(folder)) return;
    for (long long p = folder; p; ) {                 // not into itself or one of its folders
        if (p == id) return;
        const Bookmark* b = BookmarkGet(p);
        p = b ? b->parent : 0;
    }
    Bookmark b = s_items[i];
    // `index` counts the children as the page shows them, which still include this item.
    if (b.parent == folder && index >= 0) {
        int own = 0;
        for (int k = 0; k < i; ++k) if (s_items[k].parent == folder && s_items[k].id != folder) ++own;
        if (own < index) --index;
    }
    s_items.erase(s_items.begin() + i);
    b.parent = folder;
    s_items.insert(s_items.begin() + InsertPos(folder, index), b);
    Changed();
}

void BookmarkReplaceFavorites(const std::vector<std::pair<std::wstring, std::wstring>>& titleUrl) {
    BookmarksLoad();
    s_items.erase(std::remove_if(s_items.begin(), s_items.end(), [](const Bookmark& b) {
        return b.parent == kFavoritesFolder && !b.folder;
    }), s_items.end());
    for (auto& [title, url] : titleUrl) {
        if (url.empty()) continue;
        Bookmark b;
        b.id = s_nextId++;
        b.parent = kFavoritesFolder;
        b.url = Clean(url);
        b.title = Clean(title);
        if (b.title.empty()) b.title = HostOf(b.url);
        b.added = NowUnixMs();
        s_items.insert(s_items.begin() + InsertPos(kFavoritesFolder, -1), b);
    }
    Changed();
}

std::vector<Bookmark> BookmarksMatch(const std::wstring& text, size_t max) {
    BookmarksLoad();
    const std::wstring q = Lower(TrimWs(text));
    if (q.empty()) return {};
    std::vector<std::wstring> words;
    for (size_t p = 0; p < q.size();) {
        size_t e = q.find(L' ', p);
        if (e == std::wstring::npos) e = q.size();
        if (e > p) words.push_back(q.substr(p, e - p));
        p = e + 1;
    }
    std::vector<std::pair<int, const Bookmark*>> scored;
    for (const Bookmark& b : s_items) {
        if (b.folder) continue;
        const std::wstring bare = Bare(b.url), title = Lower(b.title);
        int score;
        if (bare.compare(0, q.size(), q) == 0) score = 3;                   // start of the address
        else if (title.compare(0, q.size(), q) == 0) score = 2;             // start of the name
        else {
            bool all = true;
            for (auto& w : words)
                if (bare.find(w) == std::wstring::npos && title.find(w) == std::wstring::npos) { all = false; break; }
            if (!all) continue;
            score = 1;
        }
        scored.emplace_back(score, &b);
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Bookmark> out;
    for (auto& s : scored) {
        if (out.size() >= max) break;
        out.push_back(*s.second);
    }
    return out;
}

// ----------------------------------------------------------------------------
//  Import from Chromium-based browsers (their "Bookmarks" file is JSON)
// ----------------------------------------------------------------------------
namespace {

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    std::wstring str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::wstring, JVal>> obj;
    const JVal* get(const wchar_t* key) const {
        for (auto& [k, v] : obj) if (k == key) return &v;
        return nullptr;
    }
    std::wstring text(const wchar_t* key) const {
        const JVal* v = get(key);
        return v && v->type == Str ? v->str : std::wstring();
    }
};

struct JParser {
    const std::string& s;
    size_t p = 0;
    int depth = 0;
    explicit JParser(const std::string& src) : s(src) {}

    void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p; }

    bool hex4(unsigned& out) {
        if (p + 4 > s.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = s[p++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') out |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::wstring& out) {
        if (p >= s.size() || s[p] != '"') return false;
        ++p;
        std::string raw;            // UTF-8 bytes, \u escapes appended as UTF-8
        auto putUtf8 = [&raw](unsigned cp) {
            if (cp < 0x80) raw += (char)cp;
            else if (cp < 0x800) { raw += (char)(0xC0 | (cp >> 6)); raw += (char)(0x80 | (cp & 0x3F)); }
            else if (cp < 0x10000) { raw += (char)(0xE0 | (cp >> 12)); raw += (char)(0x80 | ((cp >> 6) & 0x3F)); raw += (char)(0x80 | (cp & 0x3F)); }
            else { raw += (char)(0xF0 | (cp >> 18)); raw += (char)(0x80 | ((cp >> 12) & 0x3F)); raw += (char)(0x80 | ((cp >> 6) & 0x3F)); raw += (char)(0x80 | (cp & 0x3F)); }
        };
        while (p < s.size() && s[p] != '"') {
            char c = s[p++];
            if (c != '\\') { raw += c; continue; }
            if (p >= s.size()) return false;
            c = s[p++];
            switch (c) {
            case 'n': raw += '\n'; break;
            case 't': raw += '\t'; break;
            case 'r': raw += '\r'; break;
            case 'b': raw += '\b'; break;
            case 'f': raw += '\f'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                putUtf8(cp);
                break;
            }
            default: raw += c; break;       // \" \\ \/
            }
        }
        if (p >= s.size()) return false;
        ++p;
        out = ToWide(raw);
        return true;
    }

    bool value(JVal& v) {
        if (++depth > 200) return false;
        ws();
        if (p >= s.size()) return false;
        const char c = s[p];
        bool ok = true;
        if (c == '{') {
            v.type = JVal::Obj;
            ++p;
            ws();
            if (p < s.size() && s[p] == '}') ++p;
            else for (;;) {
                ws();
                std::wstring key;
                if (!string(key)) { ok = false; break; }
                ws();
                if (p >= s.size() || s[p] != ':') { ok = false; break; }
                ++p;
                v.obj.emplace_back(key, JVal());
                if (!value(v.obj.back().second)) { ok = false; break; }
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == '}') { ++p; break; }
                ok = false;
                break;
            }
        } else if (c == '[') {
            v.type = JVal::Arr;
            ++p;
            ws();
            if (p < s.size() && s[p] == ']') ++p;
            else for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back())) { ok = false; break; }
                ws();
                if (p < s.size() && s[p] == ',') { ++p; continue; }
                if (p < s.size() && s[p] == ']') { ++p; break; }
                ok = false;
                break;
            }
        } else if (c == '"') {
            v.type = JVal::Str;
            ok = string(v.str);
        } else {
            // number, true, false, null: kept only as text
            const size_t start = p;
            while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']' && s[p] != ' ' && s[p] != '\n' && s[p] != '\r' && s[p] != '\t') ++p;
            const std::string word = s.substr(start, p - start);
            if (word.empty()) ok = false;
            v.type = word == "null" ? JVal::Null : (word == "true" || word == "false") ? JVal::Bool : JVal::Num;
            v.str = ToWide(word);
        }
        --depth;
        return ok;
    }
};

struct Importer { const wchar_t* key; const wchar_t* name; int base; const wchar_t* path; };
// base: 0 = %LOCALAPPDATA%, 1 = %APPDATA%
const Importer kImporters[] = {
    { L"chrome",  L"Google Chrome",  0, L"\\Google\\Chrome\\User Data\\Default\\Bookmarks" },
    { L"edge",    L"Microsoft Edge", 0, L"\\Microsoft\\Edge\\User Data\\Default\\Bookmarks" },
    { L"yandex",  L"Yandex Browser", 0, L"\\Yandex\\YandexBrowser\\User Data\\Default\\Bookmarks" },
    { L"brave",   L"Brave",          0, L"\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Bookmarks" },
    { L"opera",   L"Opera",          1, L"\\Opera Software\\Opera Stable\\Default\\Bookmarks" },
    { L"opera",   L"Opera",          1, L"\\Opera Software\\Opera Stable\\Bookmarks" },
    { L"vivaldi", L"Vivaldi",        0, L"\\Vivaldi\\User Data\\Default\\Bookmarks" },
};

std::wstring ImporterPath(const Importer& im) {
    wchar_t base[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(im.base == 0 ? L"LOCALAPPDATA" : L"APPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(base) + im.path;
}

bool Exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

}  // namespace

std::vector<std::pair<std::wstring, std::wstring>> BookmarkImporters() {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    for (const Importer& im : kImporters) {
        bool listed = false;
        for (auto& o : out) if (o.first == im.key) listed = true;
        if (!listed && Exists(ImporterPath(im))) out.emplace_back(im.key, im.name);
    }
    return out;
}

int BookmarkImport(const std::wstring& key) {
    BookmarksLoad();
    const Importer* found = nullptr;
    std::wstring path;
    for (const Importer& im : kImporters) {
        if (key != im.key) continue;
        path = ImporterPath(im);
        if (Exists(path)) { found = &im; break; }
    }
    if (!found) return -1;
    std::string data;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    data.resize((size_t)std::min<long long>(size.QuadPart, 64LL << 20));
    DWORD read = 0;
    if (!data.empty()) ReadFile(f, &data[0], (DWORD)data.size(), &read, nullptr);
    CloseHandle(f);
    data.resize(read);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF) data.erase(0, 3);   // UTF-8 BOM

    JVal root;
    JParser parser(data);
    if (!parser.value(root) || root.type != JVal::Obj) { Log("bookmarks: cannot parse %ls", path.c_str()); return -1; }
    const JVal* roots = root.get(L"roots");
    if (!roots || roots->type != JVal::Obj) return -1;

    // Everything goes into one new folder "Imported from X" under Bookmarks; addresses that are
    // already bookmarked are left out, so importing twice adds nothing.
    std::vector<std::wstring> known;
    for (const Bookmark& b : s_items) if (!b.folder) known.push_back(Bare(b.url));
    std::vector<Bookmark> added;
    long long nextId = s_nextId;
    Bookmark top;
    top.id = nextId++;
    top.parent = kBookmarksFolder;
    top.folder = true;
    top.title = std::wstring(Tr(L"Imported from ", L"Импорт из ")) + found->name;
    top.added = NowUnixMs();

    int count = 0;
    // Returns whether `node` (folder) ended up with anything in it.
    std::function<bool(const JVal&, long long)> walk = [&](const JVal& node, long long parent) -> bool {
        const JVal* children = node.get(L"children");
        if (!children || children->type != JVal::Arr) return false;
        bool any = false;
        for (const JVal& c : children->arr) {
            if (c.type != JVal::Obj) continue;
            const std::wstring type = c.text(L"type");
            if (type == L"url") {
                const std::wstring url = c.text(L"url");
                if (url.empty() || url.size() > 4096) continue;
                if (url.compare(0, 11, L"javascript:") == 0) continue;
                const std::wstring bare = Bare(url);
                if (std::find(known.begin(), known.end(), bare) != known.end()) continue;
                known.push_back(bare);
                Bookmark b;
                b.id = nextId++;
                b.parent = parent;
                b.url = Clean(url);
                b.title = Clean(c.text(L"name"));
                if (b.title.empty()) b.title = HostOf(b.url);
                b.added = NowUnixMs();
                added.push_back(b);
                ++count;
                any = true;
            } else if (type == L"folder") {
                Bookmark folder;
                folder.id = nextId++;
                folder.parent = parent;
                folder.folder = true;
                folder.title = Clean(c.text(L"name"));
                if (folder.title.empty()) folder.title = Tr(L"Folder", L"Папка");
                folder.added = NowUnixMs();
                const size_t at = added.size();
                added.push_back(folder);
                if (walk(c, folder.id)) any = true;
                else added.erase(added.begin() + at);            // empty: leave it out
            }
        }
        return any;
    };
    // The bookmarks bar becomes a folder of its own; "other" and "synced" go straight in.
    struct RootPart { const wchar_t* key; const wchar_t* en; const wchar_t* ru; };
    const RootPart parts[] = { { L"bookmark_bar", L"Bookmarks bar", L"Панель закладок" },
                               { L"other", nullptr, nullptr }, { L"synced", nullptr, nullptr } };
    for (const RootPart& part : parts) {
        const JVal* r = roots->get(part.key);
        if (!r || r->type != JVal::Obj) continue;
        if (part.en) {
            Bookmark folder;
            folder.id = nextId++;
            folder.parent = top.id;
            folder.folder = true;
            folder.title = Tr(part.en, part.ru);
            folder.added = NowUnixMs();
            const size_t at = added.size();
            added.push_back(folder);
            if (!walk(*r, folder.id)) added.erase(added.begin() + at);
        } else {
            walk(*r, top.id);
        }
    }
    if (count == 0) return 0;
    s_nextId = nextId;
    s_items.push_back(top);
    s_items.insert(s_items.end(), added.begin(), added.end());
    Log("bookmarks: imported %d from %ls", count, path.c_str());
    Changed();
    return count;
}
