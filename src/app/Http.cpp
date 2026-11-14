// Lyra - plain downloads for the updater and the start page (see Http.h).
#include "app/Http.h"
#include "core/Localization.h"
#include "LyraVersion.h"
#include <winhttp.h>
#include <vector>

// Into `body`, or into the file `out` (then `expect` bytes drive the percentage).
bool HttpFetch(const std::wstring& url, std::string* body, const std::wstring& out, unsigned long long expect,
               std::atomic<int>* percent, std::wstring& err) {
    HANDLE file = INVALID_HANDLE_VALUE;
    if (!out.empty()) {
        file = CreateFileW(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) { err = L"cannot write " + out; return false; }
    }
    auto sink = [&](const void* data, DWORD n) -> bool {
        if (body) body->append((const char*)data, n);
        DWORD w = 0;
        return file == INVALID_HANDLE_VALUE || (WriteFile(file, data, n, &w, nullptr) && w == n);
    };
    unsigned long long total = 0;
    bool ok = false;
    if (url.compare(0, 7, L"http://") != 0 && url.compare(0, 8, L"https://") != 0) {
        HANDLE in = CreateFileW(url.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (in != INVALID_HANDLE_VALUE) {
            std::vector<char> buf(1 << 20);
            DWORD got = 0;
            ok = true;
            while (ok && ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
                ok = sink(buf.data(), got);
                total += got;
                if (expect && percent) *percent = (int)(total * 100 / expect);
            }
            CloseHandle(in);
        } else {
            err = L"not found: " + url;
        }
    } else {
        URL_COMPONENTS uc = { sizeof(uc) };
        wchar_t host[256] = {}, path[4096] = {}, extra[2048] = {};
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 4096;
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
        HINTERNET ses = nullptr, con = nullptr, req = nullptr;
        if (WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
            ses = WinHttpOpen(L"Lyra/" LYRA_VERSION_WSTR, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
            if (!ses) ses = WinHttpOpen(L"Lyra/" LYRA_VERSION_WSTR, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        }
        if (ses) {
            WinHttpSetTimeouts(ses, 15000, 15000, 30000, 60000);
            con = WinHttpConnect(ses, host, uc.nPort, 0);
        }
        if (con) req = WinHttpOpenRequest(con, L"GET", (std::wstring(path) + extra).c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        if (req && WinHttpSendRequest(req, L"Cache-Control: no-cache\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            DWORD status = 0, len = sizeof(status);
            WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
            if (status == 200) {
                ok = true;
                std::vector<char> buf(1 << 16);
                for (;;) {
                    DWORD avail = 0, got = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                    if (!avail) break;
                    if (!WinHttpReadData(req, buf.data(), std::min<DWORD>(avail, (DWORD)buf.size()), &got) || !got) { ok = !avail; break; }
                    if (!sink(buf.data(), got)) { ok = false; break; }
                    total += got;
                    if (expect && percent) *percent = (int)(total * 100 / expect);
                    if (body && body->size() > (1 << 20)) { ok = false; break; }   // a manifest is small
                }
                if (!ok) err = Tr(L"the connection broke off", L"соединение прервалось");
            } else {
                err = L"HTTP " + std::to_wstring(status);
            }
        } else {
            err = Tr(L"no connection", L"нет соединения");
        }
        if (req) WinHttpCloseHandle(req);
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

