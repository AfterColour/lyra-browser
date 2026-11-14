// Lyra — модель окна настроек: строки, страницы, конструкторы строк.
#pragma once

#include "common/Platform.h"

// ---- model -----------------------------------------------------------------
struct SRow {
    enum Type {
        Header,      // group title above a card
        Toggle,      // switch
        ToggleGear,  // switch + gear button that opens a page with the details
        Choice,      // drop-down list
        Segmented,   // a few options side by side
        Cards,       // full-width picture cards (theme / tab layout)
        Swatches,    // colour circles
        Slider, Text, Button, Info,
        Custom       // drawn by settings.html itself; `right` names it ("start-wallpaper", "start-layout")
    };
    Type type = Info;
    std::wstring label, desc, right;
    bool kbd = false;
    int  cardKind = 0;                    // Cards: 0 theme previews, 1 tab layout previews
    std::vector<std::wstring> options;    // Choice / Segmented / Cards labels, Swatches names
    std::vector<Color> colors;            // Swatches
    int minV = 0, maxV = 100, step = 1;
    std::function<int()> getInt;
    std::function<void(int)> setInt;
    std::function<std::wstring()> getStr;
    std::function<void(const std::wstring&)> setStr;
    std::function<std::wstring(int)> fmt;
    std::function<void()> action;         // Button; ToggleGear: the gear
};
struct SPage {
    std::wstring name;
    const wchar_t* icon = L"";
    Color tint;                           // background of the icon in the sidebar
    int group = 0;                        // sidebar section: a gap is left between groups
    int parent = -1;                      // >= 0: a detail page reached from a row of that page (not in the sidebar)
    std::vector<SRow> rows;
};

SRow RHeader(const wchar_t* t);
SRow RToggle(const wchar_t* label, const wchar_t* desc, bool* v, std::function<void()> changed);
SRow RToggleGear(const wchar_t* label, const wchar_t* desc, bool* v, std::function<void()> changed, std::function<void()> openDetails);
SRow RChoice(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts, std::function<int()> get, std::function<void(int)> set);
SRow RSegmented(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts, std::function<int()> get, std::function<void(int)> set);
SRow RCards(const wchar_t* label, int kind, std::vector<std::wstring> opts, std::function<int()> get, std::function<void(int)> set);
SRow RSwatches(const wchar_t* label, std::vector<Color> colors, std::vector<std::wstring> names, std::function<int()> get, std::function<void(int)> set);
SRow RSlider(const wchar_t* label, const wchar_t* desc, int mn, int mx, int step, std::function<int()> get, std::function<void(int)> set, std::function<std::wstring(int)> fmt);
SRow RText(const wchar_t* label, const wchar_t* desc, std::function<std::wstring()> get, std::function<void(const std::wstring&)> set);
SRow RButton(const wchar_t* label, const wchar_t* desc, const wchar_t* caption, std::function<void()> action);
SRow RInfo(const wchar_t* label, const std::wstring& right, bool kbd = false);
SRow RCustom(const wchar_t* kind, const wchar_t* label);
