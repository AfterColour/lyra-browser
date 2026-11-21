// Lyra - passkeys and security keys (WebAuthn) through Windows.
#pragma once

#include "common/Platform.h"

// Pages get the standard Web Authentication API (navigator.credentials.create/get with publicKey,
// PublicKeyCredential and its responses) from a script Lyra adds to every page; the requests go
// to Windows' own WebAuthn service (webauthn.dll), which shows its window over Lyra: Windows
// Hello (face, fingerprint, PIN), a security key, or a phone nearby. Lyra checks each request
// against the frame's real origin (never what the page says) and writes the client data itself.

void WebAuthnInit();                 // startup, after the user content controller exists
void WebAuthnShutdown();             // an open Windows prompt is cancelled
void WebAuthnDone(LPARAM result);    // main window: WM_APP_WEBAUTHN (a request has finished)
