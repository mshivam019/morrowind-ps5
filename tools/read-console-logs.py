#!/usr/bin/env python3
"""Read logs from the active native title's writable mount before stopping it."""
import argparse
import ftplib
import io
from console_control import HOST
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
(ROOT / "logs").mkdir(exist_ok=True)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--label", default="latest")
args = parser.parse_args()
with ftplib.FTP() as ftp:
    ftp.connect(HOST, 2121, timeout=15)
    ftp.login()
    for name in ("stderr.log", "stdout.log", "config/openmw.log", "ps5-opengl.log"):
        content = io.BytesIO()
        try:
            ftp.retrbinary("RETR /mnt/sandbox/PPSA99630_000/download0/" + name, content.write)
        except ftplib.error_perm as exc:
            print(f"{name}: {exc}")
            continue
        data = content.getvalue()
        (ROOT / "logs" / (args.label + "-" + name.replace("/", "-"))).write_bytes(data)
        print(f"{name} ({len(data)} bytes):\n{data[-10000:].decode(errors='replace')}")
