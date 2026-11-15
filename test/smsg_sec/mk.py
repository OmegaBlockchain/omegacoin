import sys, os, time, hmac, hashlib, struct
def build(fake_delta, payload_len=96):
    payload = os.urandom(payload_len)
    ts = int(time.time())
    iv = os.urandom(16)
    cpkR = bytes.fromhex("0279BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798")
    mac = os.urandom(32)
    npl = (payload_len + fake_delta) & 0xffffffff
    nonce = 0
    while True:
        body = bytes([2,1]) + b"\x00" + struct.pack("<q", ts) + iv + cpkR + mac + struct.pack("<I", nonce) + struct.pack("<I", npl)
        assert len(body) == 100
        civ = struct.pack("<I", nonce) * 8
        h = hmac.new(civ, body + payload, hashlib.sha256).digest()
        if h[31] == 0 and h[30] == 0 and ((~h[29]) & 7):
            hdr = h[:4] + body
            return hdr, payload, ts
        nonce += 1
def honest_msgid(hdr, payload, ts):
    r = hashlib.new("ripemd160", hdr + payload).digest()
    return (struct.pack(">q", ts) + r).hex()
d = int(sys.argv[1])
hdr, payload, ts = build(d)
print((hdr + payload).hex())
print(honest_msgid(hdr, payload, ts), file=sys.stderr)
