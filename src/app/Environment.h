// Lyra — переменные окружения LYRA_* и настройка GStreamer до запуска WebKit.
#pragma once

#include "common/Platform.h"

void ForwardLyraEnvironment();
void ConfigureGStreamerEnvironment();
// More parallel connections for the curl network backend (see Environment.cpp).
void ConfigureNetworkEnvironment();
