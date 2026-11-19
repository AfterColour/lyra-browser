// Lyra — масштаб для каждого сайта: Ctrl + / − / 0 меняют масштаб сайта открытой вкладки.
//
// zoom.tsv (UTF-8): one "<site>\t<factor>" line per site with its own zoom.
#include "browser/SiteZoom.h"
#include "app/Log.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "core/Url.h"
#include "ui/Chrome.h"

#include <fstream>

namespace {

std::map<std::wstring, double> s_zoom;           // saved
std::map<std::wstring, double> s_private;        // set in private tabs (this session only)
bool s_loaded = false;

// Chrome's zoom steps.
const double kSteps[] = { 0.25, 0.33, 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0 };

std::wstring File() { return g_settingsDir + L"\\zoom.tsv"; }

void Load() {
    if (s_loaded) return;
    s_loaded = true;
    std::ifstream f(File(), std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        const double z = atof(line.c_str() + tab + 1);
        if (z >= 0.25 && z <= 5.0) s_zoom[ToWide(line.substr(0, tab))] = z;
    }
}

void Save() {
    CreateDirectoryW(g_settingsDir.c_str(), nullptr);
    const std::wstring path = File(), tmp = path + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        for (auto& [site, z] : s_zoom) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f", z);
            f << ToUtf8(site) << '\t' << buf << '\n';
        }
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

// The key a page's zoom is kept under: its host ("www." dropped), or the scheme for pages
// without one (local files share one zoom).
std::wstring SiteOf(const Tab& t) {
    const std::wstring url = t.lazyPending ? t.lazyUrl : t.url;
    std::wstring host = HostOf(url);
    if (host.compare(0, 4, L"www.") == 0) host = host.substr(4);
    if (!host.empty()) return host;
    const size_t colon = url.find(L':');
    return colon != std::wstring::npos && colon < 16 ? url.substr(0, colon) : std::wstring();
}

bool Lookup(const Tab& t, double* z) {
    Load();
    const std::wstring site = SiteOf(t);
    if (site.empty()) return false;
    if (t.privateTab) {
        auto p = s_private.find(site);
        if (p != s_private.end()) { *z = p->second; return true; }
    }
    auto it = s_zoom.find(site);
    if (it == s_zoom.end()) return false;
    *z = it->second;
    return true;
}

// Every tab of the site gets its zoom (private ones only from private changes, and the other way).
void ApplyToSite(const std::wstring& site, bool privateTabs) {
    for (auto& t : g_tabs) {
        if (!t->page || t->privateTab != privateTabs || SiteOf(*t) != site) continue;
        WKPageSetPageZoomFactor(t->page, SiteZoomFor(*t));
    }
    InvalidateChrome();
}

void Set(Tab& t, double z) {
    const std::wstring site = SiteOf(t);
    if (site.empty()) { WKPageSetPageZoomFactor(t.page, z); InvalidateChrome(); return; }
    auto& map = t.privateTab ? s_private : s_zoom;
    if (std::fabs(z - g_zoom) < 0.005) map.erase(site);      // the default again: not its own any more
    else map[site] = z;
    if (!t.privateTab) Save();
    Log("zoom: %ls %d%%", site.c_str(), (int)std::lround(z * 100));
    ApplyToSite(site, t.privateTab);
}

}  // namespace

double SiteZoomFor(const Tab& t) {
    double z = g_zoom;
    Lookup(t, &z);
    return z;
}

bool SiteZoomIsCustom(const Tab& t) {
    double z = 0;
    return Lookup(t, &z) && std::fabs(z - g_zoom) >= 0.005;
}

void SiteZoomApply(Tab& t) {
    if (!t.page) return;
    const double z = SiteZoomFor(t);
    if (std::fabs(WKPageGetPageZoomFactor(t.page) - z) >= 0.001) WKPageSetPageZoomFactor(t.page, z);
}

void SiteZoomStep(int direction) {
    Tab* t = ActiveTab();
    if (!t || !t->page) return;
    const double cur = WKPageGetPageZoomFactor(t->page);
    double next = cur;
    if (direction > 0) {
        for (double s : kSteps) if (s > cur + 0.001) { next = s; break; }
    } else {
        for (int i = (int)(sizeof(kSteps) / sizeof(kSteps[0])) - 1; i >= 0; --i)
            if (kSteps[i] < cur - 0.001) { next = kSteps[i]; break; }
    }
    if (std::fabs(next - cur) < 0.001) return;
    Set(*t, next);
}

void SiteZoomReset() {
    if (Tab* t = ActiveTab()) if (t->page) Set(*t, g_zoom);
}

void SiteZoomDefaultChanged() {
    for (auto& t : g_tabs) if (t->page) WKPageSetPageZoomFactor(t->page, SiteZoomFor(*t));
    InvalidateChrome();
}
