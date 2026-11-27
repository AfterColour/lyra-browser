// Lyra — раскладки интерфейса: где стоят кнопки, размеры, скругления, шрифт и цвета панелей.
//
// Three built-in layouts are the tab layouts Lyra always had (compact, separate tab bar,
// vertical); they never change and their sizes follow Settings > Appearance > Toolbar size.
// The user's own layouts live in %LOCALAPPDATA%\Lyra\layouts.json. Editing a built-in one
// (ui/Customize: "Customize toolbar") never changes it: the first change makes a copy, "My layout",
// which is switched on and keeps every later change.
//
// Items (buttons) sit in zones; which zones there are depends on where the tabs are:
//   Left, Right      the address row, left and right of the address field
//   Field            inside the address field, at its right end (small buttons)
//   TabEnd           right after the tabs (the new tab button; in the vertical list: under it)
//   StripLeft/Right  the tab strip row of the separate layout, before and after the tabs
//   SideTop          above the vertical tab list
// Items in no zone are hidden (the palette while customizing).
#pragma once

#include "common/Platform.h"

enum UiItem {
    kItemBack, kItemForward, kItemReload, kItemHome, kItemNewTab, kItemDownloads,
    kItemExtensions, kItemSidebar, kItemMenu, kItemCount
};

enum UiZone {
    kZoneLeft, kZoneRight, kZoneField, kZoneTabEnd, kZoneStripLeft, kZoneStripRight, kZoneSideTop, kZoneCount
};

// Colours a layout may set over the theme's (alpha 0: the theme's own).
enum UiColor {
    kColFrame,        // tab strip, toolbar, vertical tab list
    kColField,        // address field
    kColFieldBorder,  // address field and active tab border
    kColTabActive,    // the active tab
    kColText,         // titles and the address
    kColText2,        // secondary text, close buttons
    kColIcon,         // toolbar glyphs
    kColAccent,       // focus ring, loading line, marks
    kColHover,        // hover highlight
    kColCount
};

struct UiStyle {
    // 96-DPI pixels
    float bar = 36, strip = 34, btn = 28, tab = 26, icon = 14, font = 12, sideW = 220;
    float tabMaxW = 150;              // widest a tab gets (horizontal layouts)
    float fieldMaxW = 860;            // widest the address field gets (0: no limit)
    float tabRadius = 6, fieldRadius = 7, btnRadius = 6;
    float fieldBorder = 1, tabBorder = 1;   // line widths, 0: none
    bool  separators = true;          // hairlines between the chrome and the page
    std::wstring fontFamily;          // "" = Segoe UI Variable
    Color colors[kColCount];          // alpha 0: the theme's
    UiStyle() { for (Color& c : colors) c = Color(0, 0, 0, 0); }   // (GDI+ Color() is opaque black)
};

struct UiItemStyle {
    float size = 0;                   // button size (96-DPI px), 0: the layout's
    Color color = Color(0, 0, 0, 0);  // glyph colour, alpha 0: the layout's
};

struct UiLayoutDef {
    std::wstring id;                  // "builtin:0..2" or "c1", "c2"...
    std::wstring name;
    int mode = 0;                     // where the tabs are: kLayoutCompact / Separate / Vertical (ui/Chrome.h)
    std::vector<int> zones[kZoneCount];
    UiStyle style;
    UiItemStyle items[kItemCount];
};

void UiLoad();                                       // at startup, after LoadSettings
const UiLayoutDef& UiActive();                       // the layout in use (a built-in one: resolved for the toolbar size)
bool UiActiveIsBuiltin();
UiLayoutDef UiBuiltin(int mode);                     // a built-in layout at the current toolbar size
const std::vector<UiLayoutDef>& UiCustomLayouts();
void UiSelectBuiltin(int mode);                      // also Settings > Tabs > Tab layout
void UiSelectCustom(const std::wstring& id);
// The active layout to change. A built-in one is copied first ("My layout N"), and the copy is
// switched on. Call UiChanged afterwards.
UiLayoutDef& UiEdit();
enum UiChange { kUiLayoutOnly, kUiFonts, kUiColors };
void UiChanged(UiChange what);                       // saves (a moment later) and applies
void UiRename(const std::wstring& name);
void UiDelete(const std::wstring& id);               // a custom layout; the active one: back to its built-in base

// Item facts
const wchar_t* UiItemName(int item);
const wchar_t* UiItemGlyph(int item);
bool UiItemAllowedIn(int item, int zone);
int  UiItemZone(const UiLayoutDef& d, int item, int* index = nullptr);   // -1: hidden
bool UiZoneInMode(int zone, int mode);
const wchar_t* UiColorName(int c);

// Colours of the active layout over the theme (ui/Theme.cpp calls it last).
struct Theme;
void UiApplyColors(Theme& t);
bool UiFrameColorSet();                              // its own frame colour: no Mica behind the chrome
