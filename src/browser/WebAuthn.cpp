// Lyra - passkeys and security keys (WebAuthn) through Windows' webauthn.dll.
//
// The page side is a script (kScript, below) that defines the standard objects and posts each
// request as JSON to the "lyraWebAuthn" message handler; the reply is the result as JSON. This
// side is the browser's part of the specification:
//   - the origin is the requesting frame's own (WKFrameInfo), only secure ones (https, or http
//     on localhost), only the main frame or a frame of the same origin, only in the active tab;
//   - the relying party ID must be the origin's host or a parent domain of it that is not a
//     public suffix (libpsl's list, psl-5.dll beside Lyra): co.uk can never be claimed;
//   - Lyra writes the client data (type, challenge, origin) itself; Windows hashes and signs it.
// The Windows call blocks while its window is up, so it runs on a thread of its own; the result
// comes back to the main window (WM_APP_WEBAUTHN). One request at a time, as in other browsers;
// a page's AbortSignal cancels it (WebAuthNCancelCurrentOperation).
#include "browser/WebAuthn.h"
#include "app/Log.h"
#include "browser/AdBlock.h"
#include "browser/Tab.h"
#include "browser/TabManager.h"
#include "core/Constants.h"
#include "core/Json.h"
#include "core/StringUtils.h"
#include "ui/MainWindow.h"

#include <WebKit/WKFrameInfoRef.h>
#include <WebKit/WKPageFullScreenClient.h>      // WKCompletionListenerComplete
#include <WebKit/WKScriptMessageRef.h>
#include <WebKit/WKUserContentControllerRef.h>

#include <webauthn.h>
#include <atomic>
#include <mutex>
#include <thread>

namespace {

const char kScript[] =
R"JS((function () {
'use strict';
if (!window.isSecureContext || window.PublicKeyCredential) return;
var mh = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lyraWebAuthn;
if (!mh) return;
var post = mh.postMessage.bind(mh);
var KEY = {};
var priv = new WeakMap();
var B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_';
function b64e(u8) {
  var s = '', i = 0;
  for (; i + 2 < u8.length; i += 3) {
    var n = (u8[i] << 16) | (u8[i + 1] << 8) | u8[i + 2];
    s += B64[n >> 18 & 63] + B64[n >> 12 & 63] + B64[n >> 6 & 63] + B64[n & 63];
  }
  if (i < u8.length) {
    var m = u8[i] << 16 | (i + 1 < u8.length ? u8[i + 1] << 8 : 0);
    s += B64[m >> 18 & 63] + B64[m >> 12 & 63];
    if (i + 1 < u8.length) s += B64[m >> 6 & 63];
  }
  return s;
}
function b64d(s) {
  if (typeof s !== 'string') throw new TypeError('Expected a base64url string');
  s = s.replace(/=+$/, '').replace(/\+/g, '-').replace(/\//g, '_');
  var out = new Uint8Array(Math.floor(s.length * 3 / 4)), o = 0, acc = 0, bits = 0;
  for (var i = 0; i < s.length; i++) {
    var v = B64.indexOf(s[i]);
    if (v < 0) throw new TypeError('Invalid base64url string');
    acc = (acc << 6) | v; bits += 6;
    if (bits >= 8) { bits -= 8; out[o++] = (acc >> bits) & 255; }
  }
  return out.slice(0, o).buffer;
}
function buf(v, what) {
  if (v instanceof ArrayBuffer) return new Uint8Array(v.slice(0));
  if (ArrayBuffer.isView(v)) return new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength));
  throw new TypeError((what || 'Value') + ' must be a BufferSource');
}
function enc(v, what) { return b64e(buf(v, what)); }
function err(name, message) { return name === 'TypeError' ? new TypeError(message) : new DOMException(message || name, name); }
function call(req) {
  return post(JSON.stringify(req)).then(function (t) {
    var r = typeof t === 'string' ? JSON.parse(t) : t;
    if (!r || !r.ok) throw err(r && r.name || 'NotAllowedError', r && r.message);
    return r;
  });
}

// --- CBOR, just enough for a COSE public key in authenticator data -----------------------------
function cbor(u8, pos) {
  var p = pos.p, ib = u8[p++], mt = ib >> 5, ai = ib & 31, len = ai;
  if (ai === 24) len = u8[p++];
  else if (ai === 25) { len = u8[p] << 8 | u8[p + 1]; p += 2; }
  else if (ai === 26) { len = ((u8[p] << 24) >>> 0) + (u8[p + 1] << 16 | u8[p + 2] << 8 | u8[p + 3]); p += 4; }
  else if (ai > 26) throw new Error('cbor');
  pos.p = p;
  if (mt === 0) return len;
  if (mt === 1) return -1 - len;
  if (mt === 2 || mt === 3) { var b = u8.slice(p, p + len); pos.p = p + len; return mt === 2 ? b : new TextDecoder().decode(b); }
  if (mt === 4) { var a = []; for (var i = 0; i < len; i++) a.push(cbor(u8, pos)); return a; }
  if (mt === 5) { var m = new Map(); for (var j = 0; j < len; j++) { var k = cbor(u8, pos); m.set(k, cbor(u8, pos)); } return m; }
  if (mt === 7) return ai === 20 ? false : ai === 21 ? true : null;
  throw new Error('cbor');
}
function coseKey(authData) {
  var u8 = new Uint8Array(authData);
  if (u8.length < 55 || !(u8[32] & 0x40)) return null;
  var idLen = u8[53] << 8 | u8[54], pos = { p: 55 + idLen };
  try { return cbor(u8, pos); } catch (e) { return null; }
}
function derLen(n) { return n < 128 ? [n] : n < 256 ? [0x81, n] : [0x82, n >> 8, n & 255]; }
function der(tag, bytes) { return [tag].concat(derLen(bytes.length), bytes); }
function derInt(b) { var a = Array.from(b); while (a.length > 1 && a[0] === 0 && !(a[1] & 0x80)) a.shift(); if (a[0] & 0x80) a.unshift(0); return der(2, a); }
function hex(h) { var a = []; for (var i = 0; i < h.length; i += 2) a.push(parseInt(h.substr(i, 2), 16)); return a; }
function spki(key) {
  if (!key) return null;
  var kty = key.get(1);
  if (kty === 2 && key.get(-1) === 1) return new Uint8Array(hex('3059301306072a8648ce3d020106082a8648ce3d030107034200').concat([4], Array.from(key.get(-2)), Array.from(key.get(-3)))).buffer;
  if (kty === 1 && key.get(-1) === 6) return new Uint8Array(hex('302a300506032b6570032100').concat(Array.from(key.get(-2)))).buffer;
  if (kty === 3) {
    var rsa = der(0x30, derInt(key.get(-1)).concat(derInt(key.get(-2))));
    var alg = der(0x30, hex('06092a864886f70d0101010500'));
    return new Uint8Array(der(0x30, alg.concat(der(3, [0].concat(rsa))))).buffer;
  }
  return null;
}

