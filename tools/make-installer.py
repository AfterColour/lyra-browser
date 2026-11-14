"""Builds dist\\LyraSetup.exe: the installer with every Lyra file inside.

    python tools\\make-installer.py          (after build.bat)

1. tools\\package.py collects the install into dist\\Lyra-<version>\\
2. every file is compressed with LZMS (the Windows Compression API, so the installer can
   unpack it with nothing extra) on all CPU cores
3. the files go into a copy of build\\installer\\LyraSetup.exe as the resource PAYLOAD
   (format: see installer\\Payload.cpp); the same exe without files becomes Uninstall.exe
Writes dist\\LyraSetup-<version>.exe and dist\\LyraSetup.exe (the same file).
"""
import ctypes, hashlib, os, shutil, struct, subprocess, sys, time
from concurrent.futures import ThreadPoolExecutor
from ctypes import wintypes

LYRA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STUB = os.path.join(LYRA, "build", "installer", "LyraSetup.exe")
COMPRESS_ALGORITHM_LZMS = 5

cabinet = ctypes.WinDLL("cabinet", use_last_error=True)
cabinet.CreateCompressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
cabinet.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                             ctypes.POINTER(ctypes.c_size_t)]
cabinet.CloseCompressor.argtypes = [ctypes.c_void_p]
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.BeginUpdateResourceW.restype = ctypes.c_void_p
k32.BeginUpdateResourceW.argtypes = [wintypes.LPCWSTR, wintypes.BOOL]
k32.UpdateResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, wintypes.LPCWSTR, wintypes.WORD, ctypes.c_void_p, wintypes.DWORD]
k32.EndUpdateResourceW.argtypes = [ctypes.c_void_p, wintypes.BOOL]


def lzms(data):
    """(method, bytes): LZMS-packed, or stored when packing does not help."""
    if len(data) < 64:
        return 0, data
    h = ctypes.c_void_p()
    if not cabinet.CreateCompressor(COMPRESS_ALGORITHM_LZMS, None, ctypes.byref(h)):
        raise OSError(ctypes.get_last_error(), "CreateCompressor")
    try:
        cap = len(data) + len(data) // 8 + 65536
        out = ctypes.create_string_buffer(cap)
        size = ctypes.c_size_t()
        if not cabinet.Compress(h, data, len(data), out, cap, ctypes.byref(size)):
            raise OSError(ctypes.get_last_error(), "Compress")
        packed = out.raw[:size.value]
    finally:
        cabinet.CloseCompressor(h)
    return (1, packed) if len(packed) < len(data) else (0, data)


def main():
    if not os.path.exists(STUB):
        sys.exit("[installer] build\\installer\\LyraSetup.exe not found - run build.bat first")
    subprocess.run([sys.executable, os.path.join(LYRA, "tools", "package.py")], check=True)
    import re
    ver = re.search(r"project\(Lyra VERSION ([0-9.]+)", open(os.path.join(LYRA, "CMakeLists.txt"), encoding="utf-8").read()).group(1)
    src = os.path.join(LYRA, "dist", "Lyra-" + ver)

    files = []                                   # (path inside the install, source file)
    for root, _, names in os.walk(src):
        for n in sorted(names):
            p = os.path.join(root, n)
            files.append((os.path.relpath(p, src), p))
    files.sort(key=lambda f: (f[0].count("\\"), f[0].lower()))
    files.append(("Uninstall.exe", STUB))

    t0 = time.time()
    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        packed = list(pool.map(lambda f: lzms(open(f[1], "rb").read()), files))
    total = sum(os.path.getsize(f[1]) for f in files)

    head = [b"LYRAPAY1", struct.pack("<I", len(files))]
    v = ver.encode()
    head.append(struct.pack("<H", len(v)) + v)
    for (rel, path), (method, data) in zip(files, packed):
        name = rel.encode("utf-8")
        head.append(struct.pack("<H", len(name)) + name + struct.pack("<BQQ", method, os.path.getsize(path), len(data)))
    payload = b"".join(head) + b"".join(d for _, d in packed)

    out = os.path.join(LYRA, "dist", "LyraSetup-%s.exe" % ver)
    shutil.copy2(STUB, out)
    h = k32.BeginUpdateResourceW(out, False)
    buf = ctypes.create_string_buffer(payload, len(payload))
    if not h or not k32.UpdateResourceW(h, 10, "PAYLOAD", 0, buf, len(payload)) or not k32.EndUpdateResourceW(h, False):
        sys.exit("[installer] could not add the files to the installer (error %d)" % ctypes.get_last_error())
    shutil.copy2(out, os.path.join(LYRA, "dist", "LyraSetup.exe"))
    sha = hashlib.sha256(open(out, "rb").read()).hexdigest()
    print("[installer] %s: %d files, %.1f MB -> %.1f MB (%.0f s), SHA-256 %s" % (
        os.path.relpath(out, LYRA), len(files), total / 2**20, os.path.getsize(out) / 2**20, time.time() - t0, sha))


main()
