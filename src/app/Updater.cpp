// Lyra - automatic updates (see Updater.h).
#include "app/Updater.h"
#include "app/Http.h"
#include "app/Log.h"
#include "app/UpdateKey.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "ui/MainWindow.h"
#include "ui/settings/SettingsWindow.h"
#include "LyraVersion.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace {
const wchar_t kManifestUrl[] = L"https://aftercolour.github.io/lyra-browser/update.json";
constexpr long long kCheckEvery = 20LL * 3600;          // seconds between automatic checks

enum class St { Idle, Checking, UpToDate, Downloading, Ready, Failed };
std::mutex s_mu;
St s_state = St::Idle;
std::wstring s_version, s_error, s_setup;               // under s_mu
std::atomic<int> s_percent{0};
std::atomic<bool> s_running{false}, s_restarting{false};

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring s = buf;
    return s.substr(0, s.find_last_of(L'\\'));
}
std::wstring UpdateDir() { return g_settingsDir + L"\\Update"; }
std::wstring RestartMarker() { return g_settingsDir + L"\\update-restart"; }
bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

void Set(St st, const std::wstring& version = L"", const std::wstring& error = L"") {
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_state = st;
        if (!version.empty()) s_version = version;
        s_error = error;
    }
    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_UPDATE, 0, 0);
}

long long NowSec() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (long long)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL);
}
long long LastCheck() {
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"Update", L"LastCheck", L"0", buf, 32, (UpdateDir() + L"\\state.ini").c_str());
    return _wtoi64(buf);
}
void SaveLastCheck() {
    CreateDirectoryW(UpdateDir().c_str(), nullptr);
    WritePrivateProfileStringW(L"Update", L"LastCheck", std::to_wstring(NowSec()).c_str(), (UpdateDir() + L"\\state.ini").c_str());
}

std::string Utf8(const std::wstring& w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), (int)s.size(), nullptr, nullptr);
    return s;
}
std::wstring Wide(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), (int)w.size());
    return w;
}

// ---- tiny JSON reading: the manifest is flat {"key": "string" | number, ...} --------------
std::string JsonStr(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return {};
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = j.find('"', p);
    if (p == std::string::npos) return {};
    std::string out;
    for (++p; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] == '\\' && p + 1 < j.size()) { ++p; out += j[p] == 'n' ? '\n' : j[p]; }
        else out += j[p];
    }
    return out;
}
long long JsonNum(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return -1;
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return -1;
    while (++p < j.size() && (j[p] == ' ' || j[p] == '\t')) {}
    return _atoi64(j.c_str() + p);
}

// 1 if a > b, -1 if a < b, 0 if equal ("0.1.10" > "0.1.9")
int CompareVersions(const std::wstring& a, const std::wstring& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long va = 0, vb = 0;
        while (i < a.size() && iswdigit(a[i])) va = va * 10 + (a[i++] - L'0');
        while (j < b.size() && iswdigit(b[j])) vb = vb * 10 + (b[j++] - L'0');
        if (va != vb) return va > vb ? 1 : -1;
        while (i < a.size() && !iswdigit(a[i])) i++;
        while (j < b.size() && !iswdigit(b[j])) j++;
    }
    return 0;
}

// ---- hashing and the signature --------------------------------------------------------
std::string Hex(const unsigned char* d, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += h[d[i] >> 4]; s += h[d[i] & 15]; }
    return s;
}

bool Sha256File(const std::wstring& path, std::string& hex, unsigned long long& size) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<unsigned char> buf(1 << 20);
    size = 0;
    DWORD got = 0;
    while (ok && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
        ok = BCryptHashData(h, buf.data(), got, 0) == 0;
        size += got;
    }
    unsigned char digest[32];
    ok = ok && BCryptFinishHash(h, digest, 32, 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    if (ok) hex = Hex(digest, 32);
    return ok;
}

