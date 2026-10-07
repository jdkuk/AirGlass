"""Cross-check AirGlass's bplist codec against Python's plistlib (independent implementation)."""
import plistlib
import subprocess
import sys
import os

d = sys.argv[1]
os.makedirs(d, exist_ok=True)
py = {
    "streams": [{"type": 110, "streamConnectionID": 11653283748123456789, "latencyMs": 90}],
    "name": "Taylor’s iPhone",
    "ekey": bytes(range(72)),
    "timingPort": 51234,
    "isScreenMirroringSession": True,
    "refresh": 1 / 60,
}
with open(os.path.join(d, "py.bplist"), "wb") as f:
    plistlib.dump(py, f, fmt=plistlib.FMT_BINARY)

rc = subprocess.run([sys.argv[2], "bplist", d]).returncode
if rc != 0:
    print("C++ side failed")
    sys.exit(1)

with open(os.path.join(d, "cpp.bplist"), "rb") as f:
    c = plistlib.load(f)
assert c["small"] == 7 and c["u16"] == 4000 and c["u32"] == 123456789, c
assert c["big"] == 0x0123456789ABCDEF, hex(c["big"])
assert c["huge"] == 0xF123456789ABCDEF, hex(c["huge"])
assert abs(c["real"] - 1 / 120) < 1e-15
assert c["yes"] is True and c["no"] is False
assert c["ascii"] == "AppleTV3,2"
assert c["unicode"] == "Taylor’s iPhone \U0001F4F1", repr(c["unicode"])
assert c["data"] == bytes((i * 7) & 0xFF for i in range(72))
assert len(c["streams"]) == 20 and c["streams"][19] == {"type": 119, "name": "stream19"}
print("PASS python cross-check (C++ writer -> plistlib reader, plistlib writer -> C++ reader)")
