// Lyra — разбор адресов: что ввёл пользователь -> URL или поиск, отображение адреса.
#pragma once

#include "common/Platform.h"

std::wstring HostOf(const std::wstring& url);
std::wstring DisplayUrl(const std::wstring& url);
std::string ResolveInput(const std::wstring& input);
