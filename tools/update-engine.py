"""Brings the engine up to date with upstream WebKit, apart from the engine Lyra is released with.

    python tools\\update-engine.py              fetch, merge, build, test (see below)
    python tools\\update-engine.py --status     how far behind upstream, and which files would conflict
    python tools\\update-engine.py --restart    throw away lyra-gstreamer-next and start it again
    python tools\\update-engine.py --adopt      move lyra-gstreamer to the tested lyra-gstreamer-next

The engine is C:\\WebKit\\WebKit, branch lyra-gstreamer: upstream WebKit (remote "origin") plus
Lyra's commits. This script works in a second checkout, C:\\WebKit\\WebKit-next (a git worktree,
branch lyra-gstreamer-next), so the released engine and run\\ are never touched:
 1. fetches upstream main and merges it into lyra-gstreamer-next (with lyra-gstreamer's newest
    commits too). A conflict stops here: resolve it in WebKit-next (git add the files), then run
    the script again - it finishes the merge and goes on;
 2. builds that engine (build-webkit-gst.bat with WEBKIT_SRC, into WebKit-next\\WebKitBuild);
    the first build there takes hours, later ones only what changed;
 3. builds Lyra against it into build-next\\ and run-next\\;
 4. runs the smoke test on run-next (tools\\smoke-update.py --app: YouTube loads, draws, plays).
Exit code 0: all passed; then --adopt fast-forwards lyra-gstreamer (nothing is pushed: release.py
pushes the engine with a release). Then build-webkit-gst.bat and build.bat as usual.
"""
import os, re, shutil, subprocess, sys, time

LYRA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(LYRA)
ENGINE = os.path.join(ROOT, "WebKit")
NEXT = os.path.join(ROOT, "WebKit-next")
BRANCH, NEXT_BRANCH, UPSTREAM = "lyra-gstreamer", "lyra-gstreamer-next", "origin/main"
NEED_FREE_GB = 25          # a first build in WebKit-next: about 20 GB


def say(msg): print("[engine] " + msg, flush=True)


def fail(msg, code=1):
    say("STOPPED: " + msg)
    sys.exit(code)


def git(*args, cwd=ENGINE, check=True):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if check and r.returncode != 0:
        fail("git %s: %s" % (" ".join(args), (r.stderr or r.stdout).strip()))
    return r.stdout.strip()


def upstream_info():
    behind = git("rev-list", "--count", "%s..%s" % (BRANCH, UPSTREAM))
    ours = git("rev-list", "--count", "%s..%s" % (UPSTREAM, BRANCH))
    date = git("log", "-1", "--format=%cs", UPSTREAM)
    return int(behind), int(ours), date


