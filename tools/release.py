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
"""
import glob, hashlib, os, re, shutil, subprocess, sys, zipfile

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

    step("1/5 checks")
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
    if subprocess.run([GH, "auth", "status"], capture_output=True).returncode != 0:
        fail("gh is not signed in - run: gh auth login")
    if run(["git", "remote", "get-url", ENGINE_REMOTE], cwd=WEBKIT, check=False, capture=True) == "":
        msg = "the engine has no '%s' remote (the fork github.com/AfterColour/lyra-webkit)" % ENGINE_REMOTE
        if not dry:
            fail(msg + " - make the fork first")
        print("  note: " + msg + " - needed before a real release")

    step("2/5 build")
    run(["cmd", "/c", "echo.| build.bat"], cwd=LYRA)
    run([sys.executable, os.path.join(LYRA, "tools", "make-installer.py")])
    setup = os.path.join(LYRA, "dist", "LyraSetup.exe")

    step("3/5 LGPL sources")
    sources = lgpl_sources(ver)
    print("  %s (%.0f MB)" % (os.path.relpath(sources, LYRA), os.path.getsize(sources) / 2**20))

    notes_file = os.path.join(LYRA, "release-notes", ver + ".md")
    sha = hashlib.sha256(open(setup, "rb").read()).hexdigest()
    if dry:
        step("dry run done: %s  SHA-256 %s" % (setup, sha))
        return

    step("4/5 engine -> github.com/AfterColour/lyra-webkit")
    run(["git", "tag", "-f", "lyra-" + tag, ENGINE_BRANCH], cwd=WEBKIT)
    run(["git", "push", ENGINE_REMOTE, ENGINE_BRANCH], cwd=WEBKIT)
    run(["git", "push", "-f", ENGINE_REMOTE, "lyra-" + tag], cwd=WEBKIT)

    step("5/5 GitHub release " + tag)
    cmd = [GH, "release", "create", tag, setup, sources, "-R", REPO, "--title", "Lyra " + ver, "--target", "main"]
    cmd += ["--notes-file", notes_file] if os.path.exists(notes_file) else ["--notes", "Lyra %s\n\nSHA-256 of LyraSetup.exe: %s" % (ver, sha)]
    run(cmd)
    run(["git", "fetch", "-q", "--tags", "origin"], cwd=LYRA)
    step("done: https://github.com/%s/releases/tag/%s" % (REPO, tag))


main()
