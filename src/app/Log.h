// Lyra — диагностический журнал tabs.log.
#pragma once

#include "common/Platform.h"

void Log(const char* fmt, ...);
void LogOpen();
void LogAddress(const char* label, const void* addr);
