#!/usr/bin/env python3
"""Companion to ipv6-one-group.sh; use its isolated namespace setup."""

import pathlib
import socket
import struct
import sys
import time

JOINED = "ff12::1234"
UNJOINED = "ff12::5678"
PORT = 54021
ROUNDS = 5


def receive(work, joined_payload, unjoined_payload):
    interface = socket.if_nametoindex("rx0")
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        sock.bind(("::", PORT))
        membership = struct.pack("=16sI", socket.inet_pton(socket.AF_INET6, JOINED), interface)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, membership)
        deadline = time.monotonic() + 6.0
        (work / "ready").touch()
        joined_count = 0
        unjoined_count = 0
        while time.monotonic() < deadline:
            sock.settimeout(max(0.001, deadline - time.monotonic()))
            try:
                payload, _ = sock.recvfrom(2048)
            except socket.timeout:
                break
            if payload == joined_payload:
                joined_count += 1
            elif payload == unjoined_payload:
                unjoined_count += 1
            else:
                raise RuntimeError("unexpected payload on isolated test socket")
        print("received_joined={}, received_unjoined={}".format(joined_count, unjoined_count))
        if joined_count == 0 or unjoined_count != 0:
            raise RuntimeError("joined delivery or unjoined exclusion check failed")


def send(joined_payload, unjoined_payload):
    interface = socket.if_nametoindex("tx0")
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as sock:
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 1)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, interface)
        sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 1)
        sock.bind(("fe80::1", 0, 0, interface))
        for _ in range(ROUNDS):
            for group, payload in ((UNJOINED, unjoined_payload), (JOINED, joined_payload)):
                sent = sock.sendto(payload, (group, PORT, 0, interface))
                if sent != len(payload):
                    raise RuntimeError("incomplete UDP send")
            time.sleep(0.1)
    print("sent_joined={}, sent_unjoined={}".format(ROUNDS, ROUNDS))


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("receive", "send"):
        raise SystemExit("usage: ipv6-one-group.py {receive|send} WORK_DIRECTORY")
    work = pathlib.Path(sys.argv[2])
    token = work.name.encode("ascii")
    joined_payload = b"joined:" + token
    unjoined_payload = b"unjoined:" + token
    if sys.argv[1] == "receive":
        receive(work, joined_payload, unjoined_payload)
    else:
        send(joined_payload, unjoined_payload)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError) as error:
        print("FAIL: {}".format(error), file=sys.stderr)
        sys.exit(1)
