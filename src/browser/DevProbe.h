// Lyra — инструмент разработчика: замеры на странице (LYRA_PROBE=<файл .js>), результат в tabs.log.
#pragma once

#include "common/Platform.h"

struct Tab;

// With the environment variable LYRA_PROBE set to a JavaScript file, that file runs as an async
// function body in every web page that finishes loading; whatever it returns (it may await) is
// written to tabs.log as "probe: ...". Nothing happens without the variable.
void DevProbePageLoaded(Tab& t);
// LYRA_PROCS=<seconds> (developer): pages, their processes and memory in tabs.log.
void DevProcessesStart();
