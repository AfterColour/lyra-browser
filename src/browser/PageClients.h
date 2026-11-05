// Lyra — колбэки WebKit для страницы: навигация, сбои, alert/confirm/prompt, выбор файла.
#pragma once

#include "common/Platform.h"
#include "browser/Tab.h"

void LoadInfoPage(Tab& t, const std::wstring& heading, const std::wstring& text, const std::wstring& details, const std::wstring& failedUrl);
void StopLoadingIndicator(Tab& t);
void SetPageClients(WKPageRef page, TabCtx* ctx);
void ClearPageClients(WKPageRef page);
