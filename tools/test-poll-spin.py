#!/usr/bin/env python3
"""Exercise the canonical retirement polling policy with deterministic clocks."""
from pathlib import Path
import subprocess
p=Path(__file__).resolve().parent.parent
out=p/'build/draw-batching-driver/poll-test'
out.parent.mkdir(parents=True,exist_ok=True)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(p/'patches/ps5-draw-batching/platform'),str(p/'tests/runtime/poll-spin.c'),'-o',str(out)],check=True)
subprocess.run([str(out)],check=True)