// --- the API objects ---------------------------------------------------------------------------
function Credential() { throw new TypeError('Illegal constructor'); }
Object.defineProperties(Credential.prototype, {
  id: { get: function () { return priv.get(this).id; }, enumerable: true, configurable: true },
  type: { get: function () { return priv.get(this).type; }, enumerable: true, configurable: true }
});
function AuthenticatorResponse() { throw new TypeError('Illegal constructor'); }
Object.defineProperty(AuthenticatorResponse.prototype, 'clientDataJSON', { get: function () { return priv.get(this).clientDataJSON.slice(0); }, enumerable: true, configurable: true });
function AuthenticatorAttestationResponse() { throw new TypeError('Illegal constructor'); }
AuthenticatorAttestationResponse.prototype = Object.create(AuthenticatorResponse.prototype, { constructor: { value: AuthenticatorAttestationResponse, configurable: true, writable: true } });
Object.defineProperty(AuthenticatorAttestationResponse.prototype, 'attestationObject', { get: function () { return priv.get(this).attestationObject.slice(0); }, enumerable: true, configurable: true });
AuthenticatorAttestationResponse.prototype.getTransports = function () { return priv.get(this).transports.slice(); };
AuthenticatorAttestationResponse.prototype.getAuthenticatorData = function () { return priv.get(this).authenticatorData.slice(0); };
AuthenticatorAttestationResponse.prototype.getPublicKey = function () { var p = priv.get(this); return p.publicKey ? p.publicKey.slice(0) : null; };
AuthenticatorAttestationResponse.prototype.getPublicKeyAlgorithm = function () { return priv.get(this).alg; };
function AuthenticatorAssertionResponse() { throw new TypeError('Illegal constructor'); }
AuthenticatorAssertionResponse.prototype = Object.create(AuthenticatorResponse.prototype, { constructor: { value: AuthenticatorAssertionResponse, configurable: true, writable: true } });
['authenticatorData', 'signature'].forEach(function (k) {
  Object.defineProperty(AuthenticatorAssertionResponse.prototype, k, { get: function () { return priv.get(this)[k].slice(0); }, enumerable: true, configurable: true });
});
Object.defineProperty(AuthenticatorAssertionResponse.prototype, 'userHandle', { get: function () { var u = priv.get(this).userHandle; return u ? u.slice(0) : null; }, enumerable: true, configurable: true });

function PublicKeyCredential() { throw new TypeError('Illegal constructor'); }
PublicKeyCredential.prototype = Object.create(Credential.prototype, { constructor: { value: PublicKeyCredential, configurable: true, writable: true } });
Object.defineProperties(PublicKeyCredential.prototype, {
  rawId: { get: function () { return priv.get(this).rawId.slice(0); }, enumerable: true, configurable: true },
  response: { get: function () { return priv.get(this).response; }, enumerable: true, configurable: true },
  authenticatorAttachment: { get: function () { return priv.get(this).attachment; }, enumerable: true, configurable: true }
});
PublicKeyCredential.prototype.getClientExtensionResults = function () { return JSON.parse(JSON.stringify(priv.get(this).ext || {})); };
PublicKeyCredential.prototype.toJSON = function () {
  var p = priv.get(this), r = priv.get(p.response), out = { id: p.id, rawId: p.id, type: 'public-key', authenticatorAttachment: p.attachment, clientExtensionResults: p.ext || {} };
  var e = function (b) { return b64e(new Uint8Array(b)); };
  if (p.kind === 'create') {
    out.response = { clientDataJSON: e(r.clientDataJSON), attestationObject: e(r.attestationObject), authenticatorData: e(r.authenticatorData), transports: r.transports.slice(), publicKeyAlgorithm: r.alg };
    if (r.publicKey) out.response.publicKey = e(r.publicKey);
  } else {
    out.response = { clientDataJSON: e(r.clientDataJSON), authenticatorData: e(r.authenticatorData), signature: e(r.signature) };
    if (r.userHandle) out.response.userHandle = e(r.userHandle);
  }
  return out;
};
var uvpaa = null;
PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable = function () {
  if (!uvpaa) uvpaa = call({ op: 'uvpaa' }).then(function (r) { return !!r.available; }, function () { return false; });
  return uvpaa;
};
PublicKeyCredential.isConditionalMediationAvailable = function () { return Promise.resolve(false); };
PublicKeyCredential.getClientCapabilities = function () {
  return PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable().then(function (uv) {
    return { conditionalCreate: false, conditionalGet: false, hybridTransport: true, passkeyPlatformAuthenticator: uv,
             userVerifyingPlatformAuthenticator: uv, relatedOrigins: false, signalAllAcceptedCredentials: false,
             signalCurrentUserDetails: false, signalUnknownCredential: false, 'extension:credProps': true };
  });
};
['signalUnknownCredential', 'signalAllAcceptedCredentials', 'signalCurrentUserDetails'].forEach(function (k) {
  PublicKeyCredential[k] = function () { return Promise.resolve(); };
});
function credsFromJSON(list) { return (list || []).map(function (c) { var o = Object.assign({}, c); o.id = b64d(c.id); return o; }); }
PublicKeyCredential.parseCreationOptionsFromJSON = function (j) {
  var o = Object.assign({}, j);
  o.challenge = b64d(j.challenge);
  o.user = Object.assign({}, j.user, { id: b64d(j.user.id) });
  if (j.excludeCredentials) o.excludeCredentials = credsFromJSON(j.excludeCredentials);
  return o;
};
PublicKeyCredential.parseRequestOptionsFromJSON = function (j) {
  var o = Object.assign({}, j);
  o.challenge = b64d(j.challenge);
  if (j.allowCredentials) o.allowCredentials = credsFromJSON(j.allowCredentials);
  return o;
};

