// Lyra — окошко запроса разрешения под адресной строкой («Сайт запрашивает: показывать уведомления»).
#pragma once

#include "common/Platform.h"

// A bubble under the left end of the address field, as in other browsers: the site, what it asks
// for, Block / Allow, and × (not now). It does not take the focus, follows the active tab (each
// tab keeps its own questions) and moves with the window. Answers go to Permissions.cpp.
void PermissionPromptUpdate();       // main timer and whenever the questions change
void PermissionPromptClose();
