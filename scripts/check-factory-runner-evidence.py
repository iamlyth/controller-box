#!/usr/bin/env python3
"""Deprecated compatibility wrapper for the hidden runner checker."""
import os
from pathlib import Path
os.execv(str(Path(__file__).resolve().parents[1] / ".factory/tools/check-factory-runner-evidence.py"), ["check-factory-runner-evidence.py", *os.sys.argv[1:]])