function makeCredential(r, kind) {
  var c = Object.create(PublicKeyCredential.prototype), resp, rp;
  if (kind === 'create') {
    resp = Object.create(AuthenticatorAttestationResponse.prototype);
    var authData = b64d(r.authenticatorData), key = coseKey(authData);
    rp = { clientDataJSON: b64d(r.clientDataJSON), attestationObject: b64d(r.attestationObject), authenticatorData: authData,
           transports: r.transports || [], publicKey: spki(key), alg: key ? key.get(3) : -7 };
  } else {
    resp = Object.create(AuthenticatorAssertionResponse.prototype);
    rp = { clientDataJSON: b64d(r.clientDataJSON), authenticatorData: b64d(r.authenticatorData), signature: b64d(r.signature),
           userHandle: r.userHandle ? b64d(r.userHandle) : null };
  }
  priv.set(resp, rp);
  priv.set(c, { id: r.id, type: 'public-key', rawId: b64d(r.id), response: resp, attachment: r.attachment || null, ext: r.ext || {}, kind: kind });
  return c;
}

function creds(list, what) {
  if (list === undefined) return undefined;
  return Array.from(list).map(function (c) { return { type: String(c.type || 'public-key'), id: enc(c.id, what), transports: c.transports ? Array.from(c.transports).map(String) : [] }; });
}
var seq = 0;
function run(op, options, signal) {
  if (signal && signal.aborted) return Promise.reject(signal.reason !== undefined ? signal.reason : err('AbortError'));
  var id = ++seq;
  return new Promise(function (resolve, reject) {
    var onAbort = function () { post(JSON.stringify({ op: 'cancel', id: id })); reject(signal.reason !== undefined ? signal.reason : err('AbortError')); };
    if (signal) signal.addEventListener('abort', onAbort, { once: true });
    call({ op: op, id: id, options: options }).then(function (r) {
      if (signal) signal.removeEventListener('abort', onAbort);
      resolve(makeCredential(r, op));
    }, function (e) { if (signal) signal.removeEventListener('abort', onAbort); reject(e); });
  });
}

)JS"
R"JS(function CredentialsContainer() { throw new TypeError('Illegal constructor'); }
CredentialsContainer.prototype.create = function (options) {
  try {
    options = options || {};
    var pk = options.publicKey;
    if (!pk) return Promise.reject(err('NotSupportedError', 'Only public key credentials are supported.'));
    if (!pk.rp || !pk.user || pk.challenge === undefined || !pk.pubKeyCredParams) throw new TypeError('Missing required members of PublicKeyCredentialCreationOptions');
    var sel = pk.authenticatorSelection || {}, ext = pk.extensions || {};
    var o = {
      rp: { id: pk.rp.id !== undefined ? String(pk.rp.id) : undefined, name: String(pk.rp.name || '') },
      user: { id: enc(pk.user.id, 'user.id'), name: String(pk.user.name || ''), displayName: String(pk.user.displayName || '') },
      challenge: enc(pk.challenge, 'challenge'),
      pubKeyCredParams: Array.from(pk.pubKeyCredParams).map(function (p) { return { type: String(p.type), alg: Number(p.alg) }; }),
      timeout: pk.timeout !== undefined ? Number(pk.timeout) : undefined,
      excludeCredentials: creds(pk.excludeCredentials || [], 'excludeCredentials.id'),
      authenticatorSelection: { authenticatorAttachment: sel.authenticatorAttachment, residentKey: sel.residentKey,
                                requireResidentKey: !!sel.requireResidentKey, userVerification: sel.userVerification },
      attestation: pk.attestation, hints: pk.hints ? Array.from(pk.hints).map(String) : [],
      extensions: { credProps: !!ext.credProps }
    };
    var ul = new Uint8Array(b64d(o.user.id)).length;
    if (ul < 1 || ul > 64) throw new TypeError('user.id must be 1 to 64 bytes long');
    return run('create', o, options.signal);
  } catch (e) { return Promise.reject(e); }
};
CredentialsContainer.prototype.get = function (options) {
  try {
    options = options || {};
    var pk = options.publicKey;
    if (!pk) return Promise.resolve(null);
    if (pk.challenge === undefined) throw new TypeError('Missing challenge');
    if (options.mediation === 'conditional') {        // no passkey autofill: waits until aborted
      return new Promise(function (resolve, reject) {
        if (options.signal) options.signal.addEventListener('abort', function () { reject(options.signal.reason !== undefined ? options.signal.reason : err('AbortError')); }, { once: true });
      });
    }
    var ext = pk.extensions || {};
    var o = {
      challenge: enc(pk.challenge, 'challenge'),
      timeout: pk.timeout !== undefined ? Number(pk.timeout) : undefined,
      rpId: pk.rpId !== undefined ? String(pk.rpId) : undefined,
      allowCredentials: creds(pk.allowCredentials || [], 'allowCredentials.id'),
      userVerification: pk.userVerification, hints: pk.hints ? Array.from(pk.hints).map(String) : [],
      extensions: { appid: ext.appid !== undefined ? String(ext.appid) : undefined }
    };
    return run('get', o, options.signal);
  } catch (e) { return Promise.reject(e); }
};
CredentialsContainer.prototype.store = function () { return Promise.reject(err('NotSupportedError', 'Storing credentials is not supported.')); };
CredentialsContainer.prototype.preventSilentAccess = function () { return Promise.resolve(); };
var container = Object.create(CredentialsContainer.prototype);

