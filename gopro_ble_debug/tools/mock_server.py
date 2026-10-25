#!/usr/bin/env python3
"""mock_server.py -- run the web UI against a fake RecLink + GoPro, no hardware.

    python3 tools/mock_server.py [--port 8080]   then open http://localhost:8080

Implements the firmware's /api/* surface. Writes to the GoPro Command / Query /
Settings characteristics are depacketized and answered with Open GoPro
responses, packetized exactly like a camera (20-byte packets), so the UI's
decoders see realistic traffic.
"""
import argparse
import json
import os
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

WWW = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "www")
GP = "b5f9{}-aa8d-11e3-9046-0002a5d5c51b"
T0 = time.time()

CHARS = [  # (svc, uuid, handle, props, cccd)
    (0, "0x2a00", 3, 0x02, 0),
    (1, GP.format("0002"), 0x10, 0x0A, 0), (1, GP.format("0003"), 0x12, 0x0A, 0),
    (1, GP.format("0004"), 0x14, 0x08, 0), (1, GP.format("0005"), 0x16, 0x22, 0x17),
    (2, GP.format("0072"), 0x20, 0x08, 0), (2, GP.format("0073"), 0x22, 0x10, 0x23),
    (2, GP.format("0074"), 0x25, 0x08, 0), (2, GP.format("0075"), 0x27, 0x10, 0x28),
    (2, GP.format("0076"), 0x2A, 0x08, 0), (2, GP.format("0077"), 0x2C, 0x10, 0x2D),
    (3, GP.format("0091"), 0x40, 0x08, 0), (3, GP.format("0092"), 0x42, 0x10, 0x43),
]
SVCS = [("0x1800", 1, 9), (GP.format("0001"), 0x0F, 0x1F), ("0xfea6", 0x1F, 0x3F), (GP.format("0090"), 0x3F, 0x4F)]
H = {c[1][4:8] if c[1].startswith("b5f9") else c[1]: c[2] for c in CHARS}

lock = threading.Lock()
events, seq = [], [1]
state = {"link": "idle", "keepalive": False, "discovered": False, "reasm": {}}
statuses = {10: b"\x00", 70: b"\x55", 2: b"\x03", 33: b"\x00", 30: b"GP24500000", 54: (31_000_000).to_bytes(8, "big")}
settings = {2: b"\x01", 3: b"\x08", 121: b"\x00", 91: b"\x03"}


def ms():
    return int((time.time() - T0) * 1000)


def push(y, h=0, st=0, f=0, data=b"", text=None):
    with lock:
        e = {"s": seq[0], "t": ms(), "y": y, "h": h, "st": st, "f": f}
        if text is not None:
            e["x"] = text
        else:
            e["d"] = data.hex()
        seq[0] += 1
        events.append(e)
        del events[:-96]


def packets(msg):
    if len(msg) < 20:
        return [bytes([len(msg)]) + msg]
    out, off, cnt = [], 0, 0
    while off < len(msg):
        hdr = bytes([0x20 | (len(msg) >> 8) & 0x1F, len(msg) & 0xFF]) if off == 0 else bytes([0x80 | (cnt & 0xF)])
        if off:
            cnt += 1
        take = min(len(msg) - off, 20 - len(hdr))
        out.append(hdr + msg[off:off + take])
        off += take
    return out


def notify(short, msg):
    for p in packets(msg):
        push(2, H[short], data=p)


def tlv(d):
    return b"".join(bytes([k, len(v)]) + v for k, v in d.items())


def lv(*vals):
    return b"".join(bytes([len(v)]) + v for v in vals)


