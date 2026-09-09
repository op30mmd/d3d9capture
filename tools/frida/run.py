#!/usr/bin/env python
"""Small runner for the d3d9capture Frida verification scripts.

Usage:
    python run.py <script.js> [--pid N | --name GTAIV.exe] [--spawn <exe>] [--seconds N]

Prints every `send()` payload from the script as pretty JSON, then detaches.
With --seconds it stays attached so per-frame hooks can report.
"""

import argparse
import json
import sys
import time

import frida


def on_message(message, data):
    if message.get("type") == "send":
        payload = message["payload"]
        print(json.dumps(payload, indent=2, sort_keys=False))
    elif message.get("type") == "error":
        print("SCRIPT ERROR: %s" % message.get("description"), file=sys.stderr)
        stack = message.get("stack")
        if stack:
            print(stack, file=sys.stderr)
    else:
        print(message, file=sys.stderr)
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("script")
    ap.add_argument("--pid", type=int)
    ap.add_argument("--name", default="GTAIV.exe")
    ap.add_argument("--spawn")
    ap.add_argument("--seconds", type=float, default=0.0)
    args = ap.parse_args()

    device = frida.get_local_device()

    if args.spawn:
        pid = device.spawn([args.spawn])
        session = device.attach(pid)
    else:
        target = args.pid if args.pid else args.name
        session = device.attach(target)
        pid = session.pid

    with open(args.script, "r", encoding="utf-8") as f:
        source = f.read()

    script = session.create_script(source)
    script.on("message", on_message)
    script.load()

    if args.spawn:
        device.resume(pid)

    if args.seconds > 0:
        time.sleep(args.seconds)

    try:
        session.detach()
    except frida.Error:
        pass


if __name__ == "__main__":
    main()
