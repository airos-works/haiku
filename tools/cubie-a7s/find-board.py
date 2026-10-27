#!/usr/bin/env python3
"""Find the Cubie A7S on the LAN through Home Assistant's network trackers.

    find-board.py [host name ...]

Prints "<ip> <host name> <mac> <state>" for each tracker whose host name
matches (default: cubie-recovery, radxa-a733 and the Haiku image's names).
The URL and token come from $CUBIE_STATE/homeassistant.{json,token}.
"""

import json
import os
import sys
import urllib.request
from pathlib import Path

STATE = Path(os.environ.get("CUBIE_STATE", "/mnt/HaikuWork/cubie/state"))
CONFIG = json.loads((STATE / "homeassistant.json").read_text())
TOKEN = (STATE / "homeassistant.token").read_text().strip()
NAMES = sys.argv[1:] or ["cubie-recovery", "radxa-a733", "airos", "haiku", "shredder-cubie"]

request = urllib.request.Request(CONFIG["url"] + "/api/states",
	headers={"Authorization": "Bearer " + TOKEN})
with urllib.request.urlopen(request, timeout=10) as response:
	states = json.load(response)

for entity in states:
	if not entity["entity_id"].startswith("device_tracker."):
		continue
	attributes = entity["attributes"]
	host = attributes.get("host_name") or ""
	if any(name in host.lower() for name in NAMES):
		print(attributes.get("ip", "?"), host, attributes.get("mac", "?"),
			entity["state"], entity["last_changed"])