def camera(short, msg):
    """Answer one depacketized request like a HERO12 would."""
    if short == "0072":
        cid = msg[0]
        if cid == 0x3C:
            notify("0073", b"\x3c\x00" + lv((62).to_bytes(4, "big"), b"HERO12 Black", b"\0\0\0\0", b"H23.01.01.10.00",
                                             b"C3501324500000", b"GP24500000", b"2674f7f65f78"))
        elif cid == 0x51:
            notify("0073", b"\x51\x00\x01\x02\x01\x00")
        elif cid == 0x10:
            notify("0073", b"\x10\x00\x0a\x07\xe9\x0a\x19\x0c\x22\x05\x00\x3c\x00")
        elif cid in (0xF1,):
            notify("0073", bytes([0xF1, msg[1] | 0x80]) + b"\x08\x01")
        else:
            if cid == 0x01:
                statuses[10] = bytes([msg[2]])
                notify("0077", b"\x93\x00" + tlv({10: statuses[10]}))
            notify("0073", bytes([cid, 0]))
    elif short == "0074":
        if msg[0] != 0x5B and len(msg) >= 3:
            settings[msg[0]] = msg[2:2 + msg[1]]
            notify("0077", b"\x92\x00" + tlv({msg[0]: settings[msg[0]]}))
        notify("0075", bytes([msg[0], 0]))
    elif short == "0076":
        q = msg[0]
        if q == 0xF5 and msg[1] == 0x6C:
            schema = b"camera-capabilities/v1.0"   # ResponseGetCameraCapabilities
            notify("0077", b"\xf5\xec\x08\x01\x12" + bytes([len(schema)]) + schema + b"\x1a\x02\x08\x01")
        elif q in (0x13, 0x53):
            notify("0077", bytes([q, 0]) + tlv({k: v for k, v in statuses.items() if len(msg) == 1 or k in msg[1:]}))
        elif q in (0x12, 0x52):
            notify("0077", bytes([q, 0]) + tlv({k: v for k, v in settings.items() if len(msg) == 1 or k in msg[1:]}))
        elif q in (0x32, 0x62):
            notify("0077", bytes([q, 0]) + b"\x02\x01\x01\x02\x01\x04\x02\x01\x09\x03\x01\x08\x03\x01\x05")
        else:
            notify("0077", bytes([q, 0]))
    elif short == "0091" and msg[:2] == b"\x03\x01":
        notify("0092", b"\x03\x81\x08\x01")


