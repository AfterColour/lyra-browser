"""Update smoke test: the first start after an update, as users get it. release.py runs it.

    python tools\\smoke-update.py                      the newest earlier dist\\LyraSetup-<v>.exe -> dist\\LyraSetup.exe
    python tools\\smoke-update.py --from A.exe --to B.exe
    python tools\\smoke-update.py --app run-next          a built folder, no installers (steps 1-2 skipped)

In a folder and a profile of its own (%TEMP%\\lyra-smoke-*, never the installed Lyra or your
profile):
 1. installs the earlier version and opens a YouTube video in it once, so the profile has that
    version's GStreamer plugin cache, as a user's does;
 2. installs the new version over it (LyraSetup /S /noregister) - an update;
 3. starts it on the same video and checks, with a probe script in the page (LYRA_PROBE):
    - the page loads within the time limit and draws (requestAnimationFrame runs),
    - the video plays (its time moves on),
    - the plugin cache was not rebuilt when the page opened: the installer built it. Rebuilding
      it inside the page's process froze YouTube on the first start after the 0.6.0 update.
Needs the internet (YouTube). Exit code 0: passed. On failure the folder is kept for a look.
"""
import ctypes, glob, json, os, re, shutil, subprocess, sys, tempfile, time

LYRA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(LYRA, "dist")
VIDEO = "https://www.youtube.com/watch?v=jNQXAC9IVRw"
LOAD_LIMIT_S = 90          # from start to the probe's answer
OLD_WARMUP_S = 120         # the earlier version: until its plugin cache is written

PROBE = r"""
// A fresh profile gets YouTube's cookie consent dialog, which pauses the video (a person would
// answer it): "Reject all". On consent.youtube.com (a page of its own) the click navigates back to
// the video, which ends this run of the probe; it runs again on that page.
function consent() {
  const boxes = document.querySelectorAll('ytd-consent-bump-v2-lightbox, tp-yt-paper-dialog, form[action*="consent"]');
  const scope = location.hostname.startsWith('consent.') ? [document] : [...boxes].filter(b => b.offsetParent !== null || b.tagName === 'FORM');
  for (const box of scope)
    for (const b of box.querySelectorAll('button, tp-yt-paper-button, [role=button]')) {
      const s = ((b.getAttribute('aria-label') || '') + ' ' + (b.innerText || '')).toLowerCase();
      if (/reject|отклонить/.test(s)) { b.click(); return true; }
    }
  return false;
}
let consented = false;
const t0 = performance.now();
let v = null;
while (!(v = document.querySelector('video')) && performance.now() - t0 < 20000) {
  consented = consent() || consented;
  await new Promise(r => setTimeout(r, 250));
}
const out = { vis: document.visibilityState, video: !!v };
for (let i = 0; i < 16; ++i) {                       // (YouTube's own start-up work first: 4 s)
  consented = consent() || consented;
  await new Promise(r => setTimeout(r, 250));
}
out.consent = consented;
let raf = 0; const r0 = performance.now();
await new Promise(res => { (function f() { raf++; if (performance.now() - r0 < 2000) requestAnimationFrame(f); else res(); })(); setTimeout(res, 3000); });
out.raf2s = raf;
if (v) {
  v.muted = true;
  if (v.ended || (v.duration && v.duration - v.currentTime < 6)) v.currentTime = 0;
  if (v.paused) await v.play().catch(e => { out.playError = String(e); });
  await new Promise(r => setTimeout(r, 1000));
  const c0 = v.currentTime;
  await new Promise(r => setTimeout(r, 2000));
  out.played = +(v.currentTime - c0).toFixed(2);
  out.readyState = v.readyState;
  out.error = v.error ? v.error.code : null;
  // (for a failure: why it stands still)
  out.paused = v.paused; out.t = +v.currentTime.toFixed(2); out.dur = +(v.duration || 0).toFixed(1);
  out.ad = !!document.querySelector('.ad-showing');
  const err = document.querySelector('.ytp-error, yt-playability-error-supported-renderers');
  if (err && err.innerText) out.page = err.innerText.trim().slice(0, 120);
}
return JSON.stringify(out);
"""


def say(msg): print("  [smoke] " + msg, flush=True)


def vkey(path):
    m = re.search(r"LyraSetup-(\d+(?:\.\d+)*)\.exe$", path)
    return [int(x) for x in m.group(1).split(".")] if m else None


