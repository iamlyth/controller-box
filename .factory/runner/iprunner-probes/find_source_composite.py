#!/usr/bin/env python3
"""Print the unique composite containing an exact InputPlumber source path."""
import json, sys
from unwrap_variant import decode_object_manager, variant_value
IFACE="org.shadowblip.Input.CompositeDevice"
if len(sys.argv)!=3:
    raise SystemExit("usage: find_source_composite.py OM.json SOURCE_PATH")
try:
    doc=json.load(open(sys.argv[1],encoding="utf-8")); objects=decode_object_manager(doc)
except (OSError,ValueError,json.JSONDecodeError) as e:
    raise SystemExit(f"source-composite: malformed ObjectManager reply: {e}")
source=sys.argv[2]; matches=[]
for path,ifaces in objects.items():
    props=ifaces.get(IFACE)
    if not isinstance(props,dict): continue
    values=variant_value(props.get("SourceDevicePaths"))
    if not isinstance(values,list) or not all(isinstance(x,str) for x in values):
        raise SystemExit(f"source-composite: malformed SourceDevicePaths at {path}")
    if source in values: matches.append(path)
if len(matches)!=1:
    raise SystemExit(f"source-composite: source assignment ambiguous ({len(matches)} composites)")
print(matches[0])
