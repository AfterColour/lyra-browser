// Lyra — перевод страниц: Google (без ключа) или DeepL (свой ключ API).
#pragma once

#include "common/Platform.h"

struct Tab;

// When a page is in another language than the one chosen in Settings, a "translate" icon appears in
// the address field. A click translates the page in place: the text of the page is sent in batches
// to the chosen service and written back over the original (links, layout and scripts stay); text
// that appears later is translated as it comes. Another click shows the original. Pages opened from
// a translated one in the same tab are translated too, until the original is asked for.
void TranslateInit();
void TranslatePageCommitted(Tab& t);      // didCommit: a new page, nothing of the old one applies
void TranslatePageLoaded(Tab& t);         // didFinish: offer (icon), or translate again
void TranslateToggle();                   // the icon / the "More" menu, for the active tab
void TranslateTick();                     // main timer: text that appeared on translated pages
void TranslateResult(LPARAM lParam);      // WM_APP_TRANSLATE (a worker finished a batch)

// Settings > General > Translation
struct TranslateLanguage { const char* google; const char* deepl; const wchar_t* name; };
const std::vector<TranslateLanguage>& TranslateLanguages();   // [0] = Lyra's own language
std::wstring TranslateTargetName();
std::wstring TranslateGetDeepLKey();
void TranslateSetDeepLKey(const std::wstring& key);
