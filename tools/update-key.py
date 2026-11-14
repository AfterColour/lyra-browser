"""The key that signs Lyra's update manifest (docs/update.json).

Lyra installs an update only if the manifest is signed with this key (ECDSA P-256,
the Windows CNG / BCrypt API - nothing extra to install):

    python tools\\update-key.py new        make the key pair (once)
    python tools\\update-key.py check      sign and verify a test message

The PRIVATE key is C:\\WebKit\\keys\\lyra-update-private.key - outside the repository,
never commit it. Keep a copy somewhere safe (a USB stick, a password manager): without
it no new version can be signed, and installed copies would need a manual update.
The PUBLIC key goes into src\\app\\UpdateKey.h, compiled into Lyra.
"""
import base64, ctypes, hashlib, os, sys
from ctypes import wintypes

LYRA = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KEY_FILE = os.path.join(os.path.dirname(LYRA), "keys", "lyra-update-private.key")
HEADER = os.path.join(LYRA, "src", "app", "UpdateKey.h")

bc = ctypes.WinDLL("bcrypt")
H = ctypes.c_void_p
for name, args in {
    "BCryptOpenAlgorithmProvider": [ctypes.POINTER(H), wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.ULONG],
    "BCryptGenerateKeyPair": [H, ctypes.POINTER(H), wintypes.ULONG, wintypes.ULONG],
    "BCryptFinalizeKeyPair": [H, wintypes.ULONG],
    "BCryptExportKey": [H, H, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG],
    "BCryptImportKeyPair": [H, H, wintypes.LPCWSTR, ctypes.POINTER(H), ctypes.c_void_p, wintypes.ULONG, wintypes.ULONG],
    "BCryptSignHash": [H, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG, ctypes.POINTER(wintypes.ULONG), wintypes.ULONG],
    "BCryptVerifySignature": [H, ctypes.c_void_p, ctypes.c_void_p, wintypes.ULONG, ctypes.c_void_p, wintypes.ULONG, wintypes.ULONG],
    "BCryptDestroyKey": [H], "BCryptCloseAlgorithmProvider": [H, wintypes.ULONG],
}.items():
    getattr(bc, name).argtypes = args
    getattr(bc, name).restype = ctypes.c_long


def ok(status, what):
    if status != 0:
        raise OSError("%s failed: 0x%08X" % (what, status & 0xFFFFFFFF))


def _alg():
    a = H()
    ok(bc.BCryptOpenAlgorithmProvider(ctypes.byref(a), "ECDSA_P256", None, 0), "open ECDSA_P256")
    return a


def _export(key, kind):
    n = wintypes.ULONG()
    ok(bc.BCryptExportKey(key, None, kind, None, 0, ctypes.byref(n), 0), "export size")
    buf = ctypes.create_string_buffer(n.value)
    ok(bc.BCryptExportKey(key, None, kind, buf, n, ctypes.byref(n), 0), "export")
    return buf.raw[:n.value]


def _import(blob, kind):
    a, k = _alg(), H()
    ok(bc.BCryptImportKeyPair(a, None, kind, ctypes.byref(k), blob, len(blob), 0), "import key")
    return a, k


def message(version, url, size, sha256):
    """What the signature covers (Lyra builds the same text, see src/app/Updater.cpp)."""
    return ("lyra-update/1\n%s\n%s\n%d\n%s" % (version, url, size, sha256.lower())).encode("utf-8")


def sign(msg):
    if not os.path.exists(KEY_FILE):
        raise SystemExit("[update-key] no private key at %s - run: python tools\\update-key.py new" % KEY_FILE)
    a, k = _import(open(KEY_FILE, "rb").read(), "ECCPRIVATEBLOB")
    digest = hashlib.sha256(msg).digest()
    n = wintypes.ULONG()
    ok(bc.BCryptSignHash(k, None, digest, 32, None, 0, ctypes.byref(n), 0), "sign size")
    sig = ctypes.create_string_buffer(n.value)
    ok(bc.BCryptSignHash(k, None, digest, 32, sig, n, ctypes.byref(n), 0), "sign")
    bc.BCryptDestroyKey(k); bc.BCryptCloseAlgorithmProvider(a, 0)
    return base64.b64encode(sig.raw[:n.value]).decode()


def verify(msg, sig_b64, public_blob):
    a, k = _import(public_blob, "ECCPUBLICBLOB")
    digest, sig = hashlib.sha256(msg).digest(), base64.b64decode(sig_b64)
    r = bc.BCryptVerifySignature(k, None, digest, 32, sig, len(sig), 0)
    bc.BCryptDestroyKey(k); bc.BCryptCloseAlgorithmProvider(a, 0)
    return r == 0


def public_blob():
    a, k = _import(open(KEY_FILE, "rb").read(), "ECCPRIVATEBLOB")
    blob = _export(k, "ECCPUBLICBLOB")
    bc.BCryptDestroyKey(k); bc.BCryptCloseAlgorithmProvider(a, 0)
    return blob


def write_header(blob):
    rows = ["    " + ", ".join("0x%02X" % b for b in blob[i:i + 12]) + "," for i in range(0, len(blob), 12)]
    open(HEADER, "w", encoding="utf-8", newline="\n").write(
        "// Lyra's update signing key, PUBLIC part (BCRYPT_ECCPUBLIC_BLOB, ECDSA P-256).\n"
        "// Written by tools/update-key.py; the private key never leaves the release PC.\n"
        "#pragma once\n\n"
        "static const unsigned char kUpdatePublicKey[] = {\n" + "\n".join(rows) + "\n};\n")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "new":
        if os.path.exists(KEY_FILE):
            raise SystemExit("[update-key] %s already exists - not replacing it (a new key would orphan installed copies)" % KEY_FILE)
        a, k = _alg(), H()
        ok(bc.BCryptGenerateKeyPair(a, ctypes.byref(k), 256, 0), "generate")
        ok(bc.BCryptFinalizeKeyPair(k, 0), "finalize")
        priv, pub = _export(k, "ECCPRIVATEBLOB"), _export(k, "ECCPUBLICBLOB")
        os.makedirs(os.path.dirname(KEY_FILE), exist_ok=True)
        open(KEY_FILE, "wb").write(priv)
        write_header(pub)
        print("[update-key] private key: %s  (keep a safe copy, never commit)" % KEY_FILE)
        print("[update-key] public key:  %s" % os.path.relpath(HEADER, LYRA))
    elif cmd == "check":
        msg = message("9.9.9", "https://example.com/LyraSetup.exe", 123, "ab" * 32)
        s = sign(msg)
        good = verify(msg, s, public_blob())
        bad = verify(msg + b"x", s, public_blob())
        print("[update-key] signature ok: %s, tampered rejected: %s" % (good, not bad))
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