class Api(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b""
        try:
            return json.loads(raw or b"{}")
        except ValueError:
            return {}

    def send(self, obj, code=200, ctype="application/json"):
        data = obj if isinstance(obj, bytes) else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/api/status":
            slot = {"label": "ota_1", "valid": True, "project": "gopro_ble_debug", "version": "dbg-0.1.0", "built": "mock", "idf": "v5.4.1", "ota_state": "valid"}
            other = dict(slot, label="ota_0", project="fc_cam_bridge", version="0.1.0")
            return self.send({"fw": "dbg-0.1.0", "project": "gopro_ble_debug", "idf": "v5.4.1", "uptime_s": ms() // 1000, "heap": 90000, "heap_min": 60000,
                              "ssid": "RECLINK-DBG-MOCK", "clients": 1, "running": slot, "other": other, "ev_next": seq[0],
                              "ble": {"synced": True, "scanning": False, "link": state["link"], "peer": "26:74:F7:F6:5F:78" if state["link"] != "idle" else "",
                                      "peer_type": 0, "mtu": 247 if state["link"] == "connected" else 0, "encrypted": state["link"] == "connected",
                                      "bonded": True, "authenticated": False, "key_size": 16, "rssi": -52, "keepalive": state["keepalive"],
                                      "keepalive_ms": 3000, "tx": 0, "rx": 0, "tx_err": 0, "last_disc_reason": 0, "connected_s": 5,
                                      "auto": {"secure": True, "discover": True, "subscribe": True}}})
        if u.path == "/api/devices":
            return self.send({"devices": [{"addr": "26:74:F7:F6:5F:78", "type": 0, "rssi": -52, "name": "GoPro 5F78", "gopro": True, "bonded": True, "age_ms": 300,
                                           # flags | 16-bit UUIDs FEA6 | mfg F202: schema 2, status awake+pairing, model 62, caps
                                           "adv": "020106" + "0302a6fe" + "0fff" + "f202" + "02" + "05" + "3e" + "3f" + "00" * 8,
                                           # name "GoPro 5F78" | service data FEA6: AP MAC + serial tail "0000"
                                           "rsp": "0b09" + b"GoPro 5F78".hex() + "0b16a6fe" + "2674f7f6" + b"0000".hex()}]})
        if u.path == "/api/gatt":
            if not state["discovered"]:
                return self.send({"complete": False, "services": [], "chars": []})
            return self.send({"complete": True, "services": [{"uuid": s, "start": a, "end": b} for s, a, b in SVCS],
                              "chars": [{"uuid": u2, "svc": s, "def": h - 1, "handle": h, "cccd": c, "props": p, "sub": 1 if c else 0} for s, u2, h, p, c in CHARS]})
        if u.path == "/api/events":
            q = parse_qs(u.query)
            since, mx = int(q.get("since", ["0"])[0]), int(q.get("max", ["80"])[0])
            with lock:
                oldest = events[0]["s"] if events else seq[0]
                ev = [e for e in events if e["s"] >= since][:mx]
                nxt = ev[-1]["s"] + 1 if ev else max(since, oldest)
            return self.send({"oldest": oldest, "ev": ev, "next": nxt})
        if u.path == "/api/bonds":
            return self.send({"bonds": [{"addr": "26:74:F7:F6:5F:78", "type": 0}]})
        if u.path == "/api/log":
            return self.send(b"I (123) app: mock device log\n", ctype="text/plain")
        # The real page + a test driver from tools/: /selftest.html -> selftest.js,
        # /<name>.html -> tools/.<name>.js for ad-hoc probes.
        tools = os.path.dirname(os.path.abspath(__file__))
        drv = {"/selftest.html": "selftest.js"}.get(u.path) or (u.path.endswith(".html") and u.path != "/index.html"
                                                                  and "." + u.path[1:-5] + ".js")
        if drv and os.path.isfile(os.path.join(tools, drv)):
            page = open(os.path.join(WWW, "index.html")).read()
            return self.send(page.replace("</body>", f'<script src="/_drv/{drv}"></script></body>').encode(), ctype="text/html")
        if u.path.startswith("/_drv/"):
            return self.send(open(os.path.join(tools, os.path.basename(u.path)), "rb").read(), ctype="application/javascript")
        name = "index.html" if u.path in ("/", "") else u.path.lstrip("/")
        p = os.path.join(WWW, name)
        if os.path.isfile(p):
            ct = {"js": "application/javascript", "css": "text/css"}.get(name.rsplit(".", 1)[-1], "text/html")
            return self.send(open(p, "rb").read(), ctype=ct)
        return self.send({"error": "not found"}, 404)

    def do_POST(self):
        u = urlparse(self.path).path
        b = self.body()
        if u == "/api/connect":
            state["link"] = "connected"
            push(0, text="connected (conn 1)")
            push(0, text="encrypted (bonded=1, key 16)")
            state["discovered"] = True
            push(0, text="GATT discovered: 4 services, 13 characteristics (420 ms)")
        elif u == "/api/disconnect":
            state["link"] = "idle"
            state["discovered"] = False
            push(0, text="disconnected (reason 0x216)")
        elif u == "/api/keepalive":
            state["keepalive"] = bool(b.get("on"))
        elif u == "/api/write":
            hdl, data = b.get("handle"), bytes.fromhex(b.get("hex", ""))
            short = next((c[1][4:8] for c in CHARS if c[2] == hdl), None)
            pk = packets(data) if b.get("gopro", 1) else [data]
            for p in pk:
                push(1, hdl, data=p)
            if short:
                camera(short, data)
        elif u == "/api/read":
            hdl = b.get("handle")
            push(4, hdl, data=b"GP24500000" if hdl == H["0002"] else b"secret-pw" if hdl == H["0003"] else b"\x01")
        elif u in ("/api/boot_other", "/api/reboot"):
            return self.send({"ok": True, "rebooting": True, "project": "fc_cam_bridge", "version": "0.1.0"})
        return self.send({"ok": True})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8080)
    a = ap.parse_args()
    push(0, text="BLE host ready (mock)")
    print(f"mock RecLink on http://localhost:{a.port}")
    ThreadingHTTPServer(("127.0.0.1", a.port), Api).serve_forever()


if __name__ == "__main__":
    main()