function expose(name, value) { Object.defineProperty(window, name, { value: value, writable: true, configurable: true }); }
expose('Credential', Credential);
expose('PublicKeyCredential', PublicKeyCredential);
expose('AuthenticatorResponse', AuthenticatorResponse);
expose('AuthenticatorAttestationResponse', AuthenticatorAttestationResponse);
expose('AuthenticatorAssertionResponse', AuthenticatorAssertionResponse);
expose('CredentialsContainer', CredentialsContainer);
Object.defineProperty(Navigator.prototype, 'credentials', { get: function () { return container; }, enumerable: true, configurable: true });
})();

)JS";

// --- webauthn.dll, loaded on first use --------------------------------------------------------
struct Api {
    bool loaded = false, ok = false;
    DWORD version = 0;
    decltype(&WebAuthNGetApiVersionNumber) getVersion = nullptr;
    decltype(&WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable) uvpaa = nullptr;
    decltype(&WebAuthNAuthenticatorMakeCredential) makeCredential = nullptr;
    decltype(&WebAuthNAuthenticatorGetAssertion) getAssertion = nullptr;
    decltype(&WebAuthNFreeCredentialAttestation) freeAttestation = nullptr;
    decltype(&WebAuthNFreeAssertion) freeAssertion = nullptr;
    decltype(&WebAuthNGetCancellationId) getCancellationId = nullptr;
    decltype(&WebAuthNCancelCurrentOperation) cancel = nullptr;
    decltype(&WebAuthNGetErrorName) errorName = nullptr;
};
Api s_api;

bool LoadApi() {
    if (s_api.loaded) return s_api.ok;
    s_api.loaded = true;
    HMODULE m = LoadLibraryExW(L"webauthn.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!m) { Log("webauthn: webauthn.dll not found (Windows 10 1903 or later needed)"); return false; }
    auto get = [m](const char* name) { return GetProcAddress(m, name); };
    s_api.getVersion = (decltype(s_api.getVersion))get("WebAuthNGetApiVersionNumber");
    s_api.uvpaa = (decltype(s_api.uvpaa))get("WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable");
    s_api.makeCredential = (decltype(s_api.makeCredential))get("WebAuthNAuthenticatorMakeCredential");
    s_api.getAssertion = (decltype(s_api.getAssertion))get("WebAuthNAuthenticatorGetAssertion");
    s_api.freeAttestation = (decltype(s_api.freeAttestation))get("WebAuthNFreeCredentialAttestation");
    s_api.freeAssertion = (decltype(s_api.freeAssertion))get("WebAuthNFreeAssertion");
    s_api.getCancellationId = (decltype(s_api.getCancellationId))get("WebAuthNGetCancellationId");
    s_api.cancel = (decltype(s_api.cancel))get("WebAuthNCancelCurrentOperation");
    s_api.errorName = (decltype(s_api.errorName))get("WebAuthNGetErrorName");
    s_api.ok = s_api.getVersion && s_api.uvpaa && s_api.makeCredential && s_api.getAssertion && s_api.freeAttestation && s_api.freeAssertion;
    if (s_api.ok) s_api.version = s_api.getVersion();
    Log("webauthn: Windows WebAuthn API version %lu%s", (unsigned long)s_api.version, s_api.ok ? "" : " (incomplete: disabled)");
    return s_api.ok;
}

// --- the public suffix list (libpsl) ----------------------------------------------------------
struct psl_ctx_st;
using PslBuiltin = const psl_ctx_st* (*)();
using PslLatest = psl_ctx_st* (*)(const char*);
using PslIsPublicSuffix = int (*)(const psl_ctx_st*, const char*);
const psl_ctx_st* s_psl = nullptr;
PslIsPublicSuffix s_pslIsPublic = nullptr;
bool s_pslTried = false;

bool IsPublicSuffix(const std::string& domain) {
    if (!s_pslTried) {
        s_pslTried = true;
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
        if (HMODULE m = LoadLibraryW((dir + L"psl-5.dll").c_str())) {
            auto builtin = (PslBuiltin)GetProcAddress(m, "psl_builtin");
            auto latest = (PslLatest)GetProcAddress(m, "psl_latest");
            s_pslIsPublic = (PslIsPublicSuffix)GetProcAddress(m, "psl_is_public_suffix");
            if (builtin) s_psl = builtin();
            if (!s_psl && latest) s_psl = latest(nullptr);
        }
        Log("webauthn: public suffix list %s", s_psl && s_pslIsPublic ? "loaded" : "missing: only single-label domains are refused");
    }
    if (s_psl && s_pslIsPublic) return s_pslIsPublic(s_psl, domain.c_str()) != 0;
    return domain.find('.') == std::string::npos;          // (fallback: at least refuse a bare TLD)
}

// --- helpers ----------------------------------------------------------------------------------
using Bytes = std::vector<BYTE>;

std::string B64u(const BYTE* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string s;
    s.reserve((n * 4 + 2) / 3);
    size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const unsigned v = (p[i] << 16) | (p[i + 1] << 8) | p[i + 2];
        s += t[v >> 18 & 63]; s += t[v >> 12 & 63]; s += t[v >> 6 & 63]; s += t[v & 63];
    }
    if (i < n) {
        const unsigned v = (p[i] << 16) | (i + 1 < n ? p[i + 1] << 8 : 0);
        s += t[v >> 18 & 63]; s += t[v >> 12 & 63];
        if (i + 1 < n) s += t[v >> 6 & 63];
    }
    return s;
}
std::string B64u(const Bytes& b) { return B64u(b.data(), b.size()); }

bool UnB64u(const std::string& s, Bytes& out) {
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '-' || c == '+') v = 62;
        else if (c == '_' || c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = (acc << 6) | (unsigned)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((BYTE)((acc >> bits) & 255)); }
    }
    return true;
}

std::string Lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

// "https://host:port" -> host (lower case); empty for anything else.
std::string HostOf(const std::string& origin, bool& secure) {
    secure = false;
    const size_t sep = origin.find("://");
    if (sep == std::string::npos) return std::string();
    const std::string scheme = Lower(origin.substr(0, sep));
    std::string host = origin.substr(sep + 3);
    if (!host.empty() && host[0] == '[') {                 // IPv6
        const size_t end = host.find(']');
        host = end == std::string::npos ? std::string() : host.substr(0, end + 1);
    } else {
        const size_t colon = host.find(':');
        if (colon != std::string::npos) host = host.substr(0, colon);
    }
    host = Lower(host);
    const bool local = host == "localhost" || (host.size() > 10 && host.compare(host.size() - 10, 10, ".localhost") == 0) ||
                       host == "127.0.0.1" || host == "[::1]";
    secure = scheme == "https" || (scheme == "http" && local);
    return host;
}

