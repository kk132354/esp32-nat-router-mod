#!/usr/bin/env python3
"""WireGuard-Schluesselpaar erzeugen, ohne dass `wg` installiert sein muss.

Nur Python-Standardbibliothek (eigene X25519-Implementierung nach RFC 7748).
Aufruf:  python3 wgkeys.py            -> privater + oeffentlicher Schluessel
         python3 wgkeys.py --psk      -> zusaetzlich ein Preshared Key
         python3 wgkeys.py --selftest -> RFC-7748-Testvektor pruefen
"""
import base64
import os
import sys

P = 2**255 - 19
A24 = 121665


def x25519(k: bytes, u: bytes) -> bytes:
    k = bytearray(k)
    k[0] &= 248
    k[31] &= 127
    k[31] |= 64
    kn = int.from_bytes(k, "little")
    x1 = int.from_bytes(u, "little") & ((1 << 255) - 1)
    x2, z2, x3, z3 = 1, 0, x1, 1
    swap = 0
    for t in reversed(range(255)):
        kt = (kn >> t) & 1
        swap ^= kt
        if swap:
            x2, x3 = x3, x2
            z2, z3 = z3, z2
        swap = kt
        a = (x2 + z2) % P
        aa = a * a % P
        b = (x2 - z2) % P
        bb = b * b % P
        e = (aa - bb) % P
        c = (x3 + z3) % P
        d = (x3 - z3) % P
        da = d * a % P
        cb = c * b % P
        x3 = (da + cb) ** 2 % P
        z3 = x1 * (da - cb) ** 2 % P
        x2 = aa * bb % P
        z2 = e * (aa + A24 * e) % P
    if swap:
        x2, x3 = x3, x2
        z2, z3 = z3, z2
    return (x2 * pow(z2, P - 2, P) % P).to_bytes(32, "little")


def selftest() -> bool:
    k = bytes.fromhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4")
    u = bytes.fromhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c")
    want = "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"
    return x25519(k, u).hex() == want


def b64(b: bytes) -> str:
    return base64.b64encode(b).decode()


def main() -> int:
    if "--selftest" in sys.argv:
        ok = selftest()
        print("selftest:", "OK" if ok else "FEHLER")
        return 0 if ok else 1
    if not selftest():
        print("Interner Selbsttest fehlgeschlagen", file=sys.stderr)
        return 1
    priv = os.urandom(32)
    pub = x25519(priv, (9).to_bytes(32, "little"))
    print("PrivateKey :", b64(priv))
    print("PublicKey  :", b64(pub))
    if "--psk" in sys.argv:
        print("PresharedKey:", b64(os.urandom(32)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
