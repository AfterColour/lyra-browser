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
  const storageChanged = makeEvent("storage.onChanged");
  const area = (name) => {
    const ev = makeEvent("storage." + name + ".onChanged");
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
        return withCallback(call("scripting.executeScript", { tabId, code: d.code, files: d.file ? [d.file] : undefined }).then((r) => (r || []).map((x) => x.result)), cb);
      },
      insertCSS: (...args) => {
        const cb = splitCallback(args);
        const tabId = typeof args[0] === "number" ? args.shift() : null;
        const d = args[0] || {};
        return withCallback(call("scripting.insertCSS", { tabId, css: d.code, files: d.file ? [d.file] : undefined }), cb);
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
        return withCallback(call("scripting.executeScript", { tabId: target.tabId, code, files: inj.files }), cb);
      },
      insertCSS: (inj, cb) => withCallback(call("scripting.insertCSS", { tabId: (inj.target || {}).tabId, css: inj.css, files: inj.files }), cb),
      removeCSS: stub(undefined),
      registerContentScripts: stub(undefined), getRegisteredContentScripts: stub([]), unregisterContentScripts: stub(undefined), updateContentScripts: stub(undefined),
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
    const menus = {
      create: (props, cb) => { if (isFunction(cb)) setTimeout(cb, 0); return (props && props.id) || Math.floor(Math.random() * 1e9); },
      update: stub(undefined), remove: stub(undefined), removeAll: stub(undefined),
      onClicked: makeEvent("contextMenus.onClicked"), onShown: makeEvent(), onHidden: makeEvent(),
    };
    const quiet = () => ({ addListener() {}, removeListener() {}, hasListener: () => false, hasListeners: () => false });
    Object.assign(api, {
      tabs, windows, scripting, alarms,
      action, browserAction: action, pageAction: action,
      contextMenus: menus, menus,
      permissions: {
        contains: stub(true), request: stub(true), remove: stub(true),
        getAll: stub({ permissions: EXT.manifest.permissions || [], origins: EXT.manifest.host_permissions || [] }),
        onAdded: makeEvent(), onRemoved: makeEvent(),
      },
      commands: { getAll: stub([]), onCommand: makeEvent("commands.onCommand") },
      notifications: { create: (...a) => withCallback(Promise.resolve(String(typeof a[0] === "string" ? a[0] : Date.now())), splitCallback(a)), clear: stub(true), getAll: stub({}), onClicked: makeEvent(), onClosed: makeEvent(), onButtonClicked: makeEvent() },
      webNavigation: { onBeforeNavigate: quiet(), onCommitted: quiet(), onCompleted: quiet(), onDOMContentLoaded: quiet(), onErrorOccurred: quiet(), onHistoryStateUpdated: quiet(), onReferenceFragmentUpdated: quiet(), onCreatedNavigationTarget: quiet(), getFrame: stub(null), getAllFrames: stub([]) },
      webRequest: { onBeforeRequest: quiet(), onBeforeSendHeaders: quiet(), onSendHeaders: quiet(), onHeadersReceived: quiet(), onResponseStarted: quiet(), onCompleted: quiet(), onErrorOccurred: quiet(), onBeforeRedirect: quiet(), onAuthRequired: quiet(), handlerBehaviorChanged: stub(undefined) },
      declarativeNetRequest: { updateDynamicRules: stub(undefined), getDynamicRules: stub([]), updateSessionRules: stub(undefined), getSessionRules: stub([]), updateEnabledRulesets: stub(undefined), getEnabledRulesets: stub([]), isRegexSupported: stub({ isSupported: true }), MAX_NUMBER_OF_DYNAMIC_RULES: 30000 },
      idle: { queryState: stub("active"), setDetectionInterval() {}, onStateChanged: makeEvent() },
      offscreen: { createDocument: stub(undefined), closeDocument: stub(undefined), hasDocument: stub(false) },
      sidePanel: { setOptions: stub(undefined), setPanelBehavior: stub(undefined), open: stub(undefined) },
    });
    api.extension.getViews = () => [window];
    if (window.top === window) window.close = () => { call("window.close").catch(() => {}); };
  }

  globalThis.chrome = api;
  globalThis.browser = api;
}
