// Lyra — содержимое разделов настроек и сброс к значениям по умолчанию.
#include "ui/settings/SettingsPages.h"
#include <WebKit/WKTextChecker.h>
#include "browser/Translate.h"
#include "browser/AdBlock.h"
#include "browser/ClearData.h"
#include "browser/Passwords.h"
#include "browser/Permissions.h"
#include "browser/Downloads.h"
#include "browser/History.h"
#include "core/Url.h"
#include "browser/StartPage.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "browser/WebContext.h"
#include "core/Constants.h"
#include "core/Localization.h"
#include "core/Settings.h"
#include "core/StringUtils.h"
#include "ui/Chrome.h"
#include "ui/Commands.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/settings/SettingsModel.h"
#include "ui/settings/SettingsWindow.h"
#include "LyraVersion.h"

// Icon background colours in the sidebar
static const Color kTintGray(122, 128, 138), kTintPurple(107, 91, 214), kTintBlue(31, 122, 224),
                   kTintOrange(217, 120, 11), kTintGreen(46, 143, 71), kTintTeal(29, 147, 168),
                   kTintGraphite(94, 99, 107);

static void ResetAllSettings() {
    const int oldLanguage = g_prefs.language;
    const bool onboarded = g_prefs.onboarded;
    g_prefs = Prefs();
    g_prefs.onboarded = onboarded;
    g_homePage = kStartAlias;
    g_downloadDir.clear();
    g_searchEngine = L"https://duckduckgo.com/?q=";
    ApplyWebPrefs();
    SetZoom(1.0);                         // also saves
    ApplyTheme(ResolveDark());
    ApplyChromeSettings();
    SaveSettings();
    if (oldLanguage != g_prefs.language) SettingsLanguageChanged();
    if (g_setHwnd) InvalidateRect(g_setHwnd, nullptr, FALSE);
}

static int s_passwordsPageIndex = -1;
static int s_clearDataPageIndex = -1;

// Clear browsing data: the choice stays for the session; the row under the button says how it went.
static ClearDataChoice s_clearChoice;
static std::wstring s_clearStatus;
static bool s_clearing = false;

static void ClearChosenData() {
    const ClearDataChoice& c = s_clearChoice;
    if (s_clearing) return;
    if (!c.history && !c.cookies && !c.cache && !c.downloads && !c.permissions && !c.passwords) {
        MessageBoxW(g_setHwnd, Tr(L"Choose what to clear first.", L"Сначала выберите, что удалить."),
                    Tr(L"Lyra Settings", L"Настройки Lyra"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    const std::wstring range = ClearDataRanges()[c.range];
    std::wstring q = Tr(L"Clear the chosen data (", L"Удалить выбранные данные (") + range + L")?";
    if (c.cookies) q += Tr(L"\n\nYou will be signed out of most sites.", L"\n\nВы выйдете из большинства сайтов.");
    if (c.passwords) q += Tr(L"\n\nSaved passwords cannot be restored.", L"\n\nСохранённые пароли не вернуть.");
    if (MessageBoxW(g_setHwnd, q.c_str(), Tr(L"Lyra Settings", L"Настройки Lyra"), MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    s_clearing = true;
    s_clearStatus = Tr(L"Clearing\x2026", L"Удаление\x2026");
    SettingsLanguageChanged();                           // shows the status
    ClearBrowsingData(c, [range]() {
        s_clearing = false;
        s_clearStatus = Tr(L"Done: data cleared (", L"Готово: данные удалены (") + range + L").";
        SettingsLanguageChanged();
    });
}

void OpenClearDataSettings() {
    ShowSettingsWindow();
    if (s_clearDataPageIndex < 0) return;
    SettingsOpenPage(s_clearDataPageIndex);
}

static void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_setHwnd)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(h), s.c_str(), bytes);
        GlobalUnlock(h);
        if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
    }
    CloseClipboard();
}

// Saved passwords page: copy the name or the password, or delete the sign-in.
static void PasswordActions(const std::string& origin, const std::wstring& username) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1, Tr(L"Copy username", L"Копировать имя пользователя"));
    AppendMenuW(m, MF_STRING, 2, Tr(L"Copy password", L"Копировать пароль"));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, 3, Tr(L"Delete", L"Удалить"));
    POINT pt;
    GetCursorPos(&pt);
    const int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_setHwnd, nullptr);
    DestroyMenu(m);
    if (cmd == 1) CopyText(username);
    else if (cmd == 2) {
        for (const SavedPassword& e : PasswordsList())
            if (e.origin == origin && e.username == username) { CopyText(e.password); break; }
    } else if (cmd == 3) {
        std::wstring q = Tr(L"Delete the saved password for ", L"Удалить сохранённый пароль для ") + PasswordsSiteName(origin) + L"?";
        if (MessageBoxW(g_setHwnd, q.c_str(), Tr(L"Lyra Settings", L"Настройки Lyra"), MB_YESNO | MB_ICONQUESTION) == IDYES) {
            PasswordsDelete(origin, username);
            SettingsLanguageChanged();                   // rebuilds the list
        }
    }
}

void OpenPasswordsSettings() {
    ShowSettingsWindow();
    if (s_passwordsPageIndex < 0) return;
    SettingsOpenPage(s_passwordsPageIndex);
    SettingsLanguageChanged();                           // rebuilt: the list is up to date
}

