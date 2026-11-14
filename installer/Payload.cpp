// The files carried inside LyraSetup.exe: an RCDATA resource "PAYLOAD" added by
// tools/make-installer.py. Layout (little endian):
//   "LYRAPAY1"  u32 file count  u16 len + version (UTF-8)
//   per file:   u16 len + path (UTF-8)  u8 method (0 stored, 1 LZMS)  u64 size  u64 packed size
//   then the packed data of every file, in the same order.
// LZMS is the Windows Compression API (cabinet.dll), so nothing extra is needed to unpack.
#include "Setup.h"
#include <compressapi.h>
#include <cstring>

namespace {
struct Reader {
    const uint8_t* p; const uint8_t* end; bool ok = true;
    template <class T> T get() {
        T v{};
        if ((size_t)(end - p) < sizeof(T)) { ok = false; return v; }
        memcpy(&v, p, sizeof(T)); p += sizeof(T); return v;
    }
    std::wstring str() {
        const uint16_t n = get<uint16_t>();
        if (!ok || (size_t)(end - p) < n) { ok = false; return {}; }
        std::wstring w(MultiByteToWideChar(CP_UTF8, 0, (const char*)p, n, nullptr, 0), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)p, n, w.data(), (int)w.size());
        p += n; return w;
    }
};

bool MakeDirs(const std::wstring& dir) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const size_t cut = dir.find_last_of(L'\\');
    if (cut != std::wstring::npos && cut > 2) MakeDirs(dir.substr(0, cut));
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::wstring SysError(DWORD e) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = msg ? msg : L"";
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}
}  // namespace

bool PayloadOpen(Payload& out) {
    HRSRC res = FindResourceW(nullptr, L"PAYLOAD", (LPCWSTR)RT_RCDATA);
    if (!res) return false;
    const uint8_t* base = (const uint8_t*)LockResource(LoadResource(nullptr, res));
    const DWORD size = SizeofResource(nullptr, res);
    if (!base || size < 16 || memcmp(base, "LYRAPAY1", 8) != 0) return false;
    Reader r{base + 8, base + size};
    const uint32_t count = r.get<uint32_t>();
    out.version = r.str();
    uint64_t offset = 0;
    for (uint32_t i = 0; i < count && r.ok; i++) {
        PayloadFile f;
        f.path = r.str();
        f.method = r.get<uint8_t>();
        f.size = r.get<uint64_t>();
        f.csize = r.get<uint64_t>();
        f.offset = offset;
        offset += f.csize;
        out.totalSize += f.size;
        // No absolute paths or ".." in a file name: everything stays inside the install folder.
        if (f.path.empty() || f.path.find(L"..") != std::wstring::npos || f.path.find(L':') != std::wstring::npos ||
            f.path[0] == L'\\' || f.path[0] == L'/') r.ok = false;
        out.files.push_back(std::move(f));
    }
    if (!r.ok || (uint64_t)(r.end - r.p) < offset) return false;
    out.data = r.p;
    out.dataSize = offset;
    return true;
}

bool PayloadExtract(const Payload& p, const PayloadFile& f, const std::wstring& dest, std::wstring& err) {
    const size_t cut = dest.find_last_of(L'\\');
    if (cut != std::wstring::npos && !MakeDirs(dest.substr(0, cut))) {
        err = SysError(GetLastError()) + L" (" + dest.substr(0, cut) + L")";
        return false;
    }
    std::vector<uint8_t> buf;
    const uint8_t* bytes = p.data + f.offset;
    if (f.method == 1) {
        static thread_local DECOMPRESSOR_HANDLE dec = nullptr;
        if (!dec && !CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &dec)) {
            err = L"LZMS: " + SysError(GetLastError());
            return false;
        }
        buf.resize((size_t)f.size);
        SIZE_T got = 0;
        if (!Decompress(dec, bytes, (SIZE_T)f.csize, buf.data(), buf.size(), &got) || got != f.size) {
            err = Tr(L"The installer is damaged (", L"Установщик повреждён (") + f.path + L")";
            return false;
        }
        bytes = buf.data();
    }
    HANDLE h = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION) {
        // A file still in use (an old helper process): move it aside, it is removed later.
        const std::wstring aside = dest + L"." + std::to_wstring(GetTickCount64()) + L".old";
        if (MoveFileExW(dest.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING))
            h = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (h == INVALID_HANDLE_VALUE) {
        err = SysError(GetLastError()) + L" (" + dest + L")";
        return false;
    }
    uint64_t left = f.size;
    bool ok = true;
    while (left && ok) {
        const DWORD chunk = (DWORD)(left > (1u << 24) ? (1u << 24) : left);
        DWORD w = 0;
        ok = WriteFile(h, bytes, chunk, &w, nullptr) && w == chunk;
        bytes += chunk; left -= chunk;
    }
    if (!ok) err = SysError(GetLastError()) + L" (" + dest + L")";
    CloseHandle(h);
    return ok;
}
