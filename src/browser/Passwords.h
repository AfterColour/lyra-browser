// Lyra — менеджер паролей: предложение сохранить вход, автозаполнение, хранение (шифрование Windows).
#pragma once

#include "common/Platform.h"

// A script in every page notices a sign-in (a form sent, Enter or a button with a password typed)
// and asks Lyra; the bubble under the address field offers to save or update the password. On a
// page with a password field the saved sign-in for that site is filled in. Passwords are kept in
// passwords.dat, encrypted for the current Windows user (DPAPI): another account or computer
// cannot read the file. A page only ever gets the passwords of its own origin.

struct PasswordAsk {
    unsigned long long seq = 0;
    unsigned long long tabId = 0;
    bool update = false;             // a saved sign-in with another password
    std::string origin;
    std::wstring site, username;
};
const PasswordAsk* PasswordsPendingForTab(unsigned long long tabId);
// 1 save / update; 0 never for this site (save) or not now (update); -1 not now
void PasswordsAnswer(unsigned long long seq, int answer);

struct SavedPassword { std::string origin; std::wstring username, password; long long lastUsed = 0; };
std::vector<SavedPassword> PasswordsList();          // by site
std::vector<std::string> PasswordsNeverList();
void PasswordsDelete(const std::string& origin, const std::wstring& username);
void PasswordsRemoveNever(const std::string& origin);
std::wstring PasswordsSiteName(const std::string& origin);

size_t PasswordsCountForUrl(const std::wstring& url);   // sign-ins saved for the page's site (the key in the address field)
void PasswordsSiteMenu();            // the key: copy a saved sign-in, open the list

void PasswordsInit();                // after AdBlockInit (the shared user content controller)
void PasswordsShutdown();
