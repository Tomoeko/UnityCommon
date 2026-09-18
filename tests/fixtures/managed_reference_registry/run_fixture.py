#!/usr/bin/env python3
"""Generate the managed reference registry fixture using an isolated Editor project."""

from pathlib import Path
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from unity_fixture_runner import main

if __name__ == "__main__":
    raise SystemExit(main(Path(__file__).resolve().parent, log_cap=128 * 1024))
