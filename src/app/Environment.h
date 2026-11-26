// Lyra — переменные окружения LYRA_* и настройка GStreamer до запуска WebKit.
#pragma once

#include "common/Platform.h"

void ForwardLyraEnvironment();
void ConfigureGStreamerEnvironment();
// Lyra.exe --gst-registry (run by the installer after copying the files): builds GStreamer's
// plugin cache and exits. Returns the process exit code.
int BuildGStreamerRegistry();
// More parallel connections for the curl network backend (see Environment.cpp).
void ConfigureNetworkEnvironment();
