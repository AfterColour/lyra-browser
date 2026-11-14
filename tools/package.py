"""Collects what a Lyra install needs into dist\\Lyra-<version>\\ (run build.bat first).

Only what is really used goes in: the programs, the DLLs they (and the GStreamer
plugins) load, the plugins of the current WebKit build, Lyra's pages, WebKit's
resources, LICENSE.md and THIRD_PARTY_NOTICES.txt. Old DLLs left in run\\ by earlier
builds, .pdb files and test libraries stay out.

    python tools\\package.py
"""
import glob, os, re, shutil, struct, subprocess, sys

LYRA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN = os.path.join(LYRA, "run")
PROGRAMS = ["Lyra.exe", "WebKitWebProcess.exe", "WebKitNetworkProcess.exe",
            "WebKitGPUProcess.exe", "gst-plugin-scanner.exe"]
TOOL_PORTS = re.compile(r"^(vcpkg-.*|pkgconf|ffmpeg-bin2c|dirent|egl-registry|opengl-registry|opengl|openssl|zlib)$")


def fail(msg):
    print("[package] " + msg)
    sys.exit(1)


def cmake_cache(name):
    for line in open(os.path.join(LYRA, "build", "CMakeCache.txt"), encoding="utf-8", errors="replace"):
        if line.startswith(name + ":"):
            return line.split("=", 1)[1].strip()
    fail(name + " not in build\\CMakeCache.txt - run build.bat first")


def crt_dir():
    """The Visual C++ runtime DLLs (vcruntime140.dll, msvcp140.dll...) from the Visual Studio
    redistributable folder: shipped next to Lyra.exe so a PC without the VC++ Redistributable
    can run it (Microsoft allows this "app-local" copy)."""
    vswhere = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                           "Microsoft Visual Studio", "Installer", "vswhere.exe")
    vs = subprocess.run([vswhere, "-latest", "-products", "*", "-property", "installationPath"],
                        capture_output=True, text=True).stdout.strip()
    dirs = sorted(glob.glob(os.path.join(vs, "VC", "Redist", "MSVC", "*", "x64", "Microsoft.VC*.CRT")))
    if not dirs:
        fail("Visual C++ runtime not found in " + vs + "\\VC\\Redist")
    return dirs[-1]


def version():
    m = re.search(r"project\(Lyra VERSION ([0-9.]+)", open(os.path.join(LYRA, "CMakeLists.txt"), encoding="utf-8").read())
    return m.group(1)


def pe_imports(path):
    """DLL names a PE file imports (normal and delay-load), lower case."""
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    opt = pe + 24
    dirs = opt + (112 if struct.unpack_from("<H", d, opt)[0] == 0x20B else 96)
    sec = opt + struct.unpack_from("<H", d, pe + 20)[0]
    sections = [struct.unpack_from("<IIII", d, sec + i * 40 + 8)
                for i in range(struct.unpack_from("<H", d, pe + 6)[0])]  # vsize, va, rawsize, raw

    def offset(rva):
        for vsize, va, _, raw in sections:
            if va <= rva < va + max(vsize, 1):
                return rva - va + raw
        return None

    names = set()
    for index, entry, name_field in ((1, 20, 12), (13, 32, 4)):
        o = offset(struct.unpack_from("<I", d, dirs + index * 8)[0] or 0xFFFFFFFF)
        while o is not None:
            rva = struct.unpack_from("<I", d, o + name_field)[0]
            if not rva:
                break
            s = offset(rva)
            names.add(d[s:d.index(b"\0", s)].decode("latin1").lower())
            o += entry
    return names


def installed_ports(vcpkg_dir):
    """{port: version} of the libraries vcpkg installed for WebKit."""
    ports, cur = {}, {}
    for line in open(os.path.join(vcpkg_dir, "..", "vcpkg", "status"), encoding="utf-8") .read().split("\n") + [""]:
        if not line.strip():
            if cur.get("Status", "").endswith("install ok installed") and "Feature" not in cur \
                    and cur.get("Architecture") == "x64-windows-webkit":
                v = cur["Version"] + ("#" + cur["Port-Version"] if "Port-Version" in cur else "")
                ports[cur["Package"]] = v
            cur = {}
        elif ":" in line and not line.startswith(" "):
            k, v = line.split(":", 1)
            cur[k] = v.strip()
    return ports