bool IsIpAddress(const std::string& host) {
    if (!host.empty() && host[0] == '[') return true;
    if (host.empty()) return false;
    for (char c : host) if (!(std::isdigit((unsigned char)c) || c == '.')) return false;
    return true;
}

// The relying party ID `rpId` may be used by a page on `host` (WebAuthn: a registrable domain
// suffix of the origin's effective domain).
bool RpIdAllowed(const std::string& rpId, const std::string& host) {
    if (rpId.empty() || host.empty() || IsIpAddress(host) || IsIpAddress(rpId)) return false;
    if (rpId == host) return true;
    if (host.size() <= rpId.size() + 1 || host.compare(host.size() - rpId.size() - 1, rpId.size() + 1, "." + rpId) != 0) return false;
    return !IsPublicSuffix(rpId);
}

std::wstring W(const std::string& s) { return ToWide(s); }

DWORD Transports(const JsonValue* list) {
    DWORD t = 0;
    if (!list || !list->isArray()) return 0;
    for (const auto& v : list->items) {
        if (v.s == "usb") t |= WEBAUTHN_CTAP_TRANSPORT_USB;
        else if (v.s == "nfc") t |= WEBAUTHN_CTAP_TRANSPORT_NFC;
        else if (v.s == "ble") t |= WEBAUTHN_CTAP_TRANSPORT_BLE;
        else if (v.s == "internal") t |= WEBAUTHN_CTAP_TRANSPORT_INTERNAL;
        else if (v.s == "hybrid") t |= WEBAUTHN_CTAP_TRANSPORT_HYBRID;
        else if (v.s == "smart-card") t |= WEBAUTHN_CTAP_TRANSPORT_SMART_CARD;
    }
    return t;
}

std::string TransportList(DWORD t) {
    std::string s = "[";
    auto add = [&](DWORD bit, const char* name) { if (t & bit) { if (s.size() > 1) s += ","; s += "\""; s += name; s += "\""; } };
    add(WEBAUTHN_CTAP_TRANSPORT_BLE, "ble");
    add(WEBAUTHN_CTAP_TRANSPORT_HYBRID, "hybrid");
    add(WEBAUTHN_CTAP_TRANSPORT_INTERNAL, "internal");
    add(WEBAUTHN_CTAP_TRANSPORT_NFC, "nfc");
    add(WEBAUTHN_CTAP_TRANSPORT_SMART_CARD, "smart-card");
    add(WEBAUTHN_CTAP_TRANSPORT_USB, "usb");
    return s + "]";
}

const char* Attachment(DWORD usedTransport) {
    if (!usedTransport) return nullptr;
    return (usedTransport & WEBAUTHN_CTAP_TRANSPORT_INTERNAL) ? "platform" : "cross-platform";
}

std::string Error(const char* name, const std::string& message) {
    return std::string("{\"ok\":false,\"name\":") + JsonQuote(name) + ",\"message\":" + JsonQuote(message) + "}";
}

std::string ErrorFromHr(HRESULT hr) {
    std::string name = "NotAllowedError";
    if (s_api.errorName) {
        if (PCWSTR n = s_api.errorName(hr)) {
            const std::string s = ToUtf8(n);
            if (s == "InvalidStateError" || s == "ConstraintError" || s == "NotSupportedError" || s == "NotAllowedError" || s == "UnknownError")
                name = s;
        }
    }
    char msg[160];
    sprintf_s(msg, "The operation either timed out or was not allowed (0x%08lx).", (unsigned long)hr);
    if (name == "InvalidStateError") return Error("InvalidStateError", "This authenticator already has a credential for this account.");
    return Error(name.c_str(), msg);
}

// Structure versions Windows understands for each API version (webauthn.h).
DWORD MakeCredentialOptionsVersion() {
    const DWORD v = s_api.version;
    return v >= 9 ? 9 : v >= 8 ? 8 : v >= 7 ? 7 : v >= 6 ? 6 : v >= 4 ? 5 : v >= 3 ? 4 : 3;
}
DWORD GetAssertionOptionsVersion() {
    const DWORD v = s_api.version;
    return v >= 9 ? 9 : v >= 8 ? 8 : v >= 7 ? 7 : v >= 4 ? 6 : v >= 3 ? 5 : 4;
}

// --- one request at a time --------------------------------------------------------------------
std::mutex s_lock;
bool s_busy = false;
GUID s_cancelId = {};
bool s_haveCancelId = false;
unsigned long long s_requestTab = 0;
double s_requestId = 0;

struct Result {
    WKCompletionListenerRef reply = nullptr;
    std::string json;
};

void Finish(WKCompletionListenerRef reply, const std::string& json) {
    WKStringRef r = WKStringCreateWithUTF8CString(json.c_str());
    WKCompletionListenerComplete(reply, r);
    WKRelease(r);
}

void PostResult(WKCompletionListenerRef reply, std::string json) {
    auto* res = new Result{ reply, std::move(json) };
    if (!g_hWnd || !PostMessageW(g_hWnd, WM_APP_WEBAUTHN, 0, (LPARAM)res)) delete res;   // (closing: dropped)
}

struct Request {
    bool create = false;
    std::string origin, rpId, clientData;
    // create
    std::string rpName, userName, userDisplayName;
    Bytes userId;
    std::vector<LONG> algs;
    bool requireResident = false, preferResident = false, credProps = false;
    DWORD attestation = WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;
    // both
    std::vector<std::pair<Bytes, DWORD>> credentials;      // exclude (create) or allow (get) list
    DWORD timeout = 120000, attachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
    DWORD uv = WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
    std::string appId;                                     // get: the U2F appid extension
    bool privateTab = false;
};