bool SignatureOk(const std::string& message, const std::string& sigB64) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(sigB64.c_str(), 0, CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr) || n > 256) return false;
    std::vector<BYTE> sig(n);
    CryptStringToBinaryA(sigB64.c_str(), 0, CRYPT_STRING_BASE64, sig.data(), &n, nullptr, nullptr);
    unsigned char digest[32];
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)message.data(), (ULONG)message.size(), digest, 32) == 0 &&
              BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)kUpdatePublicKey, sizeof(kUpdatePublicKey), 0) == 0 &&
              BCryptVerifySignature(key, nullptr, digest, 32, sig.data(), n, 0) == 0;
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ---- the check ------------------------------------------------------------------------------
void Work(bool manual) {
    std::wstring err;
    std::string json;
    wchar_t env[2048];
    const std::wstring manifestUrl = GetEnvironmentVariableW(L"LYRA_UPDATE_URL", env, 2048) ? env : kManifestUrl;
    if (manual) Set(St::Checking);
    if (!HttpFetch(manifestUrl, &json, L"", 0, nullptr, err)) {
        Log("update: manifest %ls: %ls", manifestUrl.c_str(), err.c_str());
        Set(manual ? St::Failed : St::Idle, L"", err);
        return;
    }
    SaveLastCheck();
    const std::wstring version = Wide(JsonStr(json, "version"));
    const std::string url = JsonStr(json, "url"), sha = JsonStr(json, "sha256"), sig = JsonStr(json, "sig");
    const long long size = JsonNum(json, "size");
    if (version.empty() || url.empty() || sha.size() != 64 || size <= 0) {
        Set(manual ? St::Failed : St::Idle, L"", Tr(L"the update information is damaged", L"сведения об обновлении повреждены"));
        return;
    }
    if (CompareVersions(version, LYRA_VERSION_WSTR) <= 0) {
        Log("update: %ls is the newest (manifest %ls)", LYRA_VERSION_WSTR, version.c_str());
        Set(manual ? St::UpToDate : St::Idle);
        return;
    }
    // Only an update signed with Lyra's key: a changed manifest (or a hacked site) gets nowhere.
    const std::string message = "lyra-update/1\n" + Utf8(version) + "\n" + url + "\n" + std::to_string(size) + "\n" + sha;
    if (!SignatureOk(message, sig)) {
        Log("update: manifest for %ls has a bad signature - ignored", version.c_str());
        Set(manual ? St::Failed : St::Idle, L"", Tr(L"the update is not signed by Lyra", L"обновление не подписано Lyra"));
        return;
    }
    CreateDirectoryW(UpdateDir().c_str(), nullptr);
    const std::wstring setup = UpdateDir() + L"\\LyraSetup-" + version + L".exe";
    std::string have;
    unsigned long long haveSize = 0;
    if (!(Sha256File(setup, have, haveSize) && have == sha && haveSize == (unsigned long long)size)) {
        Log("update: downloading %ls", version.c_str());
        s_percent = 0;
        Set(St::Downloading, version);
        const std::wstring part = setup + L".part";
        if (!HttpFetch(Wide(url), nullptr, part, (unsigned long long)size, &s_percent, err)) {
            DeleteFileW(part.c_str());
            Set(manual ? St::Failed : St::Idle, L"", err);
            return;
        }
        if (!(Sha256File(part, have, haveSize) && have == sha && haveSize == (unsigned long long)size)) {
            DeleteFileW(part.c_str());
            Log("update: %ls downloaded but does not match the manifest", version.c_str());
            Set(manual ? St::Failed : St::Idle, L"", Tr(L"the download is damaged", L"загрузка повреждена"));
            return;
        }
        MoveFileExW(part.c_str(), setup.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    // Older downloads are not needed any more.
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((UpdateDir() + L"\\LyraSetup-*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do if (UpdateDir() + L"\\" + fd.cFileName != setup) DeleteFileW((UpdateDir() + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    {
        std::lock_guard<std::mutex> lock(s_mu);
        s_setup = setup;
    }
    Log("update: %ls is ready", version.c_str());
    Set(St::Ready, version);
}

void Start(bool manual) {
    if (!UpdaterSupported() || s_running.exchange(true)) return;
    std::thread([manual] {
        Work(manual);
        s_running = false;
    }).detach();
}

// The installer, run quietly: it waits for Lyra to close, installs, and (restart) starts it again.
bool RunSetup(bool restart) {
    std::wstring setup;
    {
        std::lock_guard<std::mutex> lock(s_mu);
        setup = s_setup;
    }
    if (setup.empty() || !FileExists(setup)) return false;
    const std::wstring dir = ExeDir();
    std::wstring cmd = L"\"" + setup + L"\" /update \"/dir=" + dir + L"\"";
    if (!restart) cmd += L" /norestart";
    // Not the copy Windows knows as installed (a test install): only replace the files.
    wchar_t loc[MAX_PATH * 2] = {};
    DWORD n = sizeof(loc);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Lyra", L"InstallLocation",
                     RRF_RT_REG_SZ, nullptr, loc, &n) != ERROR_SUCCESS || _wcsicmp(loc, dir.c_str()) != 0)
        cmd += L" /noregister";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_BREAKAWAY_FROM_JOB, nullptr, UpdateDir().c_str(), &si, &pi) &&
        !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, UpdateDir().c_str(), &si, &pi)) {
        Log("update: cannot start %ls (%lu)", setup.c_str(), GetLastError());
        return false;
    }
    Log("update: started %ls", cmd.c_str());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
}  // namespace

