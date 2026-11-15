// Lyra — the WebExtensions API (chrome.* / browser.*) for an extension's scripts.
//
// browser/Extensions.cpp puts this function in front of an extension's content scripts and calls
// it with what it knows about the extension:
//   EXT = { id, base (URL of its folder, ending in /), manifest, messages (the locale's
//           messages.json, or {}), lang }
// It runs in the extension's own isolated world: the page's scripts cannot see `chrome` or the
// message handler below, and the extension sees the page's DOM, not its JavaScript.
// Anything that needs Lyra goes through window.webkit.messageHandlers.lyraExt (a JSON string in,
// a JSON string back).
//
// Supported so far: runtime (id, getURL, getManifest, sendMessage / onMessage within the same
// page, lastError, getPlatformInfo), storage.local / sync / session (+ onChanged), i18n,
// extension.getURL. Callbacks and promises both work, as in Firefox and Chrome MV3.
function (EXT) {
  "use strict";
  const handler = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lyraExt;
  const call = (msg) => {
    if (!handler) return Promise.reject(new Error("Lyra: no extension bridge here"));
    return handler.postMessage(JSON.stringify(msg)).then((r) => {
      const v = r ? JSON.parse(r) : {};
      if (v && v.error) throw new Error(v.error);
      return v ? v.value : undefined;
    });
  };

  // ---- events --------------------------------------------------------------------------------
  const makeEvent = () => {
    const listeners = new Set();
    return {
      addListener(f) { if (typeof f === "function") listeners.add(f); },
      removeListener(f) { listeners.delete(f); },
      hasListener(f) { return listeners.has(f); },
      hasListeners() { return listeners.size > 0; },
      _listeners: listeners,
      _fire(...args) {
        for (const f of [...listeners]) {
          try { f(...args); } catch (e) { console.error(e); }
        }
      },
    };
  };

  // ---- callbacks and promises: f(args..., callback?) ---------------------------------------
  const runtime = {};
  const withCallback = (promise, callback) => {
    if (typeof callback !== "function") return promise;
    promise.then((value) => callback(value), (error) => {
      runtime.lastError = { message: String(error && error.message || error) };
      try { callback(); } finally { runtime.lastError = undefined; }
    });
    return undefined;
  };

  // ---- runtime -------------------------------------------------------------------------------
  Object.assign(runtime, {
    id: EXT.id,
    lastError: undefined,
    getURL: (path) => EXT.base + String(path || "").replace(/^\/+/, ""),
    getManifest: () => JSON.parse(JSON.stringify(EXT.manifest)),
    getPlatformInfo: (cb) => withCallback(Promise.resolve({ os: "win", arch: "x86-64", nacl_arch: "x86-64" }), cb),
    onMessage: makeEvent(),
    onConnect: makeEvent(),
    onInstalled: makeEvent(),
    onStartup: makeEvent(),
    onSuspend: makeEvent(),
    // No background page yet: a message reaches the listeners of the other scripts in this page.
    sendMessage(...args) {
      let callback = typeof args[args.length - 1] === "function" ? args.pop() : undefined;
      if (args.length > 1 && typeof args[0] === "string" && args[0] !== EXT.id) args.shift();   // (extensionId, message)
      const message = args[0];
      const promise = new Promise((resolve) => {
        let answered = false, async = false;
        const sendResponse = (r) => { if (!answered) { answered = true; resolve(r); } };
        for (const f of [...runtime.onMessage._listeners]) {
          try {
            const r = f(message, { id: EXT.id, url: location.href }, sendResponse);
            if (r === true) async = true;
            else if (r && typeof r.then === "function") { async = true; r.then(sendResponse); }
          } catch (e) { console.error(e); }
        }
        if (!async) sendResponse(undefined);
      });
      return withCallback(promise, callback);
    },
    connect() {
      return { name: "", onMessage: makeEvent(), onDisconnect: makeEvent(), postMessage() {}, disconnect() {} };
    },
    openOptionsPage: (cb) => withCallback(call({ op: "runtime.openOptionsPage" }), cb),
  });

  // ---- storage ---------------------------------------------------------------------------------
  const storageChanged = makeEvent();
  const area = (name) => {
    const ev = makeEvent();
    const changed = (changes) => {
      if (changes && Object.keys(changes).length) { ev._fire(changes); storageChanged._fire(changes, name); }
    };
    return {
      get: (keys, cb) => {
        if (typeof keys === "function") { cb = keys; keys = null; }
        return withCallback(call({ op: "storage.get", area: name, keys: keys === undefined ? null : keys }), cb);
      },
      set: (items, cb) => withCallback(call({ op: "storage.set", area: name, items: items || {} }).then(changed), cb),
      remove: (keys, cb) => withCallback(call({ op: "storage.remove", area: name, keys }).then(changed), cb),
      clear: (cb) => withCallback(call({ op: "storage.clear", area: name }).then(changed), cb),
      getBytesInUse: (keys, cb) => withCallback(Promise.resolve(0), typeof keys === "function" ? keys : cb),
      setAccessLevel: () => Promise.resolve(),
      onChanged: ev,
      QUOTA_BYTES: 10485760,
    };
  };
  const storage = { local: area("local"), sync: area("sync"), session: area("session"), managed: area("managed"), onChanged: storageChanged };

  // ---- i18n ------------------------------------------------------------------------------------
  const getMessage = (name, substitutions) => {
    const entry = EXT.messages[String(name)] || EXT.messages[String(name).toLowerCase()];
    if (!entry || typeof entry.message !== "string") {
      if (name === "@@extension_id") return EXT.id;
      if (name === "@@ui_locale") return EXT.lang.replace("-", "_");
      if (name === "@@bidi_dir") return "ltr";
      return "";
    }
    const subs = substitutions === undefined ? [] : Array.isArray(substitutions) ? substitutions.map(String) : [String(substitutions)];
    const placeholders = entry.placeholders || {};
    let text = entry.message.replace(/\$([A-Za-z0-9_@]+)\$/g, (m, key) => {
      const p = placeholders[key] || placeholders[key.toLowerCase()];
      return p && typeof p.content === "string" ? p.content : m;
    });
    text = text.replace(/\$(\d)/g, (m, d) => subs[+d - 1] !== undefined ? subs[+d - 1] : "").replace(/\$\$/g, "$");
    return text;
  };
  const i18n = {
    getMessage,
    getUILanguage: () => EXT.lang,
    getAcceptLanguages: (cb) => withCallback(Promise.resolve([EXT.lang, "en"]), cb),
    detectLanguage: (text, cb) => withCallback(Promise.resolve({ isReliable: false, languages: [] }), cb),
  };

  const api = {
    runtime,
    storage,
    i18n,
    extension: { getURL: runtime.getURL, inIncognitoContext: false, isAllowedIncognitoAccess: (cb) => withCallback(Promise.resolve(false), cb) },
  };
  globalThis.chrome = api;
  globalThis.browser = api;
}
