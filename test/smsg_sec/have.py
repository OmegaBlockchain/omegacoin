# Sends an unsolicited smsgHave for an arbitrary (non bucket-aligned) time to a regtest node.
import socket, struct, time, hashlib, sys, os
MAGIC = bytes([0xfc, 0xc1, 0xb7, 0xdc])
def msg(cmd, payload=b""):
    chk = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    return MAGIC + cmd.encode().ljust(12, b"\0") + struct.pack("<I", len(payload)) + chk + payload
def addr():
    return struct.pack("<Q", 0) + b"\0" * 10 + b"\xff\xff" + bytes([127, 0, 0, 1]) + struct.pack(">H", 0)
def recv_msgs(s, wanted, deadline):
    buf = b""; seen = []
    s.settimeout(0.5)
    while time.time() < deadline and not set(wanted) <= set(seen):
        try:
            d = s.recv(65536)
        except socket.timeout:
            continue
        if not d: break
        buf += d
        while len(buf) >= 24:
            ln = struct.unpack("<I", buf[16:20])[0]
            if len(buf) < 24 + ln: break
            seen.append(buf[4:16].rstrip(b"\0").decode()); buf = buf[24 + ln:]
    return seen
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=5)
ver = struct.pack("<iQq", 70234, 0, int(time.time())) + addr() + addr() + os.urandom(8) + b"\x06/poc:/" + struct.pack("<i", 1) + b"\x01"
s.sendall(msg("version", ver))
seen = recv_msgs(s, ["version", "verack"], time.time() + 10)
s.sendall(msg("verack"))
t = int(time.time()) - 777          # inside retention, deliberately not bucket-aligned
body = struct.pack("<q", t) + os.urandom(16)
s.sendall(msg("smsgHave", bytes([len(body)]) + body))
after = recv_msgs(s, ["smsgWant"], time.time() + 6)
print("handshake:", sorted(set(seen)), "| got smsgWant back:", "smsgWant" in after, "| time:", t)
s.close()
