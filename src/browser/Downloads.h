// Lyra — загрузки файлов в папку «Загрузки».
#pragma once

#include "common/Platform.h"

void OnActionBecameDownload(WKPageRef, WKNavigationActionRef, WKDownloadRef d, const void* ci);
void OnResponseBecameDownload(WKPageRef, WKNavigationResponseRef, WKDownloadRef d, const void* ci);
void OnContextMenuDownload(WKPageRef, WKDownloadRef d, const void*);
