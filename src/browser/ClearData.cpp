// Lyra — очистка данных браузера.
#include "browser/ClearData.h"
#include "app/Log.h"
#include "browser/Downloads.h"
#include "browser/History.h"
#include "browser/Passwords.h"
#include "browser/Permissions.h"
#include "browser/StartPage.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Localization.h"

#include <WebKit/WKWebsiteDataStoreRef.h>

namespace {

// Seconds back for each range; 0 = all time.
constexpr long long kRangeSeconds[] = { 3600, 24 * 3600, 7 * 24 * 3600, 28 * 24 * 3600, 0 };

void OnEngineDone(void* context) {
    std::unique_ptr<std::function<void()>> done(static_cast<std::function<void()>*>(context));
    Log("clear data: engine done");
    if (*done) (*done)();
}

} // namespace

std::vector<std::wstring> ClearDataRanges() {
    return { Tr(L"Last hour", L"За последний час"), Tr(L"Last 24 hours", L"За последние 24 часа"),
             Tr(L"Last 7 days", L"За последние 7 дней"), Tr(L"Last 4 weeks", L"За последние 4 недели"),
             Tr(L"All time", L"За всё время") };
}

void ClearBrowsingData(const ClearDataChoice& c, std::function<void()> done) {
    const int range = std::min(std::max(c.range, 0), (int)std::size(kRangeSeconds) - 1);
    const long long back = kRangeSeconds[range];
    const long long sinceMs = back ? NowUnixMs() - back * 1000 : 0;
    Log("clear data: range %d history=%d cookies=%d cache=%d downloads=%d permissions=%d passwords=%d",
        range, c.history, c.cookies, c.cache, c.downloads, c.permissions, c.passwords);

    if (c.history) {
        HistoryClearSince(sinceMs);
        g_closedUrls.clear();                            // no times kept for them: all of them
        RefreshStartPages();
    }
    if (c.downloads) DownloadsClear();
    if (c.permissions) PermissionsReset();
    if (c.passwords) {
        for (const SavedPassword& p : PasswordsList())
            if (!sinceMs || p.lastUsed >= sinceMs / 1000) PasswordsDelete(p.origin, p.username);   // lastUsed: seconds
    }

    WKWebsiteDataKinds kinds = 0;
    if (c.cookies) kinds |= kWKWebsiteDataCookiesAndSiteData;
    if (c.cache) kinds |= kWKWebsiteDataCaches;
    if (!kinds || !g_dataStore) {
        if (done) done();
        return;
    }
    auto* context = new std::function<void()>(std::move(done));
    WKWebsiteDataStoreRemoveDataModifiedSince(g_dataStore, kinds, sinceMs / 1000.0, context, OnEngineDone);
}