std::string RunMakeCredential(const Request& q, GUID* cancelId) {
    WEBAUTHN_RP_ENTITY_INFORMATION rp = { WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION };
    const std::wstring rpId = W(q.rpId), rpName = W(q.rpName.empty() ? q.rpId : q.rpName);
    rp.pwszId = rpId.c_str();
    rp.pwszName = rpName.c_str();
    WEBAUTHN_USER_ENTITY_INFORMATION user = { WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION };
    Bytes userId = q.userId;
    const std::wstring userName = W(q.userName), displayName = W(q.userDisplayName.empty() ? q.userName : q.userDisplayName);
    user.cbId = (DWORD)userId.size();
    user.pbId = userId.data();
    user.pwszName = userName.c_str();
    user.pwszDisplayName = displayName.c_str();
    std::vector<WEBAUTHN_COSE_CREDENTIAL_PARAMETER> params;
    for (LONG alg : q.algs) params.push_back({ WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION, WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, alg });
    WEBAUTHN_COSE_CREDENTIAL_PARAMETERS paramList = { (DWORD)params.size(), params.data() };
    std::string clientData = q.clientData;
    WEBAUTHN_CLIENT_DATA cd = { WEBAUTHN_CLIENT_DATA_CURRENT_VERSION, (DWORD)clientData.size(), (PBYTE)clientData.data(), WEBAUTHN_HASH_ALGORITHM_SHA_256 };

    std::vector<Bytes> ids;
    std::vector<WEBAUTHN_CREDENTIAL_EX> ex;
    std::vector<PWEBAUTHN_CREDENTIAL_EX> exPtrs;
    ids.reserve(q.credentials.size());
    ex.reserve(q.credentials.size());
    for (const auto& c : q.credentials) {
        ids.push_back(c.first);
        ex.push_back({ WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION, (DWORD)ids.back().size(), ids.back().data(), WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, c.second });
    }
    for (auto& e : ex) exPtrs.push_back(&e);
    WEBAUTHN_CREDENTIAL_LIST exclude = { (DWORD)exPtrs.size(), exPtrs.data() };

    WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS o = {};
    o.dwVersion = MakeCredentialOptionsVersion();
    o.dwTimeoutMilliseconds = q.timeout;
    o.dwAuthenticatorAttachment = q.attachment;
    o.bRequireResidentKey = q.requireResident;
    o.dwUserVerificationRequirement = q.uv;
    o.dwAttestationConveyancePreference = q.attestation;
    o.pCancellationId = cancelId;
    if (!exPtrs.empty()) o.pExcludeCredentialList = &exclude;
    if (o.dwVersion >= 4) o.bPreferResidentKey = q.preferResident && !q.requireResident;
    if (o.dwVersion >= 5) o.bBrowserInPrivateMode = q.privateTab;

    PWEBAUTHN_CREDENTIAL_ATTESTATION a = nullptr;
    const HRESULT hr = s_api.makeCredential(g_hWnd, &rp, &user, &paramList, &cd, &o, &a);
    if (FAILED(hr) || !a) {
        Log("webauthn: create for %s -> 0x%08lx", q.rpId.c_str(), (unsigned long)hr);
        if (a) s_api.freeAttestation(a);
        return ErrorFromHr(FAILED(hr) ? hr : E_FAIL);
    }
    const DWORD used = a->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_3 ? a->dwUsedTransport : 0;
    const DWORD transports = a->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_8 && a->dwTransports ? a->dwTransports : used;
    std::string json = "{\"ok\":true,\"id\":\"" + B64u(a->pbCredentialId, a->cbCredentialId) + "\"";
    if (const char* att = Attachment(used)) json += std::string(",\"attachment\":\"") + att + "\"";
    json += ",\"clientDataJSON\":\"" + B64u((const BYTE*)clientData.data(), clientData.size()) + "\"";
    json += ",\"attestationObject\":\"" + B64u(a->pbAttestationObject, a->cbAttestationObject) + "\"";
    json += ",\"authenticatorData\":\"" + B64u(a->pbAuthenticatorData, a->cbAuthenticatorData) + "\"";
    json += ",\"transports\":" + TransportList(transports);
    json += ",\"ext\":{";
    if (q.credProps) {
        const bool known = a->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_4;
        json += known ? std::string("\"credProps\":{\"rk\":") + (a->bResidentKey ? "true" : "false") + "}" : "\"credProps\":{}";
    }
    json += "}}";
    Log("webauthn: credential created for %s (%s)", q.rpId.c_str(), Attachment(used) ? Attachment(used) : "?");
    s_api.freeAttestation(a);
    return json;
}

