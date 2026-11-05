// Lyra — модель окна настроек: строки, страницы, конструкторы строк.
#pragma once

#include "common/Platform.h"

// ---- model -----------------------------------------------------------------
struct SRow {
    enum Type { Header, Toggle, Choice, Slider, Text, Button, Info };
    Type type = Info;
    std::wstring label, desc, right;
    bool kbd = false;
    std::vector<std::wstring> options;
    int minV = 0, maxV = 100, step = 1;
    std::function<int()> getInt;
    std::function<void(int)> setInt;
    std::function<std::wstring()> getStr;
    std::function<void(const std::wstring&)> setStr;
    std::function<std::wstring(int)> fmt;
    std::function<void()> action;
};
struct SPage { std::wstring name; const wchar_t* icon = L""; std::vector<SRow> rows; };
struct SView {
    SRow* row = nullptr;          // null => synthetic heading (search results)
    std::wstring heading;
    float y = 0, h = 0;
    int group = -1;
    bool first = false, last = false;
};

SRow RHeader(const wchar_t* t);
SRow RToggle(const wchar_t* label, const wchar_t* desc, bool* v, std::function<void()> changed);
SRow RChoice(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts, std::function<int()> get, std::function<void(int)> set);
SRow RSlider(const wchar_t* label, const wchar_t* desc, int mn, int mx, int step, std::function<int()> get, std::function<void(int)> set, std::function<std::wstring(int)> fmt);
SRow RText(const wchar_t* label, const wchar_t* desc, std::function<std::wstring()> get, std::function<void(const std::wstring&)> set);
SRow RButton(const wchar_t* label, const wchar_t* desc, const wchar_t* caption, std::function<void()> action);
SRow RInfo(const wchar_t* label, const std::wstring& right, bool kbd = false);
