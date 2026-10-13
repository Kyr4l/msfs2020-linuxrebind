#!/usr/bin/env python3
"""Tiny fake SimConnect TCP server for testing msfs-linuxrebind without MSFS.

It speaks just enough of the wire protocol: answers Open, logs mapped/transmitted
events, emulates the MobiFlight WASM module command/response areas (replies
MF.Pong to MF.Ping and prints executed calculator code) and answers system state
requests. Run:  tools/mock_simconnect.py [--port 500] and point the app at it.
"""
import argparse
import socket
import struct
import threading

RECV_EXCEPTION, RECV_OPEN, RECV_QUIT, RECV_EVENT, RECV_EVENT_FILENAME = 1, 2, 3, 4, 6
RECV_SIMOBJECT_DATA, RECV_SYSTEM_STATE, RECV_CLIENT_DATA = 8, 15, 16


def cstr(b):
    return b.split(b"\0", 1)[0].decode("latin1", "replace")


class Client(threading.Thread):
    def __init__(self, sock, addr, verbose):
        super().__init__(daemon=True)
        self.sock, self.addr, self.verbose = sock, addr, verbose
        self.events = {}
        self.client_data_names = {}
        self.defs = {}
        self.requests = {}  # request id -> (data id, define id)
        self.sim_requests = {}  # request id -> define id (simobject data)
        self.sim_defs = {}  # define id -> list of (name, units)

    def send(self, msg_id, payload):
        data = struct.pack("<III", 12 + len(payload), 5, msg_id) + payload
        self.sock.sendall(data)

    def log(self, *a):
        print(f"[{self.addr[1]}]", *a, flush=True)

    def run(self):
        buf = b""
        try:
            while True:
                chunk = self.sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
                while len(buf) >= 16:
                    size, proto, ptype, send_id = struct.unpack_from("<IIII", buf, 0)
                    if len(buf) < size:
                        break
                    body = buf[16:size]
                    buf = buf[size:]
                    self.handle(ptype & 0x0FFFFFFF, send_id, body, proto)
        except (ConnectionResetError, BrokenPipeError):
            pass
        self.log("disconnected")

    def handle(self, t, send_id, b, proto):
        if t == 0x01:
            name = cstr(b[:256])
            self.log(f"OPEN from '{name}' (protocol {proto})")
            payload = b"Mock Flight Simulator".ljust(256, b"\0") + struct.pack("<10I", 11, 0, 282174, 999, 11, 0, 62651, 3, 0, 0)
            self.send(RECV_OPEN, payload)
        elif t == 0x04:
            eid, = struct.unpack_from("<I", b, 0)
            name = cstr(b[4:260])
            self.events[eid] = name
            self.log(f"map event {eid} -> {name}")
        elif t == 0x05:
            obj, eid, data, group, flags = struct.unpack_from("<IIIII", b, 0)
            self.log(f"EVENT {self.events.get(eid, '?')} data={struct.unpack('<i', struct.pack('<I', data))[0]} group={group} flags={flags:#x}")
        elif t == 0x0C:
            did, = struct.unpack_from("<I", b, 0)
            name, units = cstr(b[4:260]), cstr(b[260:516])
            self.sim_defs.setdefault(did, []).append((name, units))
            if self.verbose:
                self.log(f"data def {did} += {name} ({units})")
        elif t == 0x0E:
            rid, did, obj, period, flags, origin, interval, limit = struct.unpack_from("<8I", b, 0)
            self.sim_requests[rid] = (did, flags)
            self.log(f"request data: req {rid} def {did} ({len(self.sim_defs.get(did, []))} vars) period {period} interval {interval} flags {flags}")
            self.send_sim_data(rid, did, flags)
        elif t == 0x17:
            eid, = struct.unpack_from("<I", b, 0)
            name = cstr(b[4:260])
            self.log(f"subscribe system event {eid} {name}")
            if name == "Sim":
                self.send(RECV_EVENT, struct.pack("<III", 0xFFFFFFFF, eid, 1))
            if name == "AircraftLoaded":
                self.send(RECV_EVENT_FILENAME, struct.pack("<III", 0xFFFFFFFF, eid, 0) + b"C:\\mock\\SimObjects\\Airplanes\\FlyByWire_A320_NEO\\aircraft.CFG".ljust(260, b"\0") + struct.pack("<I", 0))
        elif t == 0x35:
            rid, = struct.unpack_from("<I", b, 0)
            state = cstr(b[4:260])
            self.log(f"request system state {state}")
            s = b"C:\\mock\\SimObjects\\Airplanes\\FlyByWire_A320_NEO\\aircraft.CFG" if state == "AircraftLoaded" else b""
            self.send(RECV_SYSTEM_STATE, struct.pack("<IIf", rid, 1, 0.0) + s.ljust(260, b"\0"))
        elif t == 0x37:
            name = cstr(b[:256])
            cid, = struct.unpack_from("<I", b, 256)
            self.client_data_names[cid] = name
            self.log(f"map client data '{name}' -> {cid}")
        elif t == 0x39:
            did, off, size, eps, datum = struct.unpack_from("<IIIfI", b, 0)
            self.defs[did] = (off, size)
        elif t == 0x3B:
            cid, rid, did, period, flags, *_ = struct.unpack_from("<8I", b, 0)
            self.requests[rid] = (cid, did)
            self.log(f"request client data {self.client_data_names.get(cid)} req {rid}")
        elif t == 0x3C:
            cid, did, res, count, unit = struct.unpack_from("<IIIII", b, 0)
            text = cstr(b[20:20 + unit])
            name = self.client_data_names.get(cid, "?")
            if text == "MF.DummyCmd":
                return
            self.log(f"client data '{name}' <- {text}")
            if name == "MobiFlight.Command":
                resp = None
                if text == "MF.Ping":
                    resp = "MF.Pong"
                elif text.startswith("MF.SimVars.Set."):
                    self.log(f"   execute_calculator_code: {text[len('MF.SimVars.Set.'):]}")
                if resp:
                    for rid, (rcid, rdid) in self.requests.items():
                        if self.client_data_names.get(rcid) == "MobiFlight.Response":
                            payload = struct.pack("<7I", rid, 0, rdid, 0, 0, 0, 1) + resp.encode().ljust(1024, b"\0")
                            self.send(RECV_CLIENT_DATA, payload)
        else:
            if self.verbose:
                self.log(f"packet type {t:#x} ({len(b)} bytes)")

    def send_sim_data(self, rid, did, flags):
        vars_ = self.sim_defs.get(did, [])
        values = []
        for name, _ in vars_:
            v = 0.0
            if name.upper().startswith("SIM ON GROUND"):
                v = 1.0
            elif name.upper().startswith("G FORCE"):
                v = 1.0
            elif name.upper().startswith("GROUND VELOCITY"):
                v = 42.0
            values.append(v)
        if flags & 2:  # tagged: (datum id, value) pairs
            body = b"".join(struct.pack("<Id", i, v) for i, v in enumerate(values))
        else:
            body = struct.pack(f"<{len(values)}d", *values)
        payload = struct.pack("<7I", rid, 0, did, flags, 0, 1, len(values)) + body
        self.send(RECV_SIMOBJECT_DATA, payload)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=500)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(4)
    print(f"mock SimConnect listening on {args.host}:{args.port}", flush=True)
    while True:
        sock, addr = srv.accept()
        print(f"[{addr[1]}] connected", flush=True)
        Client(sock, addr, args.verbose).start()


if __name__ == "__main__":
    main()