std::string RunGetAssertion(const Request& q, GUID* cancelId) {
    const std::wstring rpId = W(q.rpId), appId = W(q.appId);
    std::string clientData = q.clientData;
    WEBAUTHN_CLIENT_DATA cd = { WEBAUTHN_CLIENT_DATA_CURRENT_VERSION, (DWORD)clientData.size(), (PBYTE)clientData.data(), WEBAUTHN_HASH_ALGORITHM_SHA_256 };
    std::vector<Bytes> ids;
    std::vector<WEBAUTHN_CREDENTIAL_EX> ex;
    std::vector<PWEBAUTHN_CREDENTIAL_EX> exPtrs;
    ids.reserve(q.credentials.size());
    ex.reserve(q.credentials.size());
    for (const auto& c : q.credentials) {
        ids.push_back(c.first);
        ex.push_back({ WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION, (DWORD)ids.back().size(), ids.back().data(), WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, c.second });
    }
    for (auto& e : ex) exPtrs.push_back(&e);
    WEBAUTHN_CREDENTIAL_LIST allow = { (DWORD)exPtrs.size(), exPtrs.data() };
    BOOL usedAppId = FALSE;

    WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS o = {};
    o.dwVersion = GetAssertionOptionsVersion();
    o.dwTimeoutMilliseconds = q.timeout;
    o.dwAuthenticatorAttachment = q.attachment;
    o.dwUserVerificationRequirement = q.uv;
    if (!q.appId.empty()) { o.pwszU2fAppId = appId.c_str(); o.pbU2fAppId = &usedAppId; }
    o.pCancellationId = cancelId;
    if (!exPtrs.empty()) o.pAllowCredentialList = &allow;
    if (o.dwVersion >= 6) o.bBrowserInPrivateMode = q.privateTab;

    PWEBAUTHN_ASSERTION a = nullptr;
    const HRESULT hr = s_api.getAssertion(g_hWnd, rpId.c_str(), &cd, &o, &a);
    if (FAILED(hr) || !a) {
        Log("webauthn: get for %s -> 0x%08lx", q.rpId.c_str(), (unsigned long)hr);
        if (a) s_api.freeAssertion(a);
        return ErrorFromHr(FAILED(hr) ? hr : E_FAIL);
    }
    const DWORD used = a->dwVersion >= WEBAUTHN_ASSERTION_VERSION_4 ? a->dwUsedTransport : 0;
    std::string json = "{\"ok\":true,\"id\":\"" + B64u(a->Credential.pbId, a->Credential.cbId) + "\"";
    if (const char* att = Attachment(used)) json += std::string(",\"attachment\":\"") + att + "\"";
    json += ",\"clientDataJSON\":\"" + B64u((const BYTE*)clientData.data(), clientData.size()) + "\"";
    json += ",\"authenticatorData\":\"" + B64u(a->pbAuthenticatorData, a->cbAuthenticatorData) + "\"";
    json += ",\"signature\":\"" + B64u(a->pbSignature, a->cbSignature) + "\"";
    if (a->cbUserId) json += ",\"userHandle\":\"" + B64u(a->pbUserId, a->cbUserId) + "\"";
    json += ",\"ext\":{";
    if (!q.appId.empty()) json += std::string("\"appid\":") + (usedAppId ? "true" : "false");
    json += "}}";
    Log("webauthn: signed in to %s (%s)", q.rpId.c_str(), Attachment(used) ? Attachment(used) : "?");
    s_api.freeAssertion(a);
    return json;
}

// The client data Windows hashes and the page gets back (WebAuthn §5.8.1, same order).
std::string ClientData(bool create, const std::string& challengeB64u, const std::string& origin) {
    return std::string("{\"type\":\"") + (create ? "webauthn.create" : "webauthn.get") + "\",\"challenge\":" +
           JsonQuote(challengeB64u) + ",\"origin\":" + JsonQuote(origin) + ",\"crossOrigin\":false}";
}

DWORD UserVerification(const std::string& s) {
    if (s == "required") return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
    if (s == "discouraged") return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
    return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
}

DWORD TimeoutOf(const JsonValue& o) {
    const JsonValue* t = o.get("timeout");
    if (!t || t->type != JsonValue::Number || t->n <= 0) return 120000;
    return (DWORD)std::max(15000.0, std::min(600000.0, t->n));       // as other browsers clamp it
}

// Parses a create/get request; empty string = fine, otherwise the error reply.
std::string ParseRequest(bool create, const JsonValue& o, const std::string& origin, const std::string& host, Request& q) {
    q.create = create;
    q.origin = origin;
    q.timeout = TimeoutOf(o);
    Bytes challenge;
    if (!UnB64u(o.str("challenge"), challenge) || challenge.size() < 1)
        return Error("TypeError", "The challenge is missing.");
    q.clientData = ClientData(create, B64u(challenge), origin);
    auto credentialList = [&](const char* key) {
        if (const JsonValue* list = o.get(key); list && list->isArray()) {
            for (const auto& c : list->items) {
                if (c.str("type", "public-key") != "public-key") continue;
                Bytes id;
                if (UnB64u(c.str("id"), id) && !id.empty()) q.credentials.push_back({ id, Transports(c.get("transports")) });
            }
        }
    };
    if (create) {
        const JsonValue* rp = o.get("rp");
        const JsonValue* user = o.get("user");
        if (!rp || !user) return Error("TypeError", "rp and user are required.");
        const JsonValue* rpIdValue = rp->get("id");
        q.rpId = rpIdValue && rpIdValue->isString() ? Lower(rpIdValue->s) : host;
        q.rpName = rp->str("name");
        if (!UnB64u(user->str("id"), q.userId) || q.userId.empty() || q.userId.size() > 64)
            return Error("TypeError", "user.id must be 1 to 64 bytes long.");
        q.userName = user->str("name");
        q.userDisplayName = user->str("displayName");
        if (const JsonValue* params = o.get("pubKeyCredParams"); params && params->isArray()) {
            for (const auto& p : params->items)
                if (p.str("type") == "public-key") if (const JsonValue* alg = p.get("alg"); alg && alg->type == JsonValue::Number) q.algs.push_back((LONG)alg->n);
        }
        if (o.get("pubKeyCredParams") && o.get("pubKeyCredParams")->items.empty()) q.algs = { -7, -257 };   // (spec: empty means the defaults)
        if (q.algs.empty()) return Error("NotSupportedError", "None of the requested algorithms is supported.");
        if (const JsonValue* sel = o.get("authenticatorSelection")) {
            const std::string att = sel->str("authenticatorAttachment");
            if (att == "platform") q.attachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
            else if (att == "cross-platform") q.attachment = WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM;
            const std::string rk = sel->str("residentKey");
            q.requireResident = rk == "required" || (rk.empty() && sel->flag("requireResidentKey"));
            q.preferResident = rk == "preferred";
            q.uv = UserVerification(sel->str("userVerification"));
        }
        const std::string att = o.str("attestation");
        q.attestation = att == "direct" || att == "enterprise" ? WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT
                      : att == "indirect" ? WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_INDIRECT
                      : WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;
        if (const JsonValue* ext = o.get("extensions")) q.credProps = ext->flag("credProps");
        credentialList("excludeCredentials");
    } else {
        const JsonValue* rpIdValue = o.get("rpId");
        q.rpId = rpIdValue && rpIdValue->isString() ? Lower(rpIdValue->s) : host;
        q.uv = UserVerification(o.str("userVerification"));
        if (const JsonValue* ext = o.get("extensions")) {
            const std::string appId = ext->str("appid");
            if (!appId.empty()) {
                // The U2F AppID must be on the same site (a simplified FIDO AppID check: https,
                // and its host one this page could use as an RP ID).
                bool secure = false;
                const std::string appHost = HostOf(appId, secure);
                if (appId.rfind("https://", 0) != 0 || !RpIdAllowed(appHost == host ? host : appHost, host))
                    return Error("SecurityError", "The appid is not valid for this page.");
                q.appId = appId;
            }
        }
        credentialList("allowCredentials");
    }
    if (!RpIdAllowed(q.rpId, host)) {
        Log("webauthn: %s may not use the RP ID %s", origin.c_str(), q.rpId.c_str());
        return Error("SecurityError", "The relying party ID is not a registrable domain suffix of, nor equal to the current domain.");
    }
    return std::string();
}

