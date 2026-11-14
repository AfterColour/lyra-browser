// Lyra - plain downloads (WinHTTP, the system proxy, redirects followed).
#pragma once
#include <atomic>
#include <string>

// GET `url` (https, or a local file path for tests) into `body` or into the file `out`.
// `expect` bytes, when known, drive `percent`. False with a short reason in `err`.
bool HttpFetch(const std::wstring& url, std::string* body, const std::wstring& out, unsigned long long expect,
               std::atomic<int>* percent, std::wstring& err);
