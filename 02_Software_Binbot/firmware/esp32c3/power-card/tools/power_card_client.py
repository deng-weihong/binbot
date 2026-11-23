#!/usr/bin/env python3
"""Send one command to a Power Card TCP server."""

import argparse
import json
import os
import socket
import sys
import uuid


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("command", choices=["status", "power", "reset", "ota", "ota_status"])
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--token", default=os.environ.get("POWER_CARD_TOKEN"))
    parser.add_argument("--url", help="Direct HTTP URL of power_card.bin (ota only)")
    args = parser.parse_args()
    if not args.token:
        parser.error("set POWER_CARD_TOKEN or pass --token (serial console: token)")
    if (args.command == "ota") != bool(args.url):
        parser.error("--url is required for ota and cannot be used with other commands")
    request = {"id": uuid.uuid4().hex, "cmd": args.command, "token": args.token}
    if args.url:
        request["url"] = args.url
    encoded = json.dumps(request, separators=(",", ":")).encode("utf-8")
    if len(encoded) > 1024:
        parser.error("request exceeds 1024 bytes")
    try:
        with socket.create_connection((args.host, args.port), timeout=10) as connection:
            connection.sendall(encoded + b"\n")
            with connection.makefile("rb") as stream:
                line = stream.readline(8193)
        if not line.endswith(b"\n") or len(line) > 8192:
            raise ValueError("incomplete or oversized response")
        response = json.loads(line)
        if response.get("id") != request["id"]:
            raise ValueError("response ID does not match request")
        print(json.dumps(response, indent=2, ensure_ascii=False))
        return 0 if response.get("ok") is True else 1
    except (OSError, ValueError) as error:
        print(f"Command failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
