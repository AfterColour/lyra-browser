// Lyra — подсказки под адресной строкой: поиск и совпадения из истории.
#pragma once

#include "common/Platform.h"

// The address field's text changed (typing): show / update / hide the list.
void SuggestUpdate();
void SuggestHide();
bool SuggestVisible();
// Up / Down in the address field: move the selection (the field shows the selected address).
// Returns false when there is no list.
bool SuggestMove(int delta);
// Enter in the address field: opens the selected suggestion; false = nothing selected.
bool SuggestAccept();
// Escape: closes the list and puts back what was typed; false = there was no list.
bool SuggestCancel();
