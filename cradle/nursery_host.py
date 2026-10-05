#!/usr/bin/env python3
"""A Cradle in the nursery: services for Amigas on the LAN.

Advertises over mDNS (nursery_advertise.py) and serves openservice.device's
LAN frames on TCP: each request is the services card's 64-byte entry, then
the buffers the service reads; each answer is the 16-byte completion, then
for each buffer the service writes a u32 byte count and the bytes.

Services here: the directory (OPEN, CLOSE, LIST, CANCEL) and echo/1, which
copies buffer 0 into buffer 1, as the emulator's test service does.

  nursery_host.py --fingerprint <fp>

MIT, Copyright (c) 2026 Dalsin Limited.
"""
import argparse
import socket
import socketserver
import struct
import sys
import threading

import nursery_advertise

OK, NOSERVICE, BADREQUEST, CANCELLED, TOOSMALL, HOSTERROR = 0, -1, -2, -3, -4, -5
DIR_OPEN, DIR_CLOSE, DIR_LIST, OP_CANCEL = 1, 2, 3, 0xFFFF


def echo(op, arg, bufs, extra):
    """echo/1: buffer 0 into buffer 1."""
    data = bufs[0] or b""
    return OK, len(data), 0, {1: data}


SERVICES = {"echo/1": echo}


class Connection(socketserver.BaseRequestHandler):
    def recv_all(self, n):
        out = b""
        while len(out) < n:
            chunk = self.request.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out

    def reply(self, rid, status, result, aux, flags, lengths, written):
        out = struct.pack(">IiII", rid, status, result, aux)
        for i in range(4):
            if lengths[i] and flags & (1 << i):
                data = written.get(i, b"")[:lengths[i]]
                out += struct.pack(">I", len(data)) + data
        self.request.sendall(out)

    def handle(self):
        self.request.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        peer = self.client_address[0]
        opened = {}                                   # handle -> service name
        print(f"host: {peer} connected", flush=True)
        try:
            while True:
                entry = self.recv_all(64)
                rid, service, op, flags, arg = struct.unpack(">IHHII", entry[:16])
                pairs = struct.unpack(">8I", entry[16:48])
                extra = struct.unpack(">4I", entry[48:64])
                lengths = [pairs[1], pairs[3], pairs[5], pairs[7]]
                bufs = [self.recv_all(lengths[i]) if lengths[i] and not flags & (1 << i) else None
                        for i in range(4)]
                written = {}
                if op == OP_CANCEL:
                    status, result, aux = OK, 0, 0    # requests here finish at once
                elif service == 0 and op == DIR_OPEN:
                    name = (bufs[0] or b"").split(b"\0")[0].decode(errors="replace")
                    if name in SERVICES:
                        handle = max(opened, default=0) + 1
                        opened[handle] = name
                        status, result, aux = OK, handle, 0
                    else:
                        status, result, aux = NOSERVICE, 0, 0
                    print(f"host: {peer} open {name!r} -> {status} {result}", flush=True)
                elif service == 0 and op == DIR_CLOSE:
                    opened.pop(arg, None)
                    status, result, aux = OK, 0, 0
                elif service == 0 and op == DIR_LIST:
                    names = b"".join(n.encode() + b"\0" for n in SERVICES)
                    written[0] = names
                    status, result, aux = OK, min(len(names), lengths[0]), 0
                elif service in opened:
                    status, result, aux, written = SERVICES[opened[service]](op, arg, bufs, extra)
                else:
                    status, result, aux = BADREQUEST, 0, 0
                self.reply(rid, status, result, aux, flags, lengths, written)
        except (EOFError, ConnectionError):
            print(f"host: {peer} gone", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    nursery_advertise.add_arguments(p)
    p.set_defaults(services=None)
    for action in p._actions:
        if action.dest == "services":
            action.required = False
    args = p.parse_args()
    args.services = args.services or ",".join(SERVICES)
    advert = nursery_advertise.Advert(args)
    threading.Thread(target=nursery_advertise.advertise, args=(advert,), daemon=True).start()
    with Server(("", args.port), Connection) as server:
        print(f"host: serving {args.services} on TCP {args.port}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    sys.exit(main())
