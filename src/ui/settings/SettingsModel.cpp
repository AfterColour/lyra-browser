// Lyra — модель окна настроек: строки, страницы, конструкторы строк.
#include "ui/settings/SettingsModel.h"

SRow RHeader(const wchar_t* t) { SRow r; r.type = SRow::Header; r.label = t; return r; }

SRow RToggle(const wchar_t* label, const wchar_t* desc, bool* v, std::function<void()> changed) {
    SRow r; r.type = SRow::Toggle; r.label = label; r.desc = desc;
    r.getInt = [v]() { return *v ? 1 : 0; };
    r.setInt = [v, changed](int x) { *v = (x != 0); if (changed) changed(); };
    return r;
}

SRow RChoice(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts,
                    std::function<int()> get, std::function<void(int)> set) {
    SRow r; r.type = SRow::Choice; r.label = label; r.desc = desc;
    r.options = std::move(opts); r.getInt = std::move(get); r.setInt = std::move(set);
    return r;
}

SRow RSlider(const wchar_t* label, const wchar_t* desc, int mn, int mx, int step,
                    std::function<int()> get, std::function<void(int)> set,
                    std::function<std::wstring(int)> fmt) {
    SRow r; r.type = SRow::Slider; r.label = label; r.desc = desc;
    r.minV = mn; r.maxV = mx; r.step = step;
    r.getInt = std::move(get); r.setInt = std::move(set); r.fmt = std::move(fmt);
    return r;
}

SRow RText(const wchar_t* label, const wchar_t* desc, std::function<std::wstring()> get,
                  std::function<void(const std::wstring&)> set) {
    SRow r; r.type = SRow::Text; r.label = label; r.desc = desc;
    r.getStr = std::move(get); r.setStr = std::move(set);
    return r;
}

SRow RButton(const wchar_t* label, const wchar_t* desc, const wchar_t* caption,
                    std::function<void()> action) {
    SRow r; r.type = SRow::Button; r.label = label; r.desc = desc; r.right = caption;
    r.action = std::move(action);
    return r;
}

SRow RInfo(const wchar_t* label, const std::wstring& right, bool kbd) {
    SRow r; r.type = SRow::Info; r.label = label; r.right = right; r.kbd = kbd;
    return r;
}

SRow RToggleGear(const wchar_t* label, const wchar_t* desc, bool* v, std::function<void()> changed,
                 std::function<void()> openDetails) {
    SRow r = RToggle(label, desc, v, std::move(changed));
    r.type = SRow::ToggleGear;
    r.action = std::move(openDetails);
    return r;
}

SRow RSegmented(const wchar_t* label, const wchar_t* desc, std::vector<std::wstring> opts,
                std::function<int()> get, std::function<void(int)> set) {
    SRow r = RChoice(label, desc, std::move(opts), std::move(get), std::move(set));
    r.type = SRow::Segmented;
    return r;
}

SRow RCards(const wchar_t* label, int kind, std::vector<std::wstring> opts,
            std::function<int()> get, std::function<void(int)> set) {
    SRow r = RChoice(label, L"", std::move(opts), std::move(get), std::move(set));
    r.type = SRow::Cards;
    r.cardKind = kind;
    return r;
}

SRow RSwatches(const wchar_t* label, std::vector<Color> colors, std::vector<std::wstring> names,
               std::function<int()> get, std::function<void(int)> set) {
    SRow r = RChoice(label, L"", std::move(names), std::move(get), std::move(set));
    r.type = SRow::Swatches;
    r.colors = std::move(colors);
    return r;
}
