// Lyra — переменные окружения LYRA_* и настройка GStreamer до запуска WebKit.
#include "app/Environment.h"
#include "app/Log.h"
#include "core/Settings.h"

// Debug switches are documented as LYRA_*; the WebKit side (GPU and web processes) still reads
// them as MYBROWSER_*. Copy every LYRA_X to MYBROWSER_X (unless that is set already) before
// WebKit starts its processes, which inherit this environment.
void ForwardLyraEnvironment() {
    LPWCH block = GetEnvironmentStringsW();
    if (!block) return;
    std::vector<std::pair<std::wstring, std::wstring>> forward;
    for (LPWCH entry = block; *entry; entry += wcslen(entry) + 1) {
        std::wstring line = entry;
        if (_wcsnicmp(line.c_str(), L"LYRA_", 5) != 0) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        forward.emplace_back(L"MYBROWSER_" + line.substr(5, eq - 5), line.substr(eq + 1));
    }
    FreeEnvironmentStringsW(block);
    for (auto& [name, value] : forward) {
        if (!GetEnvironmentVariableW(name.c_str(), nullptr, 0))
            SetEnvironmentVariableW(name.c_str(), value.c_str());
    }
}

// When this exe runs on a WebKit built with the GStreamer media backend, CMake copies
// the GStreamer plugins to <exe dir>\gstreamer-1.0. GStreamer only finds them through
// its environment variables, which the WebKit child processes (where media actually
// plays) inherit from us. With the regular Media Foundation build the folder does not
// exist and nothing is changed.
void ConfigureGStreamerEnvironment() {
    wchar_t exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring dir(exe, n);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    dir.resize(slash);

    const std::wstring plugins = dir + L"\\gstreamer-1.0";
    if (GetFileAttributesW(plugins.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    SetEnvironmentVariableW(L"GST_PLUGIN_SYSTEM_PATH_1_0", plugins.c_str());
    const std::wstring scanner = dir + L"\\gst-plugin-scanner.exe";
    if (GetFileAttributesW(scanner.c_str()) != INVALID_FILE_ATTRIBUTES)
        SetEnvironmentVariableW(L"GST_PLUGIN_SCANNER_1_0", scanner.c_str());
    // Plugin cache: rebuilt automatically when the plugin set changes.
    const std::wstring registry = g_settingsDir + L"\\gstreamer-registry.bin";
    SetEnvironmentVariableW(L"GST_REGISTRY_1_0", registry.c_str());
    Log("GStreamer plugins: %ls", plugins.c_str());
}