def previous_setup(current):
    cur = [int(x) for x in current.split(".")]
    older = [(vkey(p), p) for p in glob.glob(os.path.join(DIST, "LyraSetup-*.exe")) if vkey(p) and vkey(p) < cur]
    return max(older)[1] if older else None


def lyra_window_open():
    return bool(ctypes.windll.user32.FindWindowW("LyraWindowClass", None))


def kill_under(folder):
    ps = "Get-Process | Where-Object { $_.Path -like '%s\\*' } | Stop-Process -Force -ErrorAction SilentlyContinue" % folder
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True)
    time.sleep(2)


def install(setup, inst, env):
    t = time.time()
    r = subprocess.run([setup, "/S", "/noregister", "/dir=" + inst], env=env)
    if r.returncode != 0 or not os.path.exists(os.path.join(inst, "Lyra.exe")):
        raise SystemExit("%s did not install (exit %s)" % (os.path.basename(setup), r.returncode))
    return time.time() - t


class Log:
    """New lines of the test profile's tabs.log."""
    def __init__(self, path): self.path, self.pos = path, os.path.getsize(path) if os.path.exists(path) else 0
    def lines(self):
        if not os.path.exists(self.path): return []
        with open(self.path, "rb") as f:
            f.seek(self.pos)
            data = f.read()
        return data.decode("utf-8", "replace").splitlines()


def bring_to_front(pid):
    """The window in front, as a user has it (a window started from here opens behind the others,
    and a video there did not play in the tests)."""
    user32 = ctypes.windll.user32
    found = []
    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def each(h, _):
        p = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(h), ctypes.byref(p))
        if p.value == pid and user32.IsWindowVisible(ctypes.c_void_p(h)) and user32.GetWindowTextLengthW(ctypes.c_void_p(h)) > 0:
            found.append(h)
        return True
    for _ in range(40):
        found.clear()
        user32.EnumWindows(each, None)
        if found: break
        time.sleep(0.25)
    if not found: return False
    h = ctypes.c_void_p(found[0])
    user32.ShowWindow(h, 9)                                  # SW_RESTORE
    for _ in range(3):
        user32.SetForegroundWindow(h)
        if user32.GetForegroundWindow() == found[0]: return True
        # Windows refuses while another app has the focus; a key press (Alt) from this process
        # lets SetForegroundWindow through.
        user32.keybd_event(0x12, 0, 0, 0); user32.keybd_event(0x12, 0, 2, 0)
        time.sleep(0.5)
    return False


def run_page(inst, env, log_path, limit):
    """Starts Lyra on the video; returns (probe result or None, seconds, the session's log lines)."""
    log = Log(log_path)
    t = time.time()
    p = subprocess.Popen([os.path.join(inst, "Lyra.exe"), VIDEO], env=env, cwd=inst)
    if not bring_to_front(p.pid): say("note: could not bring Lyra's window to the front")
    result = None
    while time.time() - t < limit and result is None:
        time.sleep(1)
        for line in log.lines():
            m = re.search(r"probe tab#\d+: (.*)$", line)
            # "(no result)": the page navigated away while the probe ran (YouTube reloads itself on a
            # fresh profile); the probe runs again on the next page.
            if m and m.group(1).strip() != "(no result)":
                try: result = json.loads(m.group(1))
                except ValueError: result = {"raw": m.group(1)}
    return result, time.time() - t, log.lines()


def check_page(result, secs, lines, cache_rebuilt, problems):
    finish = [l for l in lines if "didFinish" in l and "tab#1" in l and "closed" not in l]
    say("page: %s" % (finish[0].split("] ", 1)[-1] if finish else "no didFinish"))
    say("probe after %.0f s: %s" % (secs, json.dumps(result) if result else "none"))
    if cache_rebuilt:
        problems.append("the plugin cache was rebuilt when the page opened - the first page with video waits for it")
    if result is None:
        problems.append("the YouTube page did not finish loading in %d s" % LOAD_LIMIT_S)
    else:
        if not result.get("video"): problems.append("no <video> on the page")
        if result.get("raf2s", 0) < 30: problems.append("the page hardly draws: %s animation frames in 2 s" % result.get("raf2s"))
        if result.get("video") and result.get("played", 0) < 1.0:
            problems.append("the video does not play: %.2f s of it in 2 s (%s)" % (result.get("played", 0), result.get("playError") or "error %s" % result.get("error")))


def finish(problems, work):
    if problems:
        for p in problems: say("FAIL: " + p)
        say("kept for a look: " + work)
        return 1
    shutil.rmtree(work, ignore_errors=True)
    say("passed")
    return 0


