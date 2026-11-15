# Turns the "stall:" lines of tabs.log (app/StallWatch.cpp) into function names.
#
#   python tools/symbolize-stalls.py [tabs.log] [--last N]
#
# The default log is %LOCALAPPDATA%\Lyra\tabs.log. Needs cdb.exe (Debugging Tools for Windows);
# Lyra and WebKit symbols come from run\ and WebKit\WebKitBuild\ReleaseGst\bin, Windows' own from
# Microsoft's symbol server (cached in C:\WebKit\symcache).
import os, re, subprocess, sys
from collections import defaultdict

LYRA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEBKIT_BIN = os.path.join(os.path.dirname(LYRA), "WebKit", "WebKitBuild", "ReleaseGst", "bin")
RUN = os.path.join(LYRA, "run")
SYMPATH = ";".join([RUN, WEBKIT_BIN, r"srv*C:\WebKit\symcache*https://msdl.microsoft.com/download/symbols"])

def find_cdb():
    for base in (os.environ.get("ProgramFiles(x86)", ""), os.environ.get("ProgramFiles", "")):
        p = os.path.join(base, "Windows Kits", "10", "Debuggers", "x64", "cdb.exe")
        if os.path.isfile(p):
            return p
    sys.exit("cdb.exe not found: install Debugging Tools for Windows")

def module_path(name):
    for d in (RUN, WEBKIT_BIN, os.path.join(os.environ["SystemRoot"], "System32")):
        p = os.path.join(d, name)
        if os.path.isfile(p):
            return p
    return None

def main():
    args = sys.argv[1:]
    last = 20
    if "--last" in args:
        i = args.index("--last"); last = int(args[i + 1]); del args[i:i + 2]
    log = args[0] if args else os.path.join(os.environ["LOCALAPPDATA"], "Lyra", "tabs.log")
    lines = [l.rstrip("\n") for l in open(log, encoding="utf-8", errors="replace") if " stall:" in l]
    # keep the last N stalls (a stall is its "did not respond" line and the lines after it)
    starts = [i for i, l in enumerate(lines) if "did not respond" in l]
    if len(starts) > last:
        lines = lines[starts[-last]:]
    frames = defaultdict(set)
    for l in lines:
        for mod, off in re.findall(r"([\w.\-]+\.(?:dll|exe|DLL|EXE))\+0x([0-9A-Fa-f]+)", l):
            frames[mod].add(int(off, 16))
    cdb = find_cdb()
    names = {}
    for mod, offs in frames.items():
        path = module_path(mod)
        if not path:
            continue
        offs = sorted(offs)
        # cdb opens the module as a dump image at its preferred base; "ln" each address.
        stem = os.path.splitext(mod)[0]
        cmds = ";".join("ln %s+0x%X" % (stem, o) for o in offs) + ";q"
        out = subprocess.run([cdb, "-y", SYMPATH, "-z", path, "-c", cmds], capture_output=True, text=True, errors="replace").stdout
        found = re.findall(r"^\((?:[0-9a-f`]+)\)\s+(\S+)", out, re.M)
        for o, n in zip(offs, found):
            names["%s+0x%X" % (mod, o)] = n
    for l in lines:
        def sub(m):
            key = "%s+0x%X" % (m.group(1), int(m.group(2), 16))
            return names.get(key, m.group(0))
        print(re.sub(r"([\w.\-]+\.(?:dll|exe|DLL|EXE))\+0x([0-9A-Fa-f]+)", sub, l))

if __name__ == "__main__":
    main()
