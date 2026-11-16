// Lyra — the WebExtensions API (chrome.* / browser.*) for an extension's scripts.
//
// browser/Extensions.cpp puts this function in front of an extension's scripts and calls it with
// what it knows about the extension:
//   EXT = { id, base (URL of its folder, ending in /), manifest, messages (the locale's
//           messages.json, or {}), lang, context: "content" (scripts in web pages, in the
//           extension's isolated world) or "extension" (its background page, pop-up, options) }
// Anything that needs Lyra goes through window.webkit.messageHandlers.lyraExt: a JSON string in,
// a JSON string back ({ value } or { error }). Lyra calls back into the scripts through
// __lyraDeliverMessage (a message for runtime.onMessage) and __lyraEvent (any other event).
// Callbacks and promises both work, as in Firefox and Chrome MV3.
function (EXT) {
  "use strict";
  const handler = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.lyraExt;
  const call = (op, args) => {
    if (!handler) return Promise.reject(new Error("Lyra: no extension bridge here"));
    return handler.postMessage(JSON.stringify(Object.assign({ op }, args || {}))).then((r) => {
      const v = r ? JSON.parse(r) : {};
      if (v && v.error) throw new Error(v.error);
      return v ? v.value : undefined;
    });
  };
  const isFunction = (f) => typeof f === "function";

  // Errors and warnings of the extension's own pages go to Lyra's log (tabs.log), at most 100 per page.
  if (EXT.context === "extension" && handler && !globalThis.__lyraConsole) {
    globalThis.__lyraConsole = true;
    let budget = 100;
    const text = (a) => { try { return a instanceof Error ? String(a) + (a.stack ? " @ " + a.stack.split("\n").slice(0, 4).join(" < ") : "") : typeof a === "object" ? JSON.stringify(a) : String(a); } catch (e) { return String(a); } };
    const send = (level, args) => {
      if (budget-- <= 0) return;
      try { handler.postMessage(JSON.stringify({ op: "debug.log", level, text: args.map(text).join(" ").slice(0, 600), url: location.pathname })); } catch (e) {}
    };
    for (const level of ["error", "warn"]) {
      const original = console[level].bind(console);
      console[level] = (...args) => { send(level, args); original(...args); };
    }
    addEventListener("error", (e) => send("uncaught", [e.error || e.message]));
    addEventListener("unhandledrejection", (e) => send("rejection", [e.reason]));
  }

  // ---- events --------------------------------------------------------------------------------
  const events = {};
  const makeEvent = (name) => {
    const listeners = new Set();
    const ev = {
      addListener(f) { if (isFunction(f)) listeners.add(f); },
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
    if (name) events[name] = ev;
    return ev;
  };

  // ---- callbacks and promises: f(args..., callback?) ---------------------------------------
  const runtime = {};
  const withCallback = (promise, callback) => {
    if (!isFunction(callback)) return promise;
    promise.then((value) => callback(value), (error) => {
      runtime.lastError = { message: String(error && error.message || error) };
      try { callback(); } finally { runtime.lastError = undefined; }
    });
    return undefined;
  };
  const splitCallback = (args) => isFunction(args[args.length - 1]) ? args.pop() : undefined;
  const stub = (value) => (...args) => withCallback(Promise.resolve(value), splitCallback(args));

  // ---- runtime -------------------------------------------------------------------------------
  Object.assign(runtime, {
    id: EXT.id,
    lastError: undefined,
    getURL: (path) => EXT.base + String(path || "").replace(/^\/+/, ""),
    getManifest: () => JSON.parse(JSON.stringify(EXT.manifest)),
    getPlatformInfo: stub({ os: "win", arch: "x86-64", nacl_arch: "x86-64" }),
    getBrowserInfo: stub({ name: "Lyra", vendor: "Lyra", version: "0.2", buildID: "0" }),
    onMessage: makeEvent("runtime.onMessage"),
    onMessageExternal: makeEvent("runtime.onMessageExternal"),
    onConnect: makeEvent("runtime.onConnect"),
    onInstalled: makeEvent("runtime.onInstalled"),
    onStartup: makeEvent("runtime.onStartup"),
    onSuspend: makeEvent("runtime.onSuspend"),
    onUpdateAvailable: makeEvent(),
    // A message goes to the extension's other side: from a page's scripts to its background
    // page (and pop-up), from those to each other.
    sendMessage(...args) {
      const callback = splitCallback(args);
      if (args.length > 1 && typeof args[0] === "string" && args[0] !== EXT.id && (args.length > 2 || typeof args[1] !== "object")) args.shift();
      else if (args.length > 1 && args[0] === EXT.id) args.shift();
      return withCallback(call("runtime.sendMessage", { message: args[0] === undefined ? null : args[0] }), callback);
    },
    // A long-lived connection to the extension's other side (runtime.onConnect there).
    connect(...args) {
      if (typeof args[0] === "string") args.shift();     // (an extension id: only this one)
      const info = args[0] || {};
      return openPort({ name: info.name || "" });
    },
    openOptionsPage: (cb) => withCallback(call("runtime.openOptionsPage"), cb),
    setUninstallURL: stub(undefined),
    reload: () => call("runtime.reload"),
    getBackgroundPage: (cb) => withCallback(Promise.resolve(null), cb),
  });

  // Lyra: a message for runtime.onMessage. Answers with message.reply: the first sendResponse
  // (or a promise a listener returns); answered: false if no listener will answer.
  globalThis.__lyraDeliverMessage = (callId, message, sender) => {
    let answered = false, willAnswer = false;
    const sendResponse = (value) => {
      if (answered) return;
      answered = true;
      call("message.reply", { callId, answered: true, value: value === undefined ? null : value }).catch(() => {});
    };
    for (const f of [...runtime.onMessage._listeners]) {
      try {
        const r = f(message, sender, sendResponse);
        if (r === true) willAnswer = true;
        else if (r && isFunction(r.then)) { willAnswer = true; r.then(sendResponse, () => sendResponse(undefined)); }
      } catch (e) { console.error(e); }
    }
    if (!answered && !willAnswer) call("message.reply", { callId, answered: false }).catch(() => {});
    return true;
  };
  globalThis.__lyraEvent = (name, args) => {
    const ev = events[name];
    if (ev) ev._fire(...(args || []));
    return true;
  };

  // ---- ports (runtime.connect, tabs.connect) ---------------------------------------------------
  // Lyra gives each port an id and carries its messages: __lyraPortConnect (a new port for
  // runtime.onConnect here), __lyraPortMessage, __lyraPortDisconnect (the other side has gone).
  const ports = new Map(), early = new Map();
  const makePort = (name, sender) => {
    const port = {
      name: String(name || ""),
      sender,
      onMessage: makeEvent(),
      onDisconnect: makeEvent(),
      _id: null, _queue: [], _closed: false,
      postMessage(message) {
        if (port._closed) throw new Error("Attempting to use a disconnected port object");
        const m = message === undefined ? null : message;
        if (port._id === null) port._queue.push(m);
        else call("port.post", { id: port._id, message: m }).catch(() => {});
      },
      disconnect() {
        if (port._closed) return;
        port._closed = true;
        if (port._id !== null) { ports.delete(port._id); call("port.close", { id: port._id }).catch(() => {}); }
      },
    };
    return port;
  };
  const portGone = (port, error) => {
    if (port._closed) return;
    port._closed = true;
    ports.delete(port._id);
    if (error) runtime.lastError = { message: error };
    try { port.onDisconnect._fire(port); } finally { runtime.lastError = undefined; }
  };
  const NO_NATIVE = "Specified native messaging host not found.";
  Object.assign(runtime, {
    connectNative: () => {
      const port = makePort("");
      port.postMessage = () => {};
      setTimeout(() => portGone(port, NO_NATIVE), 0);
      return port;
    },
    sendNativeMessage: (...args) => withCallback(Promise.reject(new Error(NO_NATIVE)), splitCallback(args)),
  });
  // (what arrives for a port before this side knows its id: kept a moment)
  const later = (id, f) => {
    if (!early.has(id)) { early.set(id, []); setTimeout(() => early.delete(id), 10000); }
    early.get(id).push(f);
  };
  const openPort = (args) => {
    const port = makePort(args.name);
    call("port.connect", args).then((r) => {
      port._id = r.id;
      if (port._closed) { call("port.close", { id: r.id }).catch(() => {}); return; }
      ports.set(r.id, port);
      for (const m of port._queue.splice(0)) call("port.post", { id: r.id, message: m }).catch(() => {});
      if (r.alone) { setTimeout(() => portGone(port, "Could not establish connection. Receiving end does not exist."), 0); return; }
      const missed = early.get(r.id);
      early.delete(r.id);
      if (missed) for (const f of missed) f();
    }, (e) => setTimeout(() => portGone(port, String(e && e.message || e)), 0));
    return port;
  };
  globalThis.__lyraPortConnect = (id, name, sender) => {
    if (!runtime.onConnect._listeners.size) { call("port.close", { id }).catch(() => {}); return true; }
    const port = makePort(name, sender);
    port._id = id;
    ports.set(id, port);
    runtime.onConnect._fire(port);
    return true;
  };
  globalThis.__lyraPortMessage = (id, message) => {
    const port = ports.get(id);
    if (!port) later(id, () => globalThis.__lyraPortMessage(id, message));
    else if (!port._closed) port.onMessage._fire(message, port);
    return true;
  };
  globalThis.__lyraPortDisconnect = (id) => {
    const port = ports.get(id);
    if (port) portGone(port);
    else later(id, () => globalThis.__lyraPortDisconnect(id));
    return true;
  };

  // ---- storage ---------------------------------------------------------------------------------
  // In a web page, Lyra sends storage changes only once a script here listens for them (uBlock
  // Origin writes megabytes of cache that its page scripts never read).
  let storageWatched = false;
  const watchStorage = (ev) => {
    if (EXT.context !== "content") return ev;
    const add = ev.addListener;
    ev.addListener = (f) => {
      if (!storageWatched) { storageWatched = true; call("storage.watch", {}).catch(() => {}); }
      add(f);
    };
    return ev;
  };
  const storageChanged = watchStorage(makeEvent("storage.onChanged"));
  const area = (name) => {
    const ev = watchStorage(makeEvent("storage." + name + ".onChanged"));
    return {
      get: (keys, cb) => {
        if (isFunction(keys)) { cb = keys; keys = null; }
        return withCallback(call("storage.get", { area: name, keys: keys === undefined ? null : keys }), cb);
      },
      set: (items, cb) => withCallback(call("storage.set", { area: name, items: items || {} }).then(() => undefined), cb),
      remove: (keys, cb) => withCallback(call("storage.remove", { area: name, keys }).then(() => undefined), cb),
      clear: (cb) => withCallback(call("storage.clear", { area: name }).then(() => undefined), cb),
      getBytesInUse: (keys, cb) => withCallback(Promise.resolve(0), isFunction(keys) ? keys : cb),
      getKeys: (cb) => withCallback(call("storage.get", { area: name, keys: null }).then((all) => Object.keys(all || {})), cb),
      setAccessLevel: () => Promise.resolve(),
      onChanged: ev,
      QUOTA_BYTES: 10485760,
    };
  };
  // (onChanged reaches every context of the extension, this one included: Lyra sends it)
  const storage = { local: area("local"), sync: area("sync"), session: area("session"), managed: area("managed"), onChanged: storageChanged };

  // ---- i18n ------------------------------------------------------------------------------------
  const getMessage = (name, substitutions) => {
    const entry = EXT.messages[String(name)] || EXT.messages[String(name).toLowerCase()];
    if (!entry || typeof entry.message !== "string") {
      if (name === "@@extension_id") return EXT.id;
      if (name === "@@ui_locale") return EXT.lang.replace("-", "_");
      if (name === "@@bidi_dir") return "ltr";
      if (name === "@@bidi_start_edge") return "left";
      if (name === "@@bidi_end_edge") return "right";
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
    getAcceptLanguages: stub([EXT.lang, "en"]),
    detectLanguage: stub({ isReliable: false, languages: [] }),
  };

  const api = {
    runtime,
    storage,
    i18n,
    extension: {
      getURL: runtime.getURL,
      inIncognitoContext: false,
      isAllowedIncognitoAccess: stub(false),
      isAllowedFileSchemeAccess: stub(false),
      getBackgroundPage: () => null,
      getViews: () => [],
    },
  };

  // ---- extension pages (background, pop-up, options): the rest of the API ---------------------
  if (EXT.context === "extension") {
    const tabs = {
      TAB_ID_NONE: -1,
      query: (info, cb) => withCallback(call("tabs.query", { info: info || {} }), cb),
      get: (tabId, cb) => withCallback(call("tabs.get", { tabId }), cb),
      getCurrent: (cb) => withCallback(Promise.resolve(undefined), cb),
      create: (props, cb) => withCallback(call("tabs.create", { props: props || {} }), cb),
      update: (...args) => {
        const cb = splitCallback(args);
        const tabId = typeof args[0] === "number" ? args.shift() : null;
        return withCallback(call("tabs.update", { tabId, props: args[0] || {} }), cb);
      },
      remove: (tabIds, cb) => withCallback(call("tabs.remove", { tabIds: Array.isArray(tabIds) ? tabIds : [tabIds] }), cb),
      reload: (...args) => {
        const cb = splitCallback(args);
        return withCallback(call("tabs.reload", { tabId: typeof args[0] === "number" ? args[0] : null }), cb);
      },
      connect: (tabId, info) => openPort({ tabId, name: (info || {}).name || "" }),
      sendMessage: (tabId, message, ...rest) => {
        const cb = splitCallback(rest);
        return withCallback(call("tabs.sendMessage", { tabId, message: message === undefined ? null : message }), cb);
      },
      executeScript: (...args) => {          // MV2: (tabId?, { code | file })
        const cb = splitCallback(args);
        const tabId = typeof args[0] === "number" ? args.shift() : null;
        const d = args[0] || {};
        return withCallback(call("scripting.executeScript", { tabId, code: d.code, files: d.file ? [d.file] : undefined, allFrames: !!d.allFrames, frameIds: typeof d.frameId === "number" ? [d.frameId] : undefined }).then((r) => (r || []).map((x) => x.result)), cb);
      },
      insertCSS: (...args) => {
        const cb = splitCallback(args);
        const tabId = typeof args[0] === "number" ? args.shift() : null;
        const d = args[0] || {};
        return withCallback(call("scripting.insertCSS", { tabId, css: d.code, files: d.file ? [d.file] : undefined, allFrames: !!d.allFrames, frameIds: typeof d.frameId === "number" ? [d.frameId] : undefined, origin: d.cssOrigin || "author" }), cb);
      },
      removeCSS: stub(undefined),
      setZoom: stub(undefined),
      getZoom: stub(1),
      captureVisibleTab: (...args) => withCallback(Promise.reject(new Error("captureVisibleTab is not supported yet")), splitCallback(args)),
      onCreated: makeEvent("tabs.onCreated"),
      onUpdated: makeEvent("tabs.onUpdated"),
      onActivated: makeEvent("tabs.onActivated"),
      onRemoved: makeEvent("tabs.onRemoved"),
      onReplaced: makeEvent("tabs.onReplaced"),
      onMoved: makeEvent("tabs.onMoved"),
      onHighlighted: makeEvent("tabs.onHighlighted"),
      onZoomChange: makeEvent("tabs.onZoomChange"),
    };
    const windows = {
      WINDOW_ID_NONE: -1, WINDOW_ID_CURRENT: -2,
      get: (id, ...rest) => withCallback(call("windows.get"), splitCallback(rest)),
      getCurrent: (...args) => withCallback(call("windows.get"), splitCallback(args)),
      getLastFocused: (...args) => withCallback(call("windows.get"), splitCallback(args)),
      getAll: (...args) => withCallback(call("windows.get").then((w) => [w]), splitCallback(args)),
      create: (props, cb) => withCallback(call("tabs.create", { props: { url: Array.isArray((props || {}).url) ? props.url[0] : (props || {}).url } }).then(() => call("windows.get")), cb),
      update: (...args) => withCallback(call("windows.get"), splitCallback(args)),
      remove: stub(undefined),
      onCreated: makeEvent(), onRemoved: makeEvent(), onFocusChanged: makeEvent("windows.onFocusChanged"),
    };
    const scripting = {
      executeScript: (inj, cb) => {
        inj = inj || {};
        const target = inj.target || {};
        const code = inj.func ? "(" + inj.func.toString() + ").apply(null, " + JSON.stringify(inj.args || []) + ")" : undefined;
        return withCallback(call("scripting.executeScript", { tabId: target.tabId, code, files: inj.files, world: inj.world || "ISOLATED", allFrames: !!target.allFrames, frameIds: target.frameIds }), cb);
      },
      ExecutionWorld: { ISOLATED: "ISOLATED", MAIN: "MAIN" },
      StyleOrigin: { AUTHOR: "AUTHOR", USER: "USER" },
      insertCSS: (inj, cb) => withCallback(call("scripting.insertCSS", { tabId: (inj.target || {}).tabId, css: inj.css, files: inj.files, allFrames: !!(inj.target || {}).allFrames, frameIds: (inj.target || {}).frameIds, origin: inj.origin || "AUTHOR" }), cb),
      removeCSS: stub(undefined),
      registerContentScripts: (scripts, cb) => withCallback(call("scripting.registerContentScripts", { scripts: scripts || [] }).then(() => undefined), cb),
      getRegisteredContentScripts: (...args) => { const cb = splitCallback(args); const f = args[0] || {}; return withCallback(call("scripting.getRegisteredContentScripts", { ids: Array.isArray(f.ids) ? f.ids : null }), cb); },
      unregisterContentScripts: (...args) => { const cb = splitCallback(args); const f = args[0] || {}; return withCallback(call("scripting.unregisterContentScripts", { ids: Array.isArray(f.ids) ? f.ids : null }).then(() => undefined), cb); },
      updateContentScripts: (scripts, cb) => withCallback(call("scripting.updateContentScripts", { scripts: scripts || [] }).then(() => undefined), cb),
    };
    // The toolbar button: Lyra draws it (icon, badge, title) and opens its pop-up. Values with a
    // tabId are for that tab only.
    const actionSet = (what, tabId, value, cb) => withCallback(call("action.set", { what, details: { tabId, value: value === undefined ? null : value } }), cb);
    const actionGet = (what, d, cb) => {
      if (isFunction(d)) { cb = d; d = {}; }
      return withCallback(call("action.get", { what, details: d || {} }), cb);
    };
    const relative = (p) => typeof p === "string" ? p.split(EXT.base).join("").replace(/^\.?\//, "") : p;
    // ImageData (or { size: ImageData }) to plain pixels Lyra can read.
    const packImage = (img) => {
      if (img && !img.data && typeof img === "object") {
        const sizes = Object.keys(img).map(Number).filter((n) => n > 0).sort((a, b) => a - b);
        img = img[sizes.find((n) => n >= 32) || sizes[sizes.length - 1]];
      }
      if (!img || !img.data || !img.width || !img.height) return null;
      const d = img.data;
      let bin = "";
      for (let i = 0; i < d.length; i += 0x8000) bin += String.fromCharCode.apply(null, Array.prototype.slice.call(d, i, i + 0x8000));
      return { width: img.width, height: img.height, rgba: btoa(bin) };
    };
    const iconValue = (d) => {
      if (d.imageData) return packImage(d.imageData);
      const p = d.path;
      if (p && typeof p === "object") { const o = {}; for (const k of Object.keys(p)) o[k] = relative(p[k]); return o; }
      return relative(p);
    };
    const enabler = (value) => (tabId, cb) => {
      if (isFunction(tabId)) { cb = tabId; tabId = undefined; }
      return actionSet("enabled", typeof tabId === "number" ? tabId : undefined, value, cb);
    };
    const action = {
      setBadgeText: (d, cb) => { d = d || {}; return actionSet("badgeText", d.tabId, d.text == null ? null : String(d.text), cb); },
      getBadgeText: (d, cb) => actionGet("badgeText", d, cb),
      setBadgeBackgroundColor: (d, cb) => { d = d || {}; return actionSet("badgeColor", d.tabId, d.color, cb); },
      getBadgeBackgroundColor: (d, cb) => actionGet("badgeColor", d, cb),
      setBadgeTextColor: (d, cb) => { d = d || {}; return actionSet("badgeTextColor", d.tabId, d.color, cb); },
      getBadgeTextColor: (d, cb) => actionGet("badgeTextColor", d, cb),
      setTitle: (d, cb) => { d = d || {}; return actionSet("title", d.tabId, d.title == null ? null : String(d.title), cb); },
      getTitle: (d, cb) => actionGet("title", d, cb),
      setIcon: (d, cb) => { d = d || {}; return actionSet("icon", d.tabId, iconValue(d), cb); },
      setPopup: (d, cb) => { d = d || {}; return actionSet("popup", d.tabId, d.popup == null ? null : relative(String(d.popup)), cb); },
      getPopup: (d, cb) => actionGet("popup", d, cb),
      enable: enabler(true), disable: enabler(false),
      show: enabler(true), hide: enabler(false),            // (pageAction)
      isEnabled: (d, cb) => actionGet("enabled", typeof d === "number" ? { tabId: d } : d, cb),
      isShown: (d, cb) => actionGet("enabled", d, cb),
      isEnabledByDefault: stub(true),
      getUserSettings: stub({ isOnToolbar: true }),
      openPopup: (o, cb) => withCallback(call("action.openPopup"), isFunction(o) ? o : cb),
      onClicked: makeEvent("action.onClicked"),
      onUserSettingsChanged: makeEvent(),
    };
    // declarativeNetRequest: Lyra turns the rules into WebKit content blockers (browser/ExtensionDnr.cpp).
const dnrRules = (session) => ({
  update: (o, cb) => withCallback(call("dnr.updateRules", { session, add: (o || {}).addRules || [], remove: (o || {}).removeRuleIds || [] }).then(() => undefined), cb),
  get: (...args) => { const cb = splitCallback(args); const f = args[0] || {}; return withCallback(call("dnr.getRules", { session, ids: Array.isArray(f.ruleIds) ? f.ruleIds : null }), cb); },
});
const dnr = {
  updateDynamicRules: dnrRules(false).update,
  getDynamicRules: dnrRules(false).get,
  updateSessionRules: dnrRules(true).update,
  getSessionRules: dnrRules(true).get,
  updateEnabledRulesets: (o, cb) => withCallback(call("dnr.updateEnabledRulesets", { enable: (o || {}).enableRulesetIds || [], disable: (o || {}).disableRulesetIds || [] }).then(() => undefined), cb),
  getEnabledRulesets: (cb) => withCallback(call("dnr.getEnabledRulesets"), cb),
  updateStaticRules: (o, cb) => withCallback(call("dnr.updateStaticRules", { rulesetId: (o || {}).rulesetId || "", enable: (o || {}).enableRuleIds || [], disable: (o || {}).disableRuleIds || [] }).then(() => undefined), cb),
  getDisabledRuleIds: (o, cb) => withCallback(call("dnr.getDisabledRuleIds", { rulesetId: (o || {}).rulesetId || "" }), cb),
  isRegexSupported: (o, cb) => withCallback(call("dnr.isRegexSupported", { regex: (o || {}).regex || "" }), cb),
  getAvailableStaticRuleCount: (cb) => withCallback(Promise.resolve(300000), cb),
  getMatchedRules: (...args) => withCallback(Promise.resolve({ rulesInfo: [] }), splitCallback(args)),
  setExtensionActionOptions: (...args) => withCallback(Promise.resolve(), splitCallback(args)),
  testMatchOutcome: (...args) => withCallback(Promise.resolve({ matchedRules: [] }), splitCallback(args)),
  onRuleMatchedDebug: makeEvent(),
  GUARANTEED_MINIMUM_STATIC_RULES: 30000, MAX_NUMBER_OF_RULES: 30000, MAX_NUMBER_OF_DYNAMIC_RULES: 30000,
  MAX_NUMBER_OF_DYNAMIC_AND_SESSION_RULES: 30000, MAX_NUMBER_OF_UNSAFE_DYNAMIC_RULES: 5000, MAX_NUMBER_OF_SESSION_RULES: 5000,
  MAX_NUMBER_OF_UNSAFE_SESSION_RULES: 5000, MAX_NUMBER_OF_REGEX_RULES: 1000, MAX_NUMBER_OF_STATIC_RULESETS: 100,
  MAX_NUMBER_OF_ENABLED_STATIC_RULESETS: 50, MAX_GETMATCHEDRULES_CALLS_PER_INTERVAL: 20, GETMATCHEDRULES_QUOTA_INTERVAL: 10,
  DYNAMIC_RULESET_ID: "_dynamic", SESSION_RULESET_ID: "_session",
  RuleActionType: { BLOCK: "block", REDIRECT: "redirect", ALLOW: "allow", UPGRADE_SCHEME: "upgradeScheme", MODIFY_HEADERS: "modifyHeaders", ALLOW_ALL_REQUESTS: "allowAllRequests" },
  ResourceType: { MAIN_FRAME: "main_frame", SUB_FRAME: "sub_frame", STYLESHEET: "stylesheet", SCRIPT: "script", IMAGE: "image", FONT: "font", OBJECT: "object", XMLHTTPREQUEST: "xmlhttprequest", PING: "ping", CSP_REPORT: "csp_report", MEDIA: "media", WEBSOCKET: "websocket", WEBTRANSPORT: "webtransport", WEBBUNDLE: "webbundle", OTHER: "other" },
  DomainType: { FIRST_PARTY: "firstParty", THIRD_PARTY: "thirdParty" },
  HeaderOperation: { APPEND: "append", SET: "set", REMOVE: "remove" },
  RequestMethod: { CONNECT: "connect", DELETE: "delete", GET: "get", HEAD: "head", OPTIONS: "options", PATCH: "patch", POST: "post", PUT: "put", OTHER: "other" },
  UnsupportedRegexReason: { SYNTAX_ERROR: "syntaxError", MEMORY_LIMIT_EXCEEDED: "memoryLimitExceeded" },
};

// Alarms live in the background page, which Lyra keeps running.
    const alarmTimers = new Map(), alarmInfo = new Map();
    const onAlarm = makeEvent("alarms.onAlarm");
    const stopAlarm = (name) => { const t = alarmTimers.get(name); clearTimeout(t); clearInterval(t); alarmTimers.delete(name); };
    const alarms = {
      create: (...args) => {
        const cb = splitCallback(args);
        const name = typeof args[0] === "string" ? args.shift() : "";
        const info = args[0] || {};
        stopAlarm(name);
        const delay = info.when ? Math.max(0, info.when - Date.now()) : (info.delayInMinutes != null ? info.delayInMinutes : info.periodInMinutes || 0) * 60000;
        const period = info.periodInMinutes ? info.periodInMinutes * 60000 : 0;
        const fire = () => {
          const a = { name, scheduledTime: Date.now(), periodInMinutes: info.periodInMinutes };
          if (!period) { alarmTimers.delete(name); alarmInfo.delete(name); }
          onAlarm._fire(a);
        };
        alarmInfo.set(name, { name, scheduledTime: Date.now() + delay, periodInMinutes: info.periodInMinutes });
        alarmTimers.set(name, setTimeout(() => {
          fire();
          if (period) alarmTimers.set(name, setInterval(fire, period));
        }, delay));
        return withCallback(Promise.resolve(), cb);
      },
      get: (...args) => { const cb = splitCallback(args); return withCallback(Promise.resolve(alarmInfo.get(args[0] || "")), cb); },
      getAll: (cb) => withCallback(Promise.resolve([...alarmInfo.values()]), cb),
      clear: (...args) => {
        const cb = splitCallback(args);
        const name = args[0] || "";
        const had = alarmTimers.has(name);
        stopAlarm(name);
        alarmInfo.delete(name);
        return withCallback(Promise.resolve(had), cb);
      },
      clearAll: (cb) => {
        for (const name of [...alarmTimers.keys()]) stopAlarm(name);
        alarmInfo.clear();
        return withCallback(Promise.resolve(true), cb);
      },
      onAlarm,
    };
    // Context menu items live in Lyra (kept across restarts); onclick (Manifest V2) stays here.
    const menuClicks = new Map();
    const menuCall = (op, data, cb) => {
      const p = withCallback(call("menus." + op, data), cb);
      if (!cb && p && p.catch) p.catch(() => {});
      return p;
    };
    const menuProps = (id, props) => {
      props = Object.assign({}, props || {});
      if (isFunction(props.onclick)) menuClicks.set(String(id), props.onclick);
      delete props.onclick; delete props.id; delete props.icons;
      return props;
    };
    const menus = {
      create: (props, cb) => {
        const numeric = !props || props.id === undefined || props.id === null;
        const id = numeric ? Math.floor(Math.random() * 2e9) : props.id;
        menuCall("create", { id: String(id), numeric, props: menuProps(id, props) }, cb);
        return id;
      },
      update: (id, props, cb) => menuCall("update", { id: String(id), props: menuProps(id, props) }, cb),
      remove: (id, cb) => { menuClicks.delete(String(id)); return menuCall("remove", { id: String(id) }, cb); },
      removeAll: (cb) => { menuClicks.clear(); return menuCall("removeAll", {}, cb); },
      refresh: () => {},
      onClicked: makeEvent("contextMenus.onClicked"), onShown: makeEvent(), onHidden: makeEvent(),
      ContextType: { ALL: "all", PAGE: "page", FRAME: "frame", SELECTION: "selection", LINK: "link", EDITABLE: "editable",
                     IMAGE: "image", VIDEO: "video", AUDIO: "audio", LAUNCHER: "launcher", BROWSER_ACTION: "browser_action",
                     PAGE_ACTION: "page_action", ACTION: "action" },
      ItemType: { NORMAL: "normal", CHECKBOX: "checkbox", RADIO: "radio", SEPARATOR: "separator" },
      ACTION_MENU_TOP_LEVEL_LIMIT: 6,
    };
    menus.onClicked.addListener((info, tab) => { const f = menuClicks.get(String(info.menuItemId)); if (f) f(info, tab); });
    // Bookmarks and history: only with the permission (as in Chrome, the API is not there otherwise).
    const asks = (name) => (EXT.manifest.permissions || []).includes(name);
    const bm = (op, data, cb) => withCallback(call("bookmarks." + op, data), cb);
    const bookmarks = {
      getTree: (cb) => bm("getTree", {}, cb),
      getSubTree: (id, cb) => bm("getSubTree", { id: String(id) }, cb),
      get: (ids, cb) => bm("get", { ids: [].concat(ids).map(String) }, cb),
      getChildren: (id, cb) => bm("getChildren", { id: String(id) }, cb),
      getRecent: (n, cb) => bm("getRecent", { n }, cb),
      search: (q, cb) => bm("search", { query: typeof q === "string" ? { query: q } : (q || {}) }, cb),
      create: (b, cb) => bm("create", { bookmark: b || {} }, cb),
      update: (id, changes, cb) => bm("update", { id: String(id), changes: changes || {} }, cb),
      move: (id, destination, cb) => bm("move", { id: String(id), destination: destination || {} }, cb),
      remove: (id, cb) => bm("remove", { id: String(id) }, cb),
      removeTree: (id, cb) => bm("removeTree", { id: String(id) }, cb),
      onCreated: makeEvent("bookmarks.onCreated"), onRemoved: makeEvent("bookmarks.onRemoved"),
      onChanged: makeEvent("bookmarks.onChanged"), onMoved: makeEvent("bookmarks.onMoved"),
      onChildrenReordered: makeEvent(), onImportBegan: makeEvent(), onImportEnded: makeEvent(),
      MAX_WRITE_OPERATIONS_PER_HOUR: 1000000, MAX_SUSTAINED_WRITE_OPERATIONS_PER_MINUTE: 1000000,
    };
    const hist = (op, details, cb) => withCallback(call("history." + op, { details: details || {} }), cb);
    const history = {
      search: (q, cb) => hist("search", q, cb),
      getVisits: (d, cb) => hist("getVisits", d, cb),
      addUrl: (d, cb) => hist("addUrl", d, cb),
      deleteUrl: (d, cb) => hist("deleteUrl", d, cb),
      deleteRange: (d, cb) => hist("deleteRange", d, cb),
      deleteAll: (cb) => hist("deleteAll", {}, cb),
      onVisited: makeEvent("history.onVisited"), onVisitRemoved: makeEvent("history.onVisitRemoved"),
    };
    // Firefox's contentScripts.register (extensions built for Firefox, uBlock Origin among them):
    // Lyra's registered scripts for this session, inline code too.
    const m = EXT.manifest;
    if ((m.browser_specific_settings && m.browser_specific_settings.gecko) || (m.applications && m.applications.gecko)) {
      let csSeq = 0;
      const files = (list) => (list || []).filter((x) => x && x.file).map((x) => x.file);
      const codes = (list) => (list || []).filter((x) => x && x.code).map((x) => x.code);
      api.contentScripts = {
        register: (o) => {
          o = o || {};
          const id = "lyra-cs-" + Date.now().toString(36) + "-" + (++csSeq);
          return call("scripting.registerContentScripts", { scripts: [{
            id, matches: o.matches || [], excludeMatches: o.excludeMatches || [], js: files(o.js), css: files(o.css),
            lyraJsCode: codes(o.js), lyraCssCode: codes(o.css), allFrames: !!o.allFrames, runAt: o.runAt || "document_idle",
            persistAcrossSessions: false,
          }] }).then(() => ({ unregister: () => call("scripting.unregisterContentScripts", { ids: [id] }).then(() => undefined) }));
        },
      };
    }
    if (asks("bookmarks")) api.bookmarks = bookmarks;
    if (asks("history")) api.history = history;
    // webRequest: Lyra asks this page's listeners about the tabs' loads (__lyraWebRequest) and the
    // engine waits for blocking ones: { cancel }, { redirectUrl }, { requestHeaders }.
    const wrListeners = new Map();
    let wrSeq = 0;
    const wrEvent = (name) => {
      const mine = [];
      return {
        addListener(cb, filter, spec) {
          if (!isFunction(cb) || mine.some((l) => l.cb === cb)) return;
          const id = ++wrSeq;
          mine.push({ cb, id });
          wrListeners.set(id, cb);
          const s = Array.isArray(spec) ? spec : [];
          call("webRequest.listen", { id, event: name, filter: filter || {}, blocking: s.includes("blocking"), headers: s.includes("requestHeaders") || s.includes("responseHeaders") }).catch(() => {});
        },
        removeListener(cb) {
          const i = mine.findIndex((l) => l.cb === cb);
          if (i < 0) return;
          const [l] = mine.splice(i, 1);
          wrListeners.delete(l.id);
          call("webRequest.unlisten", { id: l.id }).catch(() => {});
        },
        hasListener: (cb) => mine.some((l) => l.cb === cb),
        hasListeners: () => mine.length > 0,
      };
    };
    const webRequest = {
      onBeforeRequest: wrEvent("onBeforeRequest"), onBeforeSendHeaders: wrEvent("onBeforeSendHeaders"), onSendHeaders: wrEvent("onSendHeaders"),
      onHeadersReceived: wrEvent("onHeadersReceived"), onResponseStarted: wrEvent("onResponseStarted"), onCompleted: wrEvent("onCompleted"),
      onErrorOccurred: wrEvent("onErrorOccurred"), onBeforeRedirect: wrEvent("onBeforeRedirect"), onAuthRequired: wrEvent("onAuthRequired"),
      handlerBehaviorChanged: stub(undefined), MAX_HANDLER_BEHAVIOR_CHANGED_CALLS_PER_10_MINUTES: 20,
      ResourceType: { MAIN_FRAME: "main_frame", SUB_FRAME: "sub_frame", STYLESHEET: "stylesheet", SCRIPT: "script", IMAGE: "image", FONT: "font",
                      OBJECT: "object", XMLHTTPREQUEST: "xmlhttprequest", PING: "ping", CSP_REPORT: "csp_report", MEDIA: "media", WEBSOCKET: "websocket", OTHER: "other" },
    };
    globalThis.__lyraWebRequest = (callId, stage, ids, details) => {
      const result = {};
      const merge = (r) => {
        if (!r || typeof r !== "object") return;
        if (r.cancel) result.cancel = true;
        if (r.redirectUrl && !result.redirectUrl) result.redirectUrl = String(r.redirectUrl);
        if (Array.isArray(r.requestHeaders)) result.requestHeaders = r.requestHeaders.map((h) => ({ name: String(h.name), value: h.value === undefined ? "" : String(h.value) }));
        if (Array.isArray(r.responseHeaders)) result.responseHeaders = r.responseHeaders.map((h) => ({ name: String(h.name), value: h.value === undefined ? "" : String(h.value) }));
      };
      const waits = [];
      for (const id of ids) {
        const cb = wrListeners.get(id);
        if (!cb) continue;
        try {
          const r = cb(Object.assign({}, details));
          if (r && isFunction(r.then)) waits.push(r.then(merge, () => {}));
          else merge(r);
        } catch (e) { console.error(e); }
      }
      if (!callId) return true;
      const reply = () => call("webRequest.reply", { callId, stage, response: result }).catch(() => {});
      if (waits.length) Promise.all(waits).then(reply); else reply();
      return true;
    };
    const grantedPermissions = (p) => {
      const have = EXT.manifest.permissions || [];
      return ((p && p.permissions) || []).every((x) => have.includes(x));
    };
    const quiet = () => ({ addListener() {}, removeListener() {}, hasListener: () => false, hasListeners: () => false });
    Object.assign(api, {
      tabs, windows, scripting, alarms,
      action, browserAction: action, pageAction: action,
      contextMenus: menus, menus,
      permissions: {
        // API permissions: those the manifest asks for (optional ones are never granted: no prompt yet;
        // saying yes to "nativeMessaging" made Bitwarden look for its desktop app every 10 s). Sites: yes.
        contains: (p, cb) => withCallback(Promise.resolve(grantedPermissions(p)), cb),
        request: (p, cb) => withCallback(Promise.resolve(grantedPermissions(p)), cb),
        remove: stub(true),
        getAll: stub({ permissions: EXT.manifest.permissions || [], origins: EXT.manifest.host_permissions || [] }),
        onAdded: makeEvent(), onRemoved: makeEvent(),
      },
      commands: { getAll: stub([]), onCommand: makeEvent("commands.onCommand") },
      notifications: {
        create: (...a) => {
          const cb = splitCallback(a);
          const named = typeof a[0] === "string";
          return withCallback(call("notifications.create", { id: named ? a[0] : "", options: (named ? a[1] : a[0]) || {} }), cb);
        },
        update: (id, options, cb) => withCallback(call("notifications.update", { id: String(id), options: options || {} }), cb),
        clear: (id, cb) => withCallback(call("notifications.clear", { id: String(id) }), cb),
        getAll: (cb) => withCallback(call("notifications.getAll", {}), cb),
        getPermissionLevel: stub("granted"),
        onClicked: makeEvent("notifications.onClicked"), onClosed: makeEvent("notifications.onClosed"),
        onButtonClicked: makeEvent("notifications.onButtonClicked"), onShowSettings: makeEvent(), onPermissionLevelChanged: makeEvent(),
        TemplateType: { BASIC: "basic", IMAGE: "image", LIST: "list", PROGRESS: "progress" },
        PermissionLevel: { GRANTED: "granted", DENIED: "denied" },
      },
      webNavigation: { onBeforeNavigate: quiet(), onCommitted: quiet(), onCompleted: quiet(), onDOMContentLoaded: quiet(), onErrorOccurred: quiet(), onHistoryStateUpdated: quiet(), onReferenceFragmentUpdated: quiet(), onCreatedNavigationTarget: quiet(), getFrame: stub(null), getAllFrames: stub([]) },
      webRequest,
      declarativeNetRequest: dnr,
      idle: { queryState: stub("active"), setDetectionInterval() {}, onStateChanged: makeEvent() },
      offscreen: { createDocument: stub(undefined), closeDocument: stub(undefined), hasDocument: stub(false) },
      sidePanel: { setOptions: stub(undefined), setPanelBehavior: stub(undefined), open: stub(undefined) },
    });
    api.extension.getViews = () => [window];
    if (window.top === window) window.close = () => { call("window.close").catch(() => {}); };
  }

  // WebKit has no requestIdleCallback (uBlock Origin's background uses it).
  if (!globalThis.requestIdleCallback) {
    globalThis.requestIdleCallback = (cb, opts) => setTimeout(() => { const end = Date.now() + 15; cb({ didTimeout: false, timeRemaining: () => Math.max(0, end - Date.now()) }); }, 1);
    globalThis.cancelIdleCallback = (id) => clearTimeout(id);
  }
  globalThis.chrome = api;
  globalThis.browser = api;
  // One of its pages inside a tab: Lyra sends it messages and events from now on.
  if (location.protocol === "lyra-extension:" && EXT.context === "extension") call("frame.hello", {}).catch(() => {});
}