void BuildPages() {
    g_pages.clear();
    const std::function<void()> save    = []() { SaveSettings(); };
    const std::function<void()> web     = []() { ApplyWebPrefs(); SaveSettings(); };
    const std::function<void()> theme   = []() { ApplyTheme(ResolveDark()); SaveSettings(); };
    const std::function<void()> layout  = []() { ApplyChromeSettings(); SaveSettings(); };
    const std::function<void()> repaint = []() { InvalidateChrome(); SaveSettings(); };
    // Index of the "Animations" detail page; it is added after the sidebar pages.
    static int s_animationsPage = -1;
    static int s_passwordsPage = -1;     // "Saved passwords", from Privacy

    {   // General
        SPage p; p.name = Tr(L"General", L"Основные"); p.icon = L"\uE713"; p.tint = kTintGray;
        p.rows.push_back(RHeader(Tr(L"Startup", L"Запуск")));
        p.rows.push_back(RToggle(Tr(L"Restore tabs", L"Восстанавливать вкладки"),
            Tr(L"Opens the tabs of the last session. After a crash they always come back.",
               L"Открывает вкладки прошлого сеанса. После сбоя они возвращаются всегда."),
            &g_prefs.restoreSession, []() { SaveSettings(); }));
        p.rows.push_back(RText(Tr(L"Home page", L"Домашняя страница"),
            Tr(L"Opened when the browser starts. lyra://start is the start page.",
               L"Открывается при запуске. lyra://start — стартовая страница."),
            []() { return g_homePage; },
            [](const std::wstring& s) { std::wstring t = TrimWs(s); if (!t.empty()) { g_homePage = t; SaveSettings(); } }));
        p.rows.push_back(RButton(Tr(L"Use current page", L"Текущая страница"),
            Tr(L"Make the page in the active tab your home page.", L"Сделать домашней страницу из активной вкладки."),
            Tr(L"Set", L"Выбрать"),
            []() {
                Tab* t = ActiveTab();
                if (!t || t->url.empty() || t->url == L"about:blank") return;
                g_homePage = IsStartPageUrl(t->url) ? std::wstring(kStartAlias) : t->url;
                SaveSettings();
            }));

        p.rows.push_back(RHeader(Tr(L"Start page", L"Стартовая страница")));
        p.rows.push_back(RToggle(Tr(L"Favorites", L"Избранное"),
            Tr(L"Sites on the start page. Add them with the + tile.", L"Сайты на стартовой странице. Добавляются плиткой «+»."),
            &g_prefs.startFavorites, []() { SaveSettings(); RefreshStartPages(); }));
        p.rows.push_back(RToggle(Tr(L"Frequently visited", L"Часто посещаемые"),
            Tr(L"The sites you open most, from your history.", L"Сайты, которые вы открываете чаще всего, — по истории."),
            &g_prefs.startFrequent, []() { SaveSettings(); RefreshStartPages(); }));
        p.rows.push_back(RButton(Tr(L"Welcome screen", L"Экран приветствия"),
            Tr(L"The first-run setup: language, theme, tabs.", L"Первоначальная настройка: язык, тема, вкладки."),
            Tr(L"Show", L"Показать"),
            []() { CreateTab(StartPageUrl(true)); }));

        p.rows.push_back(RHeader(Tr(L"Search", L"Поиск")));
        std::vector<std::wstring> names;
        for (int i = 0; i < kEngineCount; ++i) names.push_back(kEngines[i].name);
        names.push_back(Tr(L"Custom", L"Свой"));
        p.rows.push_back(RChoice(Tr(L"Search engine", L"Поисковая система"),
            Tr(L"Used for anything typed in the address bar that is not an address.",
               L"Для всего, что введено в адресную строку и не является адресом."),
            names,
            []() { for (int i = 0; i < kEngineCount; ++i) if (g_searchEngine == kEngines[i].url) return i; return kEngineCount; },
            [save](int i) { if (i < kEngineCount) { g_searchEngine = kEngines[i].url; save(); } }));
        p.rows.push_back(RText(Tr(L"Search address", L"Адрес поиска"),
            Tr(L"The query is added to the end of this address.", L"Запрос добавляется в конец этого адреса."),
            []() { return g_searchEngine; },
            [](const std::wstring& s) { std::wstring t = TrimWs(s); if (!t.empty()) { g_searchEngine = t; SaveSettings(); } }));
        p.rows.push_back(RToggle(Tr(L"Show full address", L"Показывать полный адрес"),
            Tr(L"Otherwise the address field shows only the site name.", L"Иначе в адресной строке видно только имя сайта."),
            &g_prefs.showFullUrl, repaint));
        p.rows.push_back(RHeader(Tr(L"Translation", L"Перевод")));
        {
            std::vector<std::wstring> langs;
            for (const auto& l : TranslateLanguages()) langs.push_back(l.name);
            langs[0] = Tr(L"Lyra's language", L"Язык Lyra");
            p.rows.push_back(RChoice(Tr(L"Translate into", L"Переводить на"),
                Tr(L"Pages in other languages show a translate icon in the address field.",
                   L"На страницах на других языках в адресной строке появляется значок перевода."),
                langs,
                []() { return g_prefs.translateTarget; },
                [save](int i) { g_prefs.translateTarget = i; save(); }));
        }
        p.rows.push_back(RChoice(Tr(L"Service", L"Сервис"),
            Tr(L"Google needs nothing; DeepL needs your own API key (DeepL API Free: 500,000 characters a month).",
               L"Google работает сразу; DeepL — со своим ключом API (DeepL API Free: 500 000 символов в месяц)."),
            { Tr(L"Google (no key)", L"Google (без ключа)"), Tr(L"DeepL (API key)", L"DeepL (ключ API)") },
            []() { return g_prefs.translateService; },
            [save](int i) { g_prefs.translateService = i; save(); }));
        p.rows.push_back(RText(Tr(L"DeepL API key", L"Ключ DeepL API"),
            Tr(L"Kept encrypted for your Windows account. Free keys end with :fx.",
               L"Хранится зашифрованным для вашей учётной записи Windows. Бесплатные ключи заканчиваются на :fx."),
            []() { return TranslateGetDeepLKey(); },
            [](const std::wstring& s) { TranslateSetDeepLKey(s); }));

        p.rows.push_back(RHeader(Tr(L"Page zoom", L"Масштаб страниц")));
        p.rows.push_back(RSlider(Tr(L"Default zoom", L"Масштаб по умолчанию"),
            Tr(L"Ctrl +, Ctrl - and Ctrl 0 change it too.", L"Также меняется сочетаниями Ctrl +, Ctrl - и Ctrl 0."),
            25, 300, 5,
            []() { return (int)std::lround(g_zoom * 100.0); },
            [](int v) { SetZoom(v / 100.0); },
            [](int v) { return std::to_wstring(v) + L"%"; }));

        p.rows.push_back(RHeader(Tr(L"Language", L"Язык")));
        p.rows.push_back(RChoice(Tr(L"Interface language", L"Язык интерфейса"), L"",
            { Tr(L"As in Windows", L"Как в Windows"), L"English", L"Русский" },
            []() { return g_prefs.language; },
            [](int i) {
                g_prefs.language = i;
                SaveSettings();
                if (g_hWnd) InvalidateRect(g_hWnd, nullptr, FALSE);
                SettingsLanguageChanged();
                RefreshStartPages();
            }));
        p.rows.push_back(RToggle(Tr(L"Check spelling", L"Проверять орфографию"),
            WKTextCheckerIsContinuousSpellCheckingAllowed()
                ? Tr(L"Misspelled words in text fields are underlined; right-click one for corrections. Uses the Windows dictionaries (English, Russian and your Windows languages).",
                     L"Слова с ошибками в полях ввода подчёркиваются, правый клик по слову — варианты исправления. Словари Windows: английский, русский и языки Windows.")
                : Tr(L"Windows has no spelling dictionaries here (Settings > Time & language > Language).",
                     L"В Windows не установлены словари (Параметры > Время и язык > Язык)."),
            &g_prefs.spellCheck, []() { SaveSettings(); WKTextCheckerSetContinuousSpellCheckingEnabled(g_prefs.spellCheck); }));
        g_pages.push_back(std::move(p));
    }
    const int appearancePage = (int)g_pages.size();
    {   // Appearance
        SPage p; p.name = Tr(L"Appearance", L"Внешний вид"); p.icon = L"\uE790"; p.tint = kTintPurple;
        p.rows.push_back(RHeader(Tr(L"Theme", L"Тема")));
        p.rows.push_back(RCards(Tr(L"Theme", L"Тема"), 0,
            { Tr(L"As in Windows", L"Как в Windows"), Tr(L"Light", L"Светлая"), Tr(L"Dark", L"Тёмная") },
            []() { return g_prefs.themeMode; },
            [theme](int i) { g_prefs.themeMode = i; theme(); }));

        p.rows.push_back(RHeader(Tr(L"Accent color", L"Акцентный цвет")));
        std::vector<Color> colors;
        colors.push_back(WindowsAccentColor());
        for (int i = 1; i < kAccentCount; ++i) colors.push_back(Color(kAccents[i].r, kAccents[i].g, kAccents[i].b));
        p.rows.push_back(RSwatches(Tr(L"Color", L"Цвет"), colors,
            { Tr(L"As in Windows", L"Как в Windows"), Tr(L"Blue", L"Синий"), Tr(L"Purple", L"Фиолетовый"),
              Tr(L"Pink", L"Розовый"), Tr(L"Red", L"Красный"), Tr(L"Orange", L"Оранжевый"),
              Tr(L"Yellow", L"Жёлтый"), Tr(L"Green", L"Зелёный"), Tr(L"Graphite", L"Графит") },
            []() { return g_prefs.accent; },
            [theme](int i) { g_prefs.accent = i; theme(); }));

        p.rows.push_back(RHeader(Tr(L"Toolbar", L"Панели")));
        p.rows.push_back(RSegmented(Tr(L"Toolbar size", L"Размер панелей"),
            Tr(L"Compact leaves the most room for pages.", L"Компактный оставляет больше места страницам."),
            { Tr(L"Compact", L"Компактный"), Tr(L"Normal", L"Обычный"), Tr(L"Large", L"Крупный") },
            []() { return g_prefs.density; },
            [layout](int i) { g_prefs.density = i; layout(); }));
        if (MicaSupported())
            p.rows.push_back(RToggle(Tr(L"Mica", L"Эффект Mica"),
                Tr(L"The desktop shows through the tab strip and toolbar, softly blurred.",
                   L"Фон рабочего стола мягко просвечивает сквозь панели вкладок и инструментов."),
                &g_prefs.mica, theme));
        else
            p.rows.push_back(RInfo(Tr(L"Mica", L"Эффект Mica"), Tr(L"Needs Windows 11 22H2", L"Нужна Windows 11 22H2")));
        p.rows.push_back(RSegmented(Tr(L"Page loading", L"Загрузка страницы"),
            Tr(L"How the address field shows that a page is loading.", L"Как адресная строка показывает загрузку страницы."),
            { Tr(L"Glowing line", L"Светящаяся линия"), Tr(L"Fill", L"Заливка") },
            []() { return g_prefs.loadIndicator; },
            [repaint](int i) { g_prefs.loadIndicator = i; repaint(); }));
        p.rows.push_back(RToggleGear(Tr(L"Animations", L"Анимации"),
            Tr(L"Tabs glide, highlights fade. The gear opens the details.",
               L"Вкладки плавно двигаются, подсветка плавно гаснет. Шестерёнка — подробные настройки."),
            &g_prefs.animations, repaint,
            []() { SettingsOpenPage(s_animationsPage); }));
        g_pages.push_back(std::move(p));
    }
    {   // Tabs
        SPage p; p.name = Tr(L"Tabs", L"Вкладки"); p.icon = L"\uE7C4"; p.tint = kTintBlue;
        p.rows.push_back(RHeader(Tr(L"Tab layout", L"Расположение вкладок")));
        p.rows.push_back(RCards(Tr(L"Tab layout", L"Расположение вкладок"), 1,
            { Tr(L"Compact", L"Компактно"), Tr(L"Separate tab bar", L"Отдельной строкой"), Tr(L"Vertical", L"Вертикально") },
            []() { return g_prefs.tabLayout; },
            [layout](int i) { g_prefs.tabLayout = i; layout(); }));

        p.rows.push_back(RHeader(Tr(L"Tabs", L"Вкладки")));
        p.rows.push_back(RChoice(Tr(L"New tab opens", L"Новая вкладка открывает"),
            Tr(L"What Ctrl+T and the + button show.", L"Что показывают Ctrl+T и кнопка +."),
            { Tr(L"Start page", L"Стартовую страницу"), Tr(L"Blank page", L"Пустую страницу"),
              Tr(L"Home page", L"Домашнюю страницу") },
            []() { return g_prefs.newTabMode; },
            [save](int i) { g_prefs.newTabMode = i; save(); }));
        p.rows.push_back(RToggle(Tr(L"Close button on every tab", L"Кнопка закрытия на всех вкладках"),
            Tr(L"Otherwise it appears on the tab under the pointer.", L"Иначе она появляется на вкладке под курсором."),
            &g_prefs.closeAlways, repaint));
        p.rows.push_back(RHeader(Tr(L"Speed", L"Скорость")));
        p.rows.push_back(RSegmented(Tr(L"Ready tabs", L"Готовые вкладки"),
            Tr(L"The tabs used last stay drawn, so switching back to them is instant. More uses more memory.",
               L"Последние открытые вкладки остаются отрисованными — переключение на них мгновенное. Больше — больше памяти."),
            { Tr(L"Off", L"Выкл."), L"1", L"2", L"4" },
            []() { return g_prefs.warmTabs >= 4 ? 3 : g_prefs.warmTabs; },
            [save](int i) { g_prefs.warmTabs = i == 3 ? 4 : i; RefreshLayout(); save(); }));
        p.rows.push_back(RToggle(Tr(L"Put unused tabs to sleep", L"Усыплять неиспользуемые вкладки"),
            Tr(L"Tabs not opened for a while are unloaded from memory and load again when you open them.",
               L"Вкладки, которые давно не открывали, выгружаются из памяти и загружаются снова, когда вы на них переходите."),
            &g_prefs.sleepTabs, save));
        p.rows.push_back(RChoice(Tr(L"Sleep after", L"Усыплять через"),
            Tr(L"Sooner when Windows is short of memory. Never tabs that play sound or have typed text.",
               L"Раньше, если Windows не хватает памяти. Вкладки со звуком или введённым текстом не усыпляются."),
            { Tr(L"5 minutes", L"5 минут"), Tr(L"15 minutes", L"15 минут"), Tr(L"30 minutes", L"30 минут"),
              Tr(L"1 hour", L"1 час"), Tr(L"2 hours", L"2 часа") },
            []() { return g_prefs.sleepAfter; }, [save](int i) { g_prefs.sleepAfter = i; save(); }));
        p.rows.push_back(RToggle(Tr(L"Prepare the new tab", L"Готовить новую вкладку заранее"),
            Tr(L"A start page waits in the background, so a new tab appears at once.",
               L"Стартовая страница ждёт в фоне, и новая вкладка появляется сразу."),
            &g_prefs.spareTab, [save]() { if (g_prefs.spareTab) ScheduleSpareTab(300); else DiscardSpareTab(); save(); }));
        p.rows.push_back(RInfo(Tr(L"Most tabs at once", L"Наибольшее число вкладок"), std::to_wstring(metrics::kMaxTabs)));
        p.rows.push_back(RInfo(Tr(L"Closing the last tab", L"Закрытие последней вкладки"),
                               Tr(L"Closes the window", L"Закрывает окно")));
        g_pages.push_back(std::move(p));
    }
    {   // Webpages
        SPage p; p.name = Tr(L"Webpages", L"Веб-страницы"); p.icon = L"\uE774"; p.tint = kTintOrange;
        p.rows.push_back(RHeader(Tr(L"Content", L"Содержимое")));
        p.rows.push_back(RToggle(Tr(L"JavaScript", L"JavaScript"),
            Tr(L"Turning it off breaks most modern websites.", L"Без него не работает большинство современных сайтов."),
            &g_prefs.js, web));
        p.rows.push_back(RToggle(Tr(L"Load images", L"Загружать изображения"),
            Tr(L"Turn off to save data on a slow connection.", L"Отключите, чтобы экономить трафик."), &g_prefs.images, web));
        p.rows.push_back(RToggle(Tr(L"Allow full screen", L"Разрешить полноэкранный режим"),
            Tr(L"Lets pages such as video players use the whole screen.", L"Например, для видеоплееров."),
            &g_prefs.fullscreen, web));
        p.rows.push_back(RToggle(Tr(L"Web Audio", L"Web Audio"),
            Tr(L"Has no effect yet: Web Audio is off in this WebKit build.", L"Пока ни на что не влияет: в этой сборке WebKit Web Audio выключен."),
            &g_prefs.webAudio, web));
        p.rows.push_back(RHeader(Tr(L"Fonts", L"Шрифты")));
        p.rows.push_back(RSlider(Tr(L"Default font size", L"Размер шрифта по умолчанию"),
            Tr(L"For pages that do not set their own.", L"Для страниц, которые не задают свой."), 9, 32, 1,
            []() { return g_prefs.defaultFont; },
            [web](int v) { g_prefs.defaultFont = v; web(); },
            [](int v) { return std::to_wstring(v) + L" px"; }));
        p.rows.push_back(RSlider(Tr(L"Minimum font size", L"Наименьший размер шрифта"),
            Tr(L"Smaller text is enlarged. 0 turns it off.", L"Более мелкий текст увеличивается. 0 — выключено."), 0, 24, 1,
            []() { return g_prefs.minFont; },
            [web](int v) { g_prefs.minFont = v; web(); },
            [](int v) { return v == 0 ? std::wstring(Tr(L"Off", L"Выкл.")) : std::to_wstring(v) + L" px"; }));
        p.rows.push_back(RHeader(Tr(L"Developer", L"Для разработчиков")));
        p.rows.push_back(RToggle(Tr(L"Developer tools", L"Инструменты разработчика"),
            Tr(L"F12 and Inspect Element in the page menu.", L"F12 и «Просмотреть код» в меню страницы."),
            &g_prefs.devExtras, web));
        g_pages.push_back(std::move(p));
    }
    const int privacyPage = (int)g_pages.size();
    {   // Privacy
        SPage p; p.name = Tr(L"Privacy", L"Конфиденциальность"); p.icon = L"\uE72E"; p.tint = kTintGreen;
        p.rows.push_back(RHeader(Tr(L"Website data", L"Данные сайтов")));
        p.rows.push_back(RToggle(Tr(L"Local storage", L"Локальное хранилище"),
            Tr(L"Lets sites remember settings and sign-ins.", L"Сайты могут запоминать настройки и входы."),
            &g_prefs.localStorage, web));
        p.rows.push_back(RToggle(Tr(L"Web databases", L"Веб-базы данных"),
            Tr(L"Used by some web apps that work offline.", L"Нужны некоторым веб-приложениям для работы офлайн."),
            &g_prefs.databases, web));
        p.rows.push_back(RHeader(Tr(L"Ads and trackers", L"Реклама и трекеры")));
        p.rows.push_back(RToggle(Tr(L"Block ads and trackers", L"Блокировать рекламу и трекеры"),
            Tr(L"EasyList, EasyPrivacy and RU AdList, updated every few days. Turn it off for one site in the “More” menu.",
               L"EasyList, EasyPrivacy и RU AdList, обновляются раз в несколько дней. Для отдельного сайта — в меню «Ещё»."),
            &g_prefs.adBlock, [save]() { AdBlockSetEnabled(g_prefs.adBlock); save(); }));
        {
            static std::wstring status;                  // the row label must outlive the page build
            status = AdBlockStatusText();
            p.rows.push_back(RButton(Tr(L"Filter lists", L"Списки фильтров"), status.c_str(),
                Tr(L"Update", L"Обновить"), []() { AdBlockUpdateNow(); }));
        }
        p.rows.push_back(RHeader(Tr(L"Passwords", L"Пароли")));
        p.rows.push_back(RToggle(Tr(L"Offer to save passwords", L"Предлагать сохранять пароли"),
            Tr(L"Lyra remembers sign-ins and fills them in. They are encrypted with your Windows account.",
               L"Lyra запоминает входы на сайты и подставляет их. Пароли зашифрованы вашей учётной записью Windows."),
            &g_prefs.savePasswords, save));
        {
            static std::wstring saved;                   // the row label must outlive the page build
            saved = Tr(L"Saved: ", L"Сохранено: ") + std::to_wstring(PasswordsList().size());
            p.rows.push_back(RButton(Tr(L"Saved passwords", L"Сохранённые пароли"), saved.c_str(),
                Tr(L"Open", L"Открыть"), []() { SettingsOpenPage(s_passwordsPage); }));
        }
        p.rows.push_back(RHeader(Tr(L"Site permissions", L"Разрешения сайтов")));
        {
            static std::wstring count;                   // the row label must outlive the page build
            count = std::to_wstring(PermissionsCount()) + Tr(L" answers remembered (notifications, location)",
                                                             L" запомненных ответов (уведомления, местоположение)");
            p.rows.push_back(RButton(Tr(L"Notifications and location", L"Уведомления и местоположение"), count.c_str(),
                Tr(L"Reset", L"Сбросить"), []() {
                    if (MessageBoxW(g_setHwnd, Tr(L"Forget every site's answer? Sites will ask again.",
                                                  L"Забыть ответы для всех сайтов? Сайты спросят снова."),
                                    Tr(L"Lyra Settings", L"Настройки Lyra"), MB_YESNO | MB_ICONQUESTION) == IDYES)
                    {
                        PermissionsReset();
                        SettingsLanguageChanged();   // rebuilds the pages: the count above
                    }
                }));
        }
        p.rows.push_back(RHeader(Tr(L"History", L"История")));
        p.rows.push_back(RToggle(Tr(L"Remember history", L"Сохранять историю"),
            Tr(L"Visited pages, for the history page, suggestions and frequently visited sites.",
               L"Посещённые страницы — для истории, подсказок и часто посещаемых сайтов."),
            &g_prefs.recordHistory, save));
        p.rows.push_back(RButton(Tr(L"History", L"История"), Tr(L"Ctrl+H", L"Ctrl+H"), Tr(L"Open", L"Открыть"),
            []() { OpenHistory(); }));
        p.rows.push_back(RHeader(Tr(L"Clear browsing data", L"Очистка данных")));
        p.rows.push_back(RButton(Tr(L"Clear browsing data", L"Очистить данные браузера"),
            Tr(L"History, cookies and site data, cache\x2026 for a chosen period. Ctrl+Shift+Del",
               L"История, cookie и данные сайтов, кэш\x2026 за выбранный период. Ctrl+Shift+Del"),
            Tr(L"Choose\x2026", L"Выбрать\x2026"), []() { SettingsOpenPage(s_clearDataPageIndex); }));
        p.rows.push_back(RHeader(Tr(L"Note", L"Примечание")));
        p.rows.push_back(RInfo(Tr(L"Changes apply to", L"Изменения действуют для"),
                               Tr(L"pages loaded afterwards", L"страниц, открытых после них")));
        g_pages.push_back(std::move(p));
    }
    {   // Downloads
        SPage p; p.name = Tr(L"Downloads", L"Загрузки"); p.icon = L"\uE896"; p.tint = kTintBlue;
        p.rows.push_back(RHeader(Tr(L"Save files to", L"Куда сохранять")));
        static std::wstring folder;                      // the row label must outlive the page build
        folder = DownloadsFolder();
        p.rows.push_back(RButton(folder.c_str(),
            g_downloadDir.empty() ? Tr(L"The Windows Downloads folder.", L"Папка «Загрузки» Windows.")
                                  : Tr(L"Chosen in Lyra.", L"Выбрана в Lyra."),
            Tr(L"Change\x2026", L"Изменить\x2026"),
            []() { if (ChooseDownloadsFolder(g_setHwnd)) SettingsLanguageChanged(); }));
        p.rows.push_back(RButton(Tr(L"Downloads folder", L"Папка загрузок"), L"", Tr(L"Open", L"Открыть"),
            []() { OpenDownloadsFolder(); }));
        p.rows.push_back(RToggle(Tr(L"Ask where to save each file", L"Спрашивать, куда сохранять"),
            Tr(L"A Save As window opens before every download.", L"Перед каждой загрузкой открывается окно «Сохранить как»."),
            &g_prefs.askDownload, save));
        p.rows.push_back(RHeader(Tr(L"Downloads list", L"Список загрузок")));
        p.rows.push_back(RToggle(Tr(L"Show the list when a download starts", L"Показывать список, когда начинается загрузка"),
            Tr(L"Otherwise only the downloads button in the toolbar shows the progress.",
               L"Иначе прогресс видно только на кнопке загрузок в панели."),
            &g_prefs.downloadsPopup, save));
        p.rows.push_back(RButton(Tr(L"All downloads", L"Все загрузки"), L"Ctrl+J", Tr(L"Open", L"Открыть"),
            []() { OpenDownloads(); }));
        g_pages.push_back(std::move(p));
    }
    {   // Keyboard
        SPage p; p.name = Tr(L"Keyboard", L"Клавиатура"); p.icon = L"\uE765"; p.tint = kTintTeal;
        p.rows.push_back(RHeader(Tr(L"Tabs", L"Вкладки")));
        p.rows.push_back(RInfo(Tr(L"New tab", L"Новая вкладка"), L"Ctrl+T", true));
        p.rows.push_back(RInfo(Tr(L"Close tab", L"Закрыть вкладку"), L"Ctrl+W", true));
        p.rows.push_back(RInfo(Tr(L"Next tab", L"Следующая вкладка"), L"Ctrl+Tab", true));
        p.rows.push_back(RInfo(Tr(L"Previous tab", L"Предыдущая вкладка"), L"Ctrl+Shift+Tab", true));
        p.rows.push_back(RInfo(Tr(L"Go to tab 1-8", L"Перейти к вкладке 1–8"), L"Ctrl+1 … 8", true));
        p.rows.push_back(RInfo(Tr(L"Go to last tab", L"Последняя вкладка"), L"Ctrl+9", true));
        p.rows.push_back(RInfo(Tr(L"Reopen closed tab", L"Вернуть закрытую вкладку"), L"Ctrl+Shift+T", true));
        p.rows.push_back(RInfo(Tr(L"Command palette", L"Палитра команд"), L"Ctrl+K", true));
        p.rows.push_back(RInfo(Tr(L"History", L"История"), L"Ctrl+H", true));
        p.rows.push_back(RInfo(Tr(L"Bookmarks", L"Закладки"), L"Ctrl+Shift+O", true));
        p.rows.push_back(RInfo(Tr(L"Bookmark this page", L"Добавить в закладки"), L"Ctrl+D", true));
        p.rows.push_back(RInfo(Tr(L"Downloads", L"Загрузки"), L"Ctrl+J", true));
        p.rows.push_back(RInfo(Tr(L"Clear browsing data", L"Очистить данные браузера"), L"Ctrl+Shift+Del", true));
        p.rows.push_back(RHeader(Tr(L"Navigation", L"Навигация")));
        p.rows.push_back(RInfo(Tr(L"Address bar", L"Адресная строка"), L"Ctrl+L", true));
        p.rows.push_back(RInfo(Tr(L"Reload", L"Обновить"), L"Ctrl+R  /  F5", true));
        p.rows.push_back(RInfo(Tr(L"Back", L"Назад"), L"Alt+←", true));
        p.rows.push_back(RInfo(Tr(L"Forward", L"Вперёд"), L"Alt+→", true));
        p.rows.push_back(RHeader(Tr(L"View", L"Вид")));
        p.rows.push_back(RInfo(Tr(L"Zoom in", L"Увеличить"), L"Ctrl++", true));
        p.rows.push_back(RInfo(Tr(L"Zoom out", L"Уменьшить"), L"Ctrl+-", true));
        p.rows.push_back(RInfo(Tr(L"Actual size", L"Исходный размер"), L"Ctrl+0", true));
        p.rows.push_back(RHeader(Tr(L"Browser", L"Браузер")));
        p.rows.push_back(RInfo(Tr(L"Settings", L"Настройки"), L"Ctrl+,", true));
        p.rows.push_back(RInfo(Tr(L"Developer tools", L"Инструменты разработчика"), L"F12  /  Ctrl+Shift+I", true));
        g_pages.push_back(std::move(p));
    }
    {   // About
        SPage p; p.name = Tr(L"About", L"О программе"); p.icon = L"\uE946"; p.tint = kTintGraphite;
        p.rows.push_back(RHeader(kAppName));
        p.rows.push_back(RInfo(Tr(L"Engine", L"Движок"), L"WebKit"));
        p.rows.push_back(RInfo(Tr(L"Version", L"Версия"), LYRA_VERSION_WSTR));
        p.rows.push_back(RInfo(Tr(L"Settings file", L"Файл настроек"), g_settingsFile));
        p.rows.push_back(RButton(Tr(L"Settings folder", L"Папка настроек"), L"", Tr(L"Open", L"Открыть"),
            []() { ShellExecuteW(nullptr, L"open", g_settingsDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }));
        p.rows.push_back(RHeader(Tr(L"Reset", L"Сброс")));
        p.rows.push_back(RButton(Tr(L"Reset all settings", L"Сбросить все настройки"),
            Tr(L"Every option goes back to its default.", L"Все параметры вернутся к значениям по умолчанию."),
            Tr(L"Reset…", L"Сбросить…"),
            []() {
                if (MessageBoxW(g_setHwnd, Tr(L"Reset all settings to their defaults?", L"Сбросить все настройки?"),
                                Tr(L"Lyra Settings", L"Настройки Lyra"), MB_YESNO | MB_ICONQUESTION) == IDYES)
                    ResetAllSettings();
            }));
        g_pages.push_back(std::move(p));
    }

    // ---- detail pages (not in the sidebar) ----------------------------------
    {   // Animations, from the gear next to Appearance > Animations
        SPage p; p.name = Tr(L"Animations", L"Анимации"); p.icon = L"\uE945"; p.tint = kTintPurple;
        p.parent = appearancePage;
        p.rows.push_back(RHeader(Tr(L"General", L"Общие")));
        p.rows.push_back(RToggle(Tr(L"Animations", L"Анимации"),
            Tr(L"Off makes every change instant.", L"Если выключить, всё меняется мгновенно."), &g_prefs.animations, repaint));
        p.rows.push_back(RSlider(Tr(L"Speed", L"Скорость"),
            Tr(L"100% is the default; higher is faster.", L"100% — по умолчанию; больше — быстрее."), 25, 300, 5,
            []() { return g_prefs.animSpeed; },
            [save](int v) { g_prefs.animSpeed = v; save(); },
            [](int v) { return std::to_wstring(v) + L"%"; }));
        p.rows.push_back(RHeader(Tr(L"What moves", L"Что анимируется")));
        p.rows.push_back(RToggle(Tr(L"Tabs", L"Вкладки"),
            Tr(L"Tabs slide and grow when opened, closed or switched.", L"Вкладки плавно сдвигаются и растут при открытии, закрытии и переключении."),
            &g_prefs.animTabs, repaint));
        p.rows.push_back(RToggle(Tr(L"Hover highlights", L"Подсветка при наведении"),
            Tr(L"Buttons and tabs fade their highlight in and out.", L"Подсветка кнопок и вкладок плавно появляется и гаснет."),
            &g_prefs.animHover, repaint));
        p.rows.push_back(RHeader(Tr(L"Closing tabs", L"Закрытие вкладок")));
        p.rows.push_back(RSlider(Tr(L"Resize delay", L"Задержка перед расширением"),
            Tr(L"How long tabs keep their width after the pointer leaves the tab bar.",
               L"Сколько вкладки ждут, когда курсор ушёл с полосы вкладок."),
            0, 2000, 50,
            []() { return g_prefs.tabResizeDelay; },
            [save](int v) { g_prefs.tabResizeDelay = v; save(); },
            [](int v) { return v == 0 ? std::wstring(Tr(L"Off", L"Выкл.")) : std::to_wstring(v) + Tr(L" ms", L" мс"); }));
        s_animationsPage = (int)g_pages.size();
        g_pages.push_back(std::move(p));
    }
    {   // Saved passwords, from Privacy > Passwords
        SPage p; p.name = Tr(L"Saved passwords", L"Сохранённые пароли"); p.icon = L"\uE8D7"; p.tint = kTintGreen;
        p.parent = privacyPage;
        const std::vector<SavedPassword> list = PasswordsList();
        p.rows.push_back(RHeader(Tr(L"Sites", L"Сайты")));
        if (list.empty())
            p.rows.push_back(RInfo(Tr(L"No saved passwords yet", L"Сохранённых паролей пока нет"), L""));
        for (const SavedPassword& e : list) {
            const std::wstring site = PasswordsSiteName(e.origin);
            const std::wstring user = e.username.empty() ? std::wstring(Tr(L"(no username)", L"(без имени)")) : e.username;
            const std::string origin = e.origin;
            const std::wstring username = e.username;
            p.rows.push_back(RButton(site.c_str(), user.c_str(), Tr(L"Actions\x2026", L"Действия\x2026"),
                [origin, username]() { PasswordActions(origin, username); }));
        }
        const std::vector<std::string> never = PasswordsNeverList();
        if (!never.empty()) {
            p.rows.push_back(RHeader(Tr(L"Never saved for", L"Не сохранять для")));
            for (const std::string& o : never) {
                const std::wstring site = PasswordsSiteName(o);
                const std::string origin = o;
                p.rows.push_back(RButton(site.c_str(), L"", Tr(L"Remove", L"Убрать"),
                    [origin]() { PasswordsRemoveNever(origin); SettingsLanguageChanged(); }));
            }
        }
        s_passwordsPage = (int)g_pages.size();
        s_passwordsPageIndex = s_passwordsPage;
        g_pages.push_back(std::move(p));
    }
    {   // Clear browsing data, from Privacy
        SPage p; p.name = Tr(L"Clear browsing data", L"Очистка данных"); p.icon = L"\uE74D"; p.tint = kTintGreen;
        p.parent = privacyPage;
        const std::function<void()> none = []() {};
        p.rows.push_back(RHeader(Tr(L"Period", L"Период")));
        p.rows.push_back(RChoice(Tr(L"Time range", L"Временной диапазон"),
            Tr(L"Data stored or changed in this period.", L"Данные, сохранённые или изменённые за это время."),
            ClearDataRanges(), []() { return s_clearChoice.range; }, [](int v) { s_clearChoice.range = v; }));
        p.rows.push_back(RHeader(Tr(L"What to clear", L"Что удалить")));
        p.rows.push_back(RToggle(Tr(L"Browsing history", L"История посещений"),
            Tr(L"Visited pages, address bar suggestions and recently closed tabs.",
               L"Посещённые страницы, подсказки адресной строки и недавно закрытые вкладки."),
            &s_clearChoice.history, none));
        p.rows.push_back(RToggle(Tr(L"Cookies and site data", L"Cookie и данные сайтов"),
            Tr(L"Signs you out of most sites.", L"Вы выйдете из большинства сайтов."),
            &s_clearChoice.cookies, none));
        p.rows.push_back(RToggle(Tr(L"Cached images and files", L"Изображения и файлы в кэше"),
            Tr(L"Frees space; sites load a little slower the first time.",
               L"Освобождает место; сайты в первый раз загрузятся чуть дольше."),
            &s_clearChoice.cache, none));
        p.rows.push_back(RToggle(Tr(L"Downloads list", L"Список загрузок"),
            Tr(L"The downloaded files stay on disk.", L"Сами файлы остаются на диске."),
            &s_clearChoice.downloads, none));
        p.rows.push_back(RToggle(Tr(L"Site permissions", L"Разрешения сайтов"),
            Tr(L"Notifications and location: sites will ask again.", L"Уведомления и местоположение: сайты спросят снова."),
            &s_clearChoice.permissions, none));
        p.rows.push_back(RToggle(Tr(L"Saved passwords", L"Сохранённые пароли"),
            Tr(L"Used in this period. They cannot be restored.", L"Использованные за этот период. Вернуть их нельзя."),
            &s_clearChoice.passwords, none));
        p.rows.push_back(RHeader(L""));
        p.rows.push_back(RButton(Tr(L"Clear the chosen data", L"Удалить выбранные данные"), s_clearStatus.c_str(),
            Tr(L"Clear", L"Удалить"), []() { ClearChosenData(); }));
        s_clearDataPageIndex = (int)g_pages.size();
        g_pages.push_back(std::move(p));
    }
}