def main():
    ver = version()
    webkit_build = cmake_cache("WEBKIT_BUILD_DIR").replace("/", "\\")
    webkit_src = os.path.dirname(os.path.dirname(webkit_build))
    vcpkg = os.path.join(webkit_build, "vcpkg_installed", "x64-windows-webkit")
    if not os.path.exists(os.path.join(RUN, "Lyra.exe")):
        fail("run\\Lyra.exe not found - run build.bat first")

    out = os.path.join(LYRA, "dist", "Lyra-" + ver)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)

    # Programs + every DLL reachable from them and from the GStreamer plugins.
    run_files = {f.lower(): f for f in os.listdir(RUN) if os.path.isfile(os.path.join(RUN, f))}
    source = {f: os.path.join(RUN, n) for f, n in run_files.items()}
    crt = crt_dir()
    for f in os.listdir(crt):
        source.setdefault(f.lower(), os.path.join(crt, f))
    plugins = sorted(f for f in os.listdir(os.path.join(vcpkg, "plugins", "gstreamer")) if f.endswith(".dll"))
    todo = [os.path.join(RUN, p) for p in PROGRAMS] + [os.path.join(RUN, "gstreamer-1.0", p) for p in plugins]
    files = set(p.lower() for p in PROGRAMS)
    while todo:
        for dll in pe_imports(todo.pop()):
            if dll in source and dll not in files:
                files.add(dll)
                todo.append(source[dll])
    for f in sorted(files):
        shutil.copy2(source[f], out)
    os.makedirs(os.path.join(out, "gstreamer-1.0"))
    for p in plugins:
        shutil.copy2(os.path.join(RUN, "gstreamer-1.0", p), os.path.join(out, "gstreamer-1.0"))
    for d in ("resources", "WebKit.resources"):
        shutil.copytree(os.path.join(RUN, d), os.path.join(out, d))
    shutil.copy2(os.path.join(LYRA, "LICENSE.md"), out)

    # THIRD_PARTY_NOTICES.txt: list + full license texts.
    ports = {p: v for p, v in installed_ports(vcpkg).items() if not TOOL_PORTS.match(p)}
    texts = [("WebKit (WebCore, JavaScriptCore) - LGPL 2.1", os.path.join(webkit_src, "Source", "WebCore", "LICENSE-LGPL-2.1")),
             ("WebKit - BSD-style license (Apple)", os.path.join(webkit_src, "Source", "WebCore", "LICENSE-APPLE")),
             ("ANGLE (libEGL, libGLESv2)", os.path.join(webkit_src, "Source", "ThirdParty", "ANGLE", "LICENSE")),
             ("Skia", os.path.join(webkit_src, "Source", "ThirdParty", "skia", "LICENSE"))]
    for f in sorted(os.listdir(os.path.join(webkit_src, "Source", "WTF"))):
        if f.startswith("LICENSE-"):
            texts.append(("WTF: " + f[8:].rsplit(".", 1)[0], os.path.join(webkit_src, "Source", "WTF", f)))
    ext = os.path.join(webkit_src, "Source", "WebInspectorUI", "UserInterface", "External")
    if os.path.isdir(ext):
        for name in sorted(os.listdir(ext)):
            for f in ("LICENSE", "LICENSE.txt", "LICENSE.md"):
                if os.path.isfile(os.path.join(ext, name, f)):
                    texts.append(("Web Inspector: " + name, os.path.join(ext, name, f)))
                    break
    for p in sorted(ports):
        c = os.path.join(vcpkg, "share", p, "copyright")
        if os.path.isfile(c):
            texts.append((p + " " + ports[p], c))
    texts.append(("Mozilla Readability (reader view)", os.path.join(LYRA, "resources", "readability-LICENSE.md")))
    missing = [t for t, p in texts if not os.path.isfile(p)]
    if missing:
        fail("license text not found: " + ", ".join(missing))

    comps = ["- WebKit (modified, see above), with ANGLE, Skia and the Web Inspector libraries"]
    comps += ["- %s %s" % (p, ports[p]) for p in sorted(ports)]
    comps += ["- Mozilla Readability"]
    header = open(os.path.join(LYRA, "legal", "notices-header.txt"), encoding="utf-8").read()
    body = [header.replace("{VERSION}", ver).replace("{COMPONENTS}", "\n".join(comps))]
    for title, path in texts:
        body.append("\n\n" + "=" * 78 + "\n" + title + "\n" + "=" * 78 + "\n\n")
        body.append(open(path, encoding="utf-8", errors="replace").read().strip() + "\n")
    with open(os.path.join(out, "THIRD_PARTY_NOTICES.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write("".join(body))

    # Summary + file list (the installer is built from this folder).
    listing, total = [], 0
    for root, _, names in os.walk(out):
        for n in names:
            p = os.path.join(root, n)
            listing.append(os.path.relpath(p, out))
            total += os.path.getsize(p)
    with open(os.path.join(LYRA, "dist", "Lyra-%s-files.txt" % ver), "w", encoding="utf-8") as f:
        f.write("\n".join(sorted(listing)) + "\n")
    print("[package] dist\\Lyra-%s: %d files, %.1f MB (left out of run\\: %s)" % (
        ver, len(listing), total / 2**20,
        ", ".join(sorted(set(run_files) - files - {"lyra.pdb"})) or "nothing"))


main()