def status():
    behind, ours, date = upstream_info()
    say("upstream main %s (%s): %d commits Lyra does not have; Lyra's own: %d" % (git("rev-parse", "--short", UPSTREAM), date, behind, ours))
    r = subprocess.run(["git", "merge-tree", "--write-tree", "--name-only", BRANCH, UPSTREAM], cwd=ENGINE,
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    lines = r.stdout.strip().splitlines()
    files = [l for l in lines[1:] if l and not l.startswith(("Auto-merging", "CONFLICT"))]
    if r.returncode == 0:
        say("the merge has no conflicts")
    else:
        say("the merge conflicts in %d file(s):" % len(files))
        for f in files: print("    " + f)
    if os.path.isdir(NEXT):
        say("%s: %s, %d commit(s) ahead of %s" % (NEXT_BRANCH, git("log", "-1", "--format=%h %cs %s", NEXT_BRANCH),
            int(git("rev-list", "--count", "%s..%s" % (BRANCH, NEXT_BRANCH))), BRANCH))


def unmerged():
    return [f for f in git("diff", "--name-only", "--diff-filter=U", cwd=NEXT).splitlines() if f]


def stop_on_conflict():
    files = unmerged()
    if not files: return
    say("conflicts in %d file(s), in %s:" % (len(files), NEXT))
    for f in files: print("    " + f)
    fail("resolve them there (edit, then git add <file>), and run this script again: it finishes the merge", 2)


def merge(what, message):
    r = subprocess.run(["git", "merge", "--no-ff", "-m", message, what], cwd=NEXT, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    if r.returncode != 0:
        stop_on_conflict()
        fail("git merge %s: %s" % (what, (r.stderr or r.stdout).strip()))


def prepare():
    """WebKit-next with upstream merged in; stops on a conflict."""
    if not os.path.isdir(NEXT):
        free = shutil.disk_usage(ROOT).free / 2**30
        if free < NEED_FREE_GB:
            fail("%.0f GB free on the disk, a build in WebKit-next needs about %d" % (free, NEED_FREE_GB))
        has_branch = bool(git("branch", "--list", NEXT_BRANCH))
        say("creating %s (%s)" % (NEXT, NEXT_BRANCH))
        if has_branch: git("worktree", "add", NEXT, NEXT_BRANCH)
        else: git("worktree", "add", "-b", NEXT_BRANCH, NEXT, BRANCH)

    if os.path.exists(os.path.join(NEXT, git("rev-parse", "--git-path", "MERGE_HEAD", cwd=NEXT))):
        stop_on_conflict()
        say("finishing the merge")
        git("commit", "--no-edit", cwd=NEXT)
    elif git("status", "--porcelain", "--untracked-files=no", cwd=NEXT):
        fail("%s has uncommitted changes: commit or drop them first" % NEXT)

    newer = int(git("rev-list", "--count", "%s..%s" % (NEXT_BRANCH, BRANCH)))
    if newer:
        say("taking %d newer commit(s) of %s" % (newer, BRANCH))
        merge(BRANCH, "Merge %s" % BRANCH)

    behind = int(git("rev-list", "--count", "%s..%s" % (NEXT_BRANCH, UPSTREAM)))
    if behind:
        sha, date = git("rev-parse", "--short", UPSTREAM), git("log", "-1", "--format=%cs", UPSTREAM)
        say("merging upstream WebKit %s (%s): %d commit(s)" % (sha, date, behind))
        merge(UPSTREAM, "Merge upstream WebKit %s (%s)" % (sha, date))
    else:
        say("%s already has upstream main" % NEXT_BRANCH)


def memory_gb():
    import ctypes
    class MS(ctypes.Structure):
        _fields_ = [("len", ctypes.c_ulong), ("load", ctypes.c_ulong), ("total", ctypes.c_ulonglong),
                    ("avail", ctypes.c_ulonglong), ("pf", ctypes.c_ulonglong), ("apf", ctypes.c_ulonglong),
                    ("virt", ctypes.c_ulonglong), ("avirt", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]
    m = MS(); m.len = ctypes.sizeof(MS)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m))
    return m.total / 2**30


def run_bat(bat, env, log_hint):
    t = time.time()
    r = subprocess.run("\"%s\" < nul" % bat, shell=True, env=env, cwd=os.path.dirname(bat),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    mins = (time.time() - t) / 60
    return r.returncode == 0 and "FAILED" not in r.stdout, r.stdout, mins


def build():
    say("building the engine in %s (the first time: hours)" % NEXT)
    env = dict(os.environ, WEBKIT_SRC=NEXT)
    env.pop("VSINSTALLDIR", None)
    if "WEBKIT_JOBS" not in env:
        # A full build compiles the big unified sources: about 2.5 GB of memory each. Ninja's default
        # (cores + 2) ran clang out of memory ("LLVM ERROR: out of memory") on 23 GB / 20 threads.
        env["WEBKIT_JOBS"] = str(max(2, min(os.cpu_count() or 4, int(memory_gb() / 2.5))))
        say("parallel jobs: %s (WEBKIT_JOBS to change)" % env["WEBKIT_JOBS"])
    ok, out, mins = run_bat(os.path.join(ROOT, "build-webkit-gst.bat"), env, "")
    if not ok:
        log = os.path.join(NEXT, "WebKitBuild", "ReleaseGst-build.log")
        if "CONFIGURE FAILED" in out or not os.path.exists(log):
            print(out[-4000:])
            fail("configuring the engine failed (%.0f min; often a vcpkg port: see the error above); "
                 "log: %s" % (mins, os.path.join(NEXT, "WebKitBuild", "ReleaseGst-configure.log")), 3)
        errors = []
        if os.path.exists(log):
            errors = [l.rstrip() for l in open(log, encoding="utf-8", errors="replace") if re.search(r"\berror\b", l)]
        print("\n".join(errors[:40]) or out[-3000:])
        fail("the engine did not build (%.0f min); full log: %s" % (mins, log), 3)
    say("engine built in %.0f min" % mins)

    say("building Lyra against it into build-next\\, run-next\\")
    env = dict(os.environ, LYRA_WEBKITDIR=os.path.join(NEXT, "WebKitBuild", "ReleaseGst"),
               LYRA_BUILDDIR="build-next", LYRA_RUNDIR="run-next")
    env.pop("VSINSTALLDIR", None)
    ok, out, mins = run_bat(os.path.join(LYRA, "build.bat"), env, "")
    if not ok:
        print("\n".join(l for l in out.splitlines() if re.search(r"error|FAILED", l))[:6000] or out[-3000:])
        fail("Lyra did not build against the new engine: an engine API Lyra uses changed", 4)
    say("Lyra built in %.1f min" % mins)


def test():
    say("smoke test on run-next")
    r = subprocess.run([sys.executable, os.path.join(LYRA, "tools", "smoke-update.py"), "--app", os.path.join(LYRA, "run-next")])
    if r.returncode != 0:
        fail("the smoke test failed on the new engine", 5)


def adopt():
    if git("status", "--porcelain", "--untracked-files=no", "--", "Source", "WebKitLibraries"):
        fail("%s has uncommitted changes in Source/" % ENGINE)
    if git("rev-parse", "--abbrev-ref", "HEAD") != BRANCH:
        fail("%s is not on %s" % (ENGINE, BRANCH))
    if subprocess.run(["git", "merge-base", "--is-ancestor", BRANCH, NEXT_BRANCH], cwd=ENGINE).returncode != 0:
        fail("%s has commits %s lacks: run this script again first" % (BRANCH, NEXT_BRANCH))
    git("merge", "--ff-only", NEXT_BRANCH)
    say("%s is now %s. Next: C:\\WebKit\\build-webkit-gst.bat and build.bat (nothing pushed)"
        % (BRANCH, git("log", "-1", "--format=%h %s")))


def main():
    args = sys.argv[1:]
    if not os.path.isdir(os.path.join(ENGINE, ".git")) and not os.path.isfile(os.path.join(ENGINE, ".git")):
        fail("no engine checkout at " + ENGINE)
    if "--adopt" in args:
        return adopt()
    say("fetching upstream WebKit")
    git("fetch", "--quiet", "origin", "main")
    if "--status" in args:
        return status()
    if "--restart" in args:
        if os.path.isdir(NEXT):
            say("removing " + NEXT)
            git("worktree", "remove", "--force", NEXT)
        if git("branch", "--list", NEXT_BRANCH):
            git("branch", "-D", NEXT_BRANCH)
    behind, ours, date = upstream_info()
    say("upstream main is from %s; %s lacks %d of its commits" % (date, BRANCH, behind))
    prepare()
    build()
    test()
    say("all passed. To take it: python tools\\update-engine.py --adopt")


if __name__ == "__main__":
    main()