bool UpdaterSupported() {
    static const bool installed = FileExists(ExeDir() + L"\\install.files");
    return installed;
}

void UpdaterInit() {
    UpdaterJustRestarted();                                 // uses up the restart mark in any case
    if (!UpdaterSupported() || !g_hWnd) return;
    SetTimer(g_hWnd, TIMER_UPDATE, 20 * 1000, nullptr);     // the first look soon after start
}

void UpdaterTimer() {
    SetTimer(g_hWnd, TIMER_UPDATE, 3600 * 1000, nullptr);   // then every hour, checking once a day
    if (g_prefs.autoUpdate && !UpdaterReady() && NowSec() - LastCheck() >= kCheckEvery) Start(false);
}

void UpdaterCheckNow() { Start(true); }

void UpdaterChanged() {
    SettingsRefresh();
    if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
}

bool UpdaterReady() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_state == St::Ready;
}

std::wstring UpdaterReadyVersion() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_version;
}

bool UpdaterBusy() { return s_running; }

std::wstring UpdaterStatus() {
    if (!UpdaterSupported())
        return Tr(L"Updates come with the installer (this is a development build)", L"Обновления приходят через установщик (это сборка для разработки)");
    std::lock_guard<std::mutex> lock(s_mu);
    switch (s_state) {
    case St::Checking: return Tr(L"Checking for updates\x2026", L"Проверка обновлений\x2026");
    case St::UpToDate: return Tr(L"Lyra is up to date", L"Установлена последняя версия");
    case St::Downloading:
        return Tr(L"Downloading version ", L"Загрузка версии ") + s_version + L"\x2026 " + std::to_wstring(s_percent.load()) + L"%";
    case St::Ready:
        return Tr(L"Version ", L"Версия ") + s_version + Tr(L" is ready - restart Lyra to update", L" готова - перезапустите Lyra, чтобы обновиться");
    case St::Failed: return Tr(L"Could not update: ", L"Не удалось обновиться: ") + s_error;
    default:
        return g_prefs.autoUpdate ? Tr(L"Lyra looks for updates once a day", L"Lyra проверяет обновления раз в день")
                                  : Tr(L"Automatic updates are off", L"Автоматические обновления выключены");
    }
}

void UpdaterRestartAndInstall() {
    if (!UpdaterReady() || s_restarting.exchange(true)) return;
    // The next start restores every tab, as if Lyra had never closed.
    HANDLE m = CreateFileW(RestartMarker().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (m != INVALID_HANDLE_VALUE) CloseHandle(m);
    if (!RunSetup(true)) {
        DeleteFileW(RestartMarker().c_str());
        s_restarting = false;
        return;
    }
    PostMessageW(g_hWnd, WM_CLOSE, 0, 0);
}

void UpdaterOnExit() {
    if (s_restarting || !UpdaterReady()) return;
    RunSetup(false);
}

bool UpdaterJustRestarted() {
    static const bool marked = [] {
        const bool m = FileExists(RestartMarker());
        if (m) DeleteFileW(RestartMarker().c_str());
        return m;
    }();
    return marked;
}
