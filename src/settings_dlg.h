#ifndef SETTINGS_DLG_H
#define SETTINGS_DLG_H
#include <windows.h>
#include <string>

extern std::wstring g_homePage;
extern std::wstring g_searchEngine;
extern double g_scaleFactor;

void OpenSettingsDialog(HWND parent);
#endif