void OnMessage(WKScriptMessageRef message, WKCompletionListenerRef reply, const void*) {
    std::string body;
    if (WKTypeRef b = WKScriptMessageGetBody(message); b && WKGetTypeID(b) == WKStringGetTypeID()) body = WKStrUtf8((WKStringRef)b);
    JsonValue req;
    if (!JsonParse(body, req) || !req.isObject()) { Finish(reply, Error("UnknownError", "Bad request.")); return; }
    const std::string op = req.str("op");
    const JsonValue* idValue = req.get("id");
    const double id = idValue && idValue->type == JsonValue::Number ? idValue->n : 0;

    // Who is asking: the frame's own origin, never what the page says.
    std::string origin;
    bool mainFrame = false;
    WKPageRef page = nullptr;
    if (WKFrameInfoRef frame = WKScriptMessageGetFrameInfo(message)) {
        mainFrame = WKFrameInfoGetIsMainFrame(frame);
        page = WKFrameInfoGetPage(frame);
        if (WKSecurityOriginRef o = WKFrameInfoCopySecurityOrigin(frame)) {
            if (WKStringRef s = WKSecurityOriginCopyToString(o)) { origin = WKStrUtf8(s); WKRelease(s); }
            WKRelease(o);
        }
    }
    Tab* tab = nullptr;
    for (auto& t : g_tabs) if (page && t->page == page) { tab = t.get(); break; }
    bool secure = false;
    const std::string host = HostOf(origin, secure);

    if (op == "uvpaa") {
        BOOL available = FALSE;
        if (secure && LoadApi()) s_api.uvpaa(&available);
        Finish(reply, std::string("{\"ok\":true,\"available\":") + (available ? "true" : "false") + "}");
        return;
    }
    if (op == "cancel") {
        std::lock_guard<std::mutex> lock(s_lock);
        if (s_busy && s_haveCancelId && s_requestId == id && tab && tab->id == s_requestTab && s_api.cancel) {
            Log("webauthn: request cancelled by the page");
            s_api.cancel(&s_cancelId);
        }
        Finish(reply, "{\"ok\":true}");
        return;
    }
    if (op != "create" && op != "get") { Finish(reply, Error("NotSupportedError", "Unknown request.")); return; }
    if (!secure || host.empty()) { Finish(reply, Error("SecurityError", "Passkeys need a secure (https) page.")); return; }
    if (!tab || tab != ActiveTab()) { Finish(reply, Error("NotAllowedError", "The page is not in the active tab.")); return; }
    if (!mainFrame) {
        // A frame may ask only if it has the same origin as the page itself.
        bool topSecure = false;
        const std::string topUtf8 = ToUtf8(tab->url);
        const std::string topHost = HostOf(topUtf8, topSecure);
        const size_t schemeEnd = topUtf8.find("://");
        const std::string topOrigin = schemeEnd == std::string::npos ? std::string() : topUtf8.substr(0, topUtf8.find('/', schemeEnd + 3));
        if (Lower(topOrigin) != Lower(origin) || topHost != host) {
            Finish(reply, Error("NotAllowedError", "Passkeys are not available in frames from another site."));
            return;
        }
    }
    if (!LoadApi()) { Finish(reply, Error("NotSupportedError", "Windows WebAuthn is not available on this computer.")); return; }

    Request q;
    const JsonValue* options = req.get("options");
    if (!options || !options->isObject()) { Finish(reply, Error("TypeError", "Options are missing.")); return; }
    const std::string bad = ParseRequest(op == "create", *options, origin, host, q);
    if (!bad.empty()) { Finish(reply, bad); return; }
    q.privateTab = tab->privateTab;

    {
        std::lock_guard<std::mutex> lock(s_lock);
        if (s_busy) { Finish(reply, Error("NotAllowedError", "Another passkey request is already open.")); return; }
        s_busy = true;
        s_requestTab = tab->id;
        s_requestId = id;
        s_haveCancelId = s_api.getCancellationId && SUCCEEDED(s_api.getCancellationId(&s_cancelId));
    }
    Log("webauthn: %s for %s (rp %s)", op.c_str(), origin.c_str(), q.rpId.c_str());
    WKRetain(reply);
    std::thread([q = std::move(q), reply]() {
        GUID cancelId;
        bool haveCancel;
        {
            std::lock_guard<std::mutex> lock(s_lock);
            cancelId = s_cancelId;
            haveCancel = s_haveCancelId;
        }
        std::string json = q.create ? RunMakeCredential(q, haveCancel ? &cancelId : nullptr)
                                    : RunGetAssertion(q, haveCancel ? &cancelId : nullptr);
        {
            std::lock_guard<std::mutex> lock(s_lock);
            s_busy = false;
            s_haveCancelId = false;
        }
        PostResult(reply, std::move(json));
    }).detach();
}

}  // namespace

void WebAuthnInit() {
    WKUserContentControllerRef controller = AdBlockUserContentController();
    if (!controller) return;
    WKStringRef name = WKStringCreateWithUTF8CString("lyraWebAuthn");
    WKUserContentControllerAddScriptMessageHandler(controller, name, OnMessage, nullptr);
    WKRelease(name);
    AdBlockAddUserScript(kScript, false);
}

void WebAuthnShutdown() {
    std::lock_guard<std::mutex> lock(s_lock);
    if (s_busy && s_haveCancelId && s_api.cancel) s_api.cancel(&s_cancelId);
}

void WebAuthnDone(LPARAM result) {
    auto* r = (Result*)result;
    if (!r) return;
    if (r->reply) {
        Finish(r->reply, r->json);
        WKRelease(r->reply);
    }
    delete r;
}
