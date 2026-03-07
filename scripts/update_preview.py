#!/usr/bin/env python3
"""Sync LCARS_CSS from the .ino into preview.html."""

import re, sys
from pathlib import Path

ROOT = Path(__file__).parent.parent
INO = ROOT / "HA_Comm_Badge_V1/HA_Comm_Badge_V1.ino"
PREVIEW = ROOT / "preview.html"

ino = open(INO).read()

css_match = re.search(r'R"rawliteral\((.*?)\)rawliteral"', ino, re.DOTALL)
if not css_match:
    print("ERROR: Could not find LCARS_CSS rawliteral block in .ino", file=sys.stderr)
    sys.exit(1)

css = css_match.group(1).strip()

preview = open(PREVIEW).read()

updated = re.sub(
    r'(<style>).*?(</style>)',
    lambda m: m.group(1) + "\n    " + css + "\n  " + m.group(2),
    preview,
    flags=re.DOTALL
)

if updated == preview:
    print("preview.html already up to date")
else:
    PREVIEW.write_text(updated)
    print("Synced LCARS_CSS -> preview.html")
