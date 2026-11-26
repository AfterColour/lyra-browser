// Lyra — «Сообщить о проблеме»: файл с журналом и сведениями о компьютере + новое issue на GitHub.
#pragma once

#include "common/Platform.h"

#include <string>

// Settings > About > Report a problem, and the command palette. Asks first (owner: the window the
// question belongs to), then saves %LOCALAPPDATA%\Lyra\Reports\lyra-report-<time>.txt, shows it in
// Explorer and opens a new GitHub issue in a tab with the system line filled in; the person
// attaches the file themselves. Nothing is sent by Lyra.
void ReportProblem(HWND owner);

// The report's text (for tests and the developer): system, settings, extensions, the log's end.
std::wstring BugReportText();