def setup_work():
    work = tempfile.mkdtemp(prefix="lyra-smoke-")
    prof = os.path.join(work, "profile")
    os.makedirs(prof)
    probe = os.path.join(work, "probe.js")
    open(probe, "w", encoding="utf-8").write(PROBE)
    env = dict(os.environ, LOCALAPPDATA=prof, LYRA_PROBE=probe)
    registry = os.path.join(prof, "Lyra", "gstreamer-registry.bin")
    mtime = lambda: os.path.getmtime(registry) if os.path.exists(registry) else None
    return work, prof, env, os.path.join(prof, "Lyra", "tabs.log"), mtime


def check_app(app):
    """--app: a built Lyra folder as it is (no installers): the plugin cache built the way the
    installer builds it (Lyra.exe --gst-registry), then the same page checks. For an engine that is
    not released yet (tools/update-engine.py: run-next)."""
    app = os.path.abspath(app)
    if not os.path.exists(os.path.join(app, "Lyra.exe")): raise SystemExit("no Lyra.exe in " + app)
    work, prof, env, log_path, mtime = setup_work()
    say("%s, profile in %s" % (app, work))
    problems = []
    try:
        t = time.time()
        subprocess.run([os.path.join(app, "Lyra.exe"), "--gst-registry"], env=env, cwd=app, timeout=180)
        built = mtime()
        say("plugin cache: %s in %.0f s" % ("built" if built else "NOT built", time.time() - t))
        if not built: problems.append("Lyra.exe --gst-registry did not build the plugin cache")
        result, secs, lines = run_page(app, env, log_path, LOAD_LIMIT_S)
        kill_under(app)
        check_page(result, secs, lines, built is not None and mtime() != built, problems)
    finally:
        kill_under(app)
    return finish(problems, work)


def main():
    args = sys.argv[1:]
    def opt(name):
        return args[args.index(name) + 1] if name in args and args.index(name) + 1 < len(args) else None
    if opt("--app"):
        if lyra_window_open():
            raise SystemExit("Lyra is open: close it first (a second Lyra hands its page to the open one)")
        return check_app(opt("--app"))
    new = opt("--to") or os.path.join(DIST, "LyraSetup.exe")
    ver = re.search(r"VERSION (\d+\.\d+\.\d+)", open(os.path.join(LYRA, "CMakeLists.txt"), encoding="utf-8").read()).group(1)
    old = opt("--from") or previous_setup(".".join(map(str, vkey(new))) if vkey(new) else ver)
    new, old = os.path.abspath(new), old and os.path.abspath(old)
    if not os.path.exists(new): raise SystemExit("no " + new)
    if not old or not os.path.exists(old): raise SystemExit("no earlier dist\\LyraSetup-<version>.exe to update from (--from)")
    if lyra_window_open():
        raise SystemExit("Lyra is open: close it first (a second Lyra hands its page to the open one)")

    work, prof, env, log_path, mtime = setup_work()
    inst = os.path.join(work, "inst")
    say("%s -> %s in %s" % (os.path.basename(old), os.path.basename(new), work))
    problems = []
    try:
        say("installing the earlier version, opening YouTube in it once")
        install(old, inst, env)
        subprocess.Popen([os.path.join(inst, "Lyra.exe"), VIDEO], env=env, cwd=inst)
        t, seen, still = time.time(), None, 0
        while time.time() - t < OLD_WARMUP_S and still < 5:     # until its cache is written and stays
            time.sleep(1)
            m = mtime()
            still = still + 1 if m is not None and m == seen else 0
            seen = m
        kill_under(inst)
        if mtime() is None: say("note: the earlier version left no plugin cache (a first install, then)")

        say("updating")
        before = mtime()
        secs = install(new, inst, env)
        after_install = mtime()
        say("installer: %.0f s; plugin cache %s" % (secs, "built" if after_install and after_install != before else "NOT built"))
        if not after_install or after_install == before:
            problems.append("the installer did not build the plugin cache (Lyra.exe --gst-registry)")

        say("first start after the update, YouTube")
        result, secs, lines = run_page(inst, env, log_path, LOAD_LIMIT_S)
        kill_under(inst)
        check_page(result, secs, lines, mtime() != after_install, problems)
    finally:
        kill_under(inst)

    return finish(problems, work)


if __name__ == "__main__":
    sys.exit(main())
