"""Publishes a Lyra version on GitHub: release.bat runs this.

    python tools\\release.py              release the version in CMakeLists.txt
    python tools\\release.py --dry-run    do everything except publishing

Steps:
 1. checks: Lyra and the engine have nothing uncommitted, Lyra is pushed, the version
    was not released before, gh is signed in
 2. builds Lyra (build.bat) and the installer (tools\\make-installer.py)
 3. dist\\lyra-<v>-lgpl-sources.zip: the source of the LGPL libraries in the install
    (GStreamer, FFmpeg, GLib, gettext, libiconv) exactly as built - the LGPL asks for it
 4. the engine: its branch and a tag lyra-v<v> go to github.com/AfterColour/lyra-webkit
 5. a GitHub release v<v> in AfterColour/lyra-browser with LyraSetup.exe and the sources;
    the notes come from release-notes\\<v>.md (written by you) if it exists
 6. docs\\update.json - signed with the update key (tools\\update-key.py) - is committed and
    pushed: installed copies of Lyra find the new version there (GitHub Pages); with it
    docs\\releases.json, the notes of every version for the website's "What's new"
"""
import glob, hashlib, importlib.util, json, os, re, shutil, subprocess, sys, time, zipfile

LYRA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEBKIT = os.path.join(os.path.dirname(LYRA), "WebKit")
VCPKG_DOWNLOADS = os.path.join(os.path.dirname(LYRA), "vcpkg", "downloads")
REPO = "AfterColour/lyra-browser"
ENGINE_REMOTE, ENGINE_BRANCH = "lyra", "lyra-gstreamer"
GH = shutil.which("gh") or r"C:\Program Files\GitHub CLI\gh.exe"
LGPL_PORTS = ["gstreamer", "ffmpeg", "glib", "gettext", "libiconv"]   # source archives shipped


def step(msg): print("\n[release] " + msg, flush=True)
def fail(msg): sys.exit("\n[release] STOP: " + msg)
def run(cmd, cwd=None, check=True, capture=False):
    r = subprocess.run(cmd, cwd=cwd, capture_output=capture, text=True)
    if check and r.returncode != 0:
        fail("failed: " + " ".join(cmd) + ("\n" + (r.stderr or r.stdout) if capture else ""))
    return r.stdout.strip() if capture else r.returncode


