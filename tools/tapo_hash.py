"""
Spočítá přihlašovací hash KLAP pro include/secrets.h (TAPO_AUTH_HASH), aby heslo
k účtu Tapo nemuselo být ve firmwaru v čitelné podobě.

    python tools/tapo_hash.py email@example.com
    (heslo se zadá skrytě na výzvu)

Hash = sha256(sha1(e-mail) + sha1(heslo)), stejně jako KlapTransportV2 v python-kasa.
Z hashe heslo zpět získat nejde, ale kdo hash má, může se pásku hlásit stejně
jako aplikace, proto secrets.h nepatří do repozitáře.
"""

import getpass
import hashlib
import sys


def klap_auth_hash(user: str, password: str) -> str:
    inner = hashlib.sha1(user.encode()).digest() + hashlib.sha1(password.encode()).digest()
    return hashlib.sha256(inner).hexdigest()


def main() -> int:
    if len(sys.argv) < 2:
        print("Použití: python tools/tapo_hash.py EMAIL", file=sys.stderr)
        return 2
    user = sys.argv[1].strip()
    password = getpass.getpass("Heslo k účtu Tapo: ")
    print()
    print('#define TAPO_USER "%s"' % user)
    print('#define TAPO_PASS ""')
    print('#define TAPO_AUTH_HASH "%s"' % klap_auth_hash(user, password))
    return 0


if __name__ == "__main__":
    sys.exit(main())
