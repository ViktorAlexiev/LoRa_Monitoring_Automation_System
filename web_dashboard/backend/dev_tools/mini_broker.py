"""Tiny MQTT 3.1.1 broker for local demos - no Docker / mosquitto needed.

Handles CONNECT, PUBLISH (QoS 0/1, acknowledged), SUBSCRIBE (+ and # wildcards),
PING, DISCONNECT. No retain, no auth, no persistence: subscribers always get
QoS 0 copies. Only for running the dashboard with the simulator on a laptop;
the real deployment uses Mosquitto (see deploy/).

    python dev_tools/mini_broker.py [port]      (default 1884)
"""
import asyncio
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 1884
clients = set()  # connected _Client objects


def matches(flt, topic):
    f, t = flt.split("/"), topic.split("/")
    for i, part in enumerate(f):
        if part == "#":
            return True
        if i >= len(t) or (part != "+" and part != t[i]):
            return False
    return len(f) == len(t)


def varint(n):
    out = bytearray()
    while True:
        b = n % 128
        n //= 128
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


class _Client:
    def __init__(self, writer):
        self.writer = writer
        self.filters = []

    def send(self, data):
        try:
            self.writer.write(data)
        except Exception:
            pass


async def read_packet(reader):
    first = (await reader.readexactly(1))[0]
    mult, length = 1, 0
    while True:
        b = (await reader.readexactly(1))[0]
        length += (b & 127) * mult
        if not b & 128:
            break
        mult *= 128
    body = await reader.readexactly(length) if length else b""
    return first, body


async def handle(reader, writer):
    c = _Client(writer)
    clients.add(c)
    try:
        while True:
            first, body = await read_packet(reader)
            ptype = first >> 4
            if ptype == 1:                                   # CONNECT
                c.send(bytes([0x20, 2, 0, 0]))
            elif ptype == 3:                                 # PUBLISH
                qos = (first >> 1) & 3
                tlen = int.from_bytes(body[:2], "big")
                topic = body[2:2 + tlen].decode("utf-8", "replace")
                pos = 2 + tlen
                if qos:
                    c.send(bytes([0x40, 2]) + body[pos:pos + 2])
                    pos += 2
                payload = body[pos:]
                out_body = len(topic.encode()).to_bytes(2, "big") + topic.encode() + payload
                packet = bytes([0x30]) + varint(len(out_body)) + out_body
                for other in list(clients):
                    if any(matches(f, topic) for f in other.filters):
                        other.send(packet)
            elif ptype == 8:                                 # SUBSCRIBE
                pid = body[:2]
                pos, granted = 2, bytearray()
                while pos < len(body):
                    flen = int.from_bytes(body[pos:pos + 2], "big")
                    c.filters.append(body[pos + 2:pos + 2 + flen].decode("utf-8", "replace"))
                    pos += 2 + flen + 1
                    granted.append(0)
                c.send(bytes([0x90, 2 + len(granted)]) + pid + bytes(granted))
            elif ptype == 12:                                # PINGREQ
                c.send(bytes([0xD0, 0]))
            elif ptype == 14:                                # DISCONNECT
                break
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        clients.discard(c)
        writer.close()


async def main():
    server = await asyncio.start_server(handle, "0.0.0.0", PORT)
    print(f"mini_broker listening on :{PORT}", flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
