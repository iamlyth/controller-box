#!/usr/bin/env python3
"""Deprecated compatibility wrapper for the hidden campaign archiver."""
import os
from pathlib import Path
os.execv(str(Path(__file__).resolve().parents[1] / ".factory/tools/archive-factory-campaign.py"), ["archive-factory-campaign.py", *os.sys.argv[1:]])