def update_key():
    spec = importlib.util.spec_from_file_location("update_key", os.path.join(LYRA, "tools", "update-key.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def write_releases(ver):
    """docs/releases.json: the website's "What's new" - this version's notes added (or replaced)."""
    out = os.path.join(LYRA, "docs", "releases.json")
    try: items = json.load(open(out, encoding="utf-8"))
    except (OSError, ValueError): items = []
    notes_file = os.path.join(LYRA, "release-notes", ver + ".md")
    notes = open(notes_file, encoding="utf-8").read().replace("\r\n", "\n").strip() if os.path.exists(notes_file) else ""
    items = [r for r in items if r.get("version") != ver]
    items.append({"version": ver, "date": time.strftime("%Y-%m-%d"), "notes": notes})
    items.sort(key=lambda r: [int(x) for x in re.findall(r"\d+", r["version"])], reverse=True)
    open(out, "w", encoding="utf-8", newline="\n").write(json.dumps(items, ensure_ascii=False, indent=1) + "\n")
    return out


def write_manifest(ver, setup, out=None):
    """docs/update.json for the version just released, signed; returns its path."""
    uk = update_key()
    url = "https://github.com/%s/releases/download/v%s/LyraSetup.exe" % (REPO, ver)
    size = os.path.getsize(setup)
    sha = hashlib.sha256(open(setup, "rb").read()).hexdigest()
    msg = uk.message(ver, url, size, sha)
    sig = uk.sign(msg)
    if not uk.verify(msg, sig, uk.public_blob()):
        fail("the update signature does not verify - is src/app/UpdateKey.h from this key?")
    out = out or os.path.join(LYRA, "docs", "update.json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    data = {"version": ver, "url": url, "size": size, "sha256": sha, "sig": sig,
            "date": time.strftime("%Y-%m-%d"), "notes": "https://github.com/%s/releases/tag/v%s" % (REPO, ver)}
    open(out, "w", encoding="utf-8", newline="\n").write(json.dumps(data, indent=2) + "\n")
    return out


def version():
    return re.search(r"project\(Lyra VERSION ([0-9.]+)", open(os.path.join(LYRA, "CMakeLists.txt"), encoding="utf-8").read()).group(1)


def installed_ports():
    status = os.path.join(WEBKIT, "WebKitBuild", "ReleaseGst", "vcpkg_installed", "vcpkg", "status")
    ports, cur = {}, {}
    for line in open(status, encoding="utf-8").read().split("\n") + [""]:
        if not line.strip():
            if cur.get("Status", "").endswith("install ok installed") and "Feature" not in cur:
                ports[cur["Package"]] = cur["Version"]
            cur = {}
        elif ":" in line and not line.startswith(" "):
            k, v = line.split(":", 1)
            cur[k] = v.strip()
    return ports


def lgpl_sources(ver):
    """Zip of the LGPL libraries' source archives (from vcpkg's download cache) + the vcpkg
    manifest that says how they were built."""
    ports = installed_ports()
    out = os.path.join(LYRA, "dist", "lyra-%s-lgpl-sources.zip" % ver)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        for port in LGPL_PORTS:
            v = ports.get(port)
            if not v:
                fail("vcpkg port %s is not installed - is the engine built?" % port)
            vs = [v, v.replace(".", "_")]
            found = [f for f in glob.glob(os.path.join(VCPKG_DOWNLOADS, "*"))
                     if port.split("-")[0] in os.path.basename(f).lower() and any(x in os.path.basename(f) for x in vs)
                     and re.search(r"\.(tar\.gz|tgz|tar\.xz|tar\.bz2|zip)$", f)]
            if not found:
                fail("source archive of %s %s not found in %s" % (port, v, VCPKG_DOWNLOADS))
            for f in found:
                z.write(f, "sources/" + os.path.basename(f))
        for f in ("vcpkg.json", "vcpkg-configuration.json"):
            z.write(os.path.join(WEBKIT, f), "build/" + f)
        z.writestr("README.txt",
                   "Source code of the LGPL libraries in Lyra %s, exactly the versions in the installer.\n"
                   "They are built by vcpkg with the recipes pinned in build/vcpkg-configuration.json\n"
                   "(baselines of github.com/microsoft/vcpkg and github.com/WebKitForWindows/WebKitRequirements)\n"
                   "and the list in build/vcpkg.json. The WebKit engine itself, with Lyra's changes, is at\n"
                   "https://github.com/AfterColour/lyra-webkit (tag lyra-v%s).\n" % (ver, ver))
    return out


def main():
    dry = "--dry-run" in sys.argv
    ver = version()
    tag = "v" + ver
    step("Lyra %s%s" % (ver, " (dry run: nothing is published)" if dry else ""))

    step("1/6 checks")
    if run(["git", "status", "--porcelain"], cwd=LYRA, capture=True):
        fail("Lyra has uncommitted changes - commit them first (commit-lyra.bat)")
    if run(["git", "status", "--porcelain", "--", "Source", "WebKitLibraries"], cwd=WEBKIT, capture=True):
        fail("the engine has uncommitted changes in Source/ - commit them first")
    run(["git", "fetch", "-q", "origin"], cwd=LYRA)
    if run(["git", "rev-list", "--count", "origin/main..main"], cwd=LYRA, capture=True) != "0":
        fail("Lyra has commits that are not on GitHub yet - run git push")
    if run(["git", "tag", "-l", tag], cwd=LYRA, capture=True) or \
            subprocess.run([GH, "release", "view", tag, "-R", REPO], capture_output=True).returncode == 0:
        fail("%s is already released - raise the version in CMakeLists.txt (project(Lyra VERSION ...))" % tag)
    if not os.path.exists(update_key().KEY_FILE):
        fail("no update signing key - run: python tools\\update-key.py new")
    if subprocess.run([GH, "auth", "status"], capture_output=True).returncode != 0:
        fail("gh is not signed in - run: gh auth login")
    if run(["git", "remote", "get-url", ENGINE_REMOTE], cwd=WEBKIT, check=False, capture=True) == "":
        msg = "the engine has no '%s' remote (the fork github.com/AfterColour/lyra-webkit)" % ENGINE_REMOTE
        if not dry:
            fail(msg + " - make the fork first")
        print("  note: " + msg + " - needed before a real release")

    step("2/6 build")
    # Full path and input from nul ("echo.| build.bat" made cmd run it from the pipe, which
    # did not always find it); nul answers build.bat's closing pause.
    run(["cmd", "/c", os.path.join(LYRA, "build.bat") + " < nul"], cwd=LYRA)
    run([sys.executable, os.path.join(LYRA, "tools", "make-installer.py")])
    setup = os.path.join(LYRA, "dist", "LyraSetup.exe")

    step("3/6 LGPL sources")
    sources = lgpl_sources(ver)
    print("  %s (%.0f MB)" % (os.path.relpath(sources, LYRA), os.path.getsize(sources) / 2**20))

    notes_file = os.path.join(LYRA, "release-notes", ver + ".md")
    sha = hashlib.sha256(open(setup, "rb").read()).hexdigest()
    if dry:
        m = write_manifest(ver, setup, os.path.join(LYRA, "dist", "update.json"))
        print("  manifest (dist\\update.json, not published):\n" + open(m, encoding="utf-8").read())
        step("dry run done: %s  SHA-256 %s" % (setup, sha))
        return

    step("4/6 engine -> github.com/AfterColour/lyra-webkit")
    run(["git", "tag", "-f", "lyra-" + tag, ENGINE_BRANCH], cwd=WEBKIT)
    run(["git", "push", ENGINE_REMOTE, ENGINE_BRANCH], cwd=WEBKIT)
    run(["git", "push", "-f", ENGINE_REMOTE, "lyra-" + tag], cwd=WEBKIT)

    step("5/6 GitHub release " + tag)
    cmd = [GH, "release", "create", tag, setup, sources, "-R", REPO, "--title", "Lyra " + ver, "--target", "main"]
    cmd += ["--notes-file", notes_file] if os.path.exists(notes_file) else ["--notes", "Lyra %s\n\nSHA-256 of LyraSetup.exe: %s" % (ver, sha)]
    run(cmd)
    run(["git", "fetch", "-q", "--tags", "origin"], cwd=LYRA)

    step("6/6 update manifest -> docs/update.json")
    m = write_manifest(ver, setup)
    run(["git", "add", m, write_releases(ver)], cwd=LYRA)
    run(["git", "commit", "-q", "-m", "Lyra %s: update manifest" % ver], cwd=LYRA)
    run(["git", "push", "-q"], cwd=LYRA)
    step("done: https://github.com/%s/releases/tag/%s" % (REPO, tag))


main()
