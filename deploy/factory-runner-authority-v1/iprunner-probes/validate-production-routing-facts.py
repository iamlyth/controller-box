#!/usr/bin/env python3
"""Fail-closed validator for retained four-slot production-routing facts.

Fixture bundles are synthetic test inputs and never capability evidence.  The live
probe must retain the same v2 shape: four unique xb360 DBus paths, four unique
kernel nodes, an unambiguous real 045e:028e source/composite assignment, and a
separate post-assignment human event/consumer observation for every slot.
"""
from __future__ import annotations
import argparse, hashlib, json, re, sys
from pathlib import Path

SCHEMA = "iprunner-controller-production-routing-facts/v3"
PINNED = "/usr/bin/inputplumber"
OBJ = re.compile(r"^/org/shadowblip/InputPlumber/[A-Za-z0-9_/]+$")
NODE = re.compile(r"^/dev/input/event[0-9]+$")
HEX = re.compile(r"^[0-9a-f]{64}$")

def fail(msg: str) -> None:
    print(f"production-routing-probe: {msg}", file=sys.stderr); raise SystemExit(1)
def req(ok: bool, msg: str) -> None:
    if not ok: fail(msg)
def confined(rel: str, base: Path, label: str) -> Path:
    p=Path(rel)
    req(bool(rel) and not p.is_absolute(), f"{label} path is unsafe")
    req(".." not in p.parts, f"{label} path must not traverse '..'")
    q=base/rel
    try: q.resolve().relative_to(base.resolve())
    except ValueError: fail(f"{label} path escapes the fixture directory")
    req(q.is_file() and not q.is_symlink(), f"{label} file is missing or unsafe")
    return q
def digest(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()

def validate(d: dict, base: Path) -> None:
    req(d.get("schema")==SCHEMA, f"facts schema is not {SCHEMA}")
    h=d.get("home",{}); vc=h.get("virtual_controllers",{})
    req(h.get("isolated") is True, "HOME must be isolated")
    req(type(vc.get("count")) is int and vc["count"]==4 and vc.get("types")==["xb360"]*4,
        "default topology must request exactly 4 xb360 controllers")
    b=d.get("binary",{})
    for k in ("realpath_inside_prefix","assets_inside_prefix","launch_cwd_isolated"):
        req(type(b.get(k)) is bool, f"binary {k} must be boolean")
    bus=d.get("bus",{})
    req(bus.get("system_socket")=="/run/dbus/system_bus_socket" and bus.get("socket_root_owned") is True,
        "real root-owned system bus is required")
    req(type(bus.get("owner_pid")) is int and bus.get("owner_exe_pinned") is True and bus.get("owner_exe")==PINNED,
        "bus owner executable must be pinned")
    production=d.get("production_assignment",{})
    req(production.get("path")=="controller-box-overlay" and
        production.get("normal_event_dispatch") is True and
        production.get("save_persisted") is True and
        production.get("direct_assignment_dbus") is False,
        "routing must use Controller-Box overlay dispatch and persisted save, not direct assignment DBus")
    topo=d.get("topology",{})
    for key in ("expected","observed","target_paths","kernel_nodes"):
        req(type(topo.get(key)) is int, f"{key} targets must be int")
    req(topo["expected"]==4 and all(0 <= topo[k] <= 8 for k in ("observed","target_paths","kernel_nodes")),
        "topology counts are out of range")
    req(topo.get("cardinality") in ("exact","ambiguous") and type(topo.get("identities_match")) is bool,
        "topology cardinality/identity fields malformed")
    req(topo.get("identity_method") in ("udev-sysfs", "controlled-create-observe") and
        topo.get("one_to_one") is True,
        "DBus target to kernel node identity is not authoritative one-to-one")
    physical=d.get("physical",{})
    req(physical.get("vidpid")=="045e:028e" and physical.get("transport")=="usb",
        "physical source must be real USB 045e:028e")
    req(isinstance(physical.get("node"),str) and NODE.match(physical["node"]), "physical node malformed")
    req(isinstance(physical.get("source_path"),str) and OBJ.match(physical["source_path"]), "physical source path malformed")
    targets=d.get("targets")
    req(isinstance(targets,list) and len(targets)==4, "exactly four per-target results required")
    paths=set(); nodes=set(); observations=set(); last_source=-1
    for i,t in enumerate(targets):
        req(type(t) is dict and t.get("slot")==i, f"target{i} slot/order malformed")
        path=t.get("dbus_path"); node=t.get("kernel_node"); comp=t.get("composite_path")
        req(isinstance(path,str) and OBJ.match(path), f"target{i} DBus path malformed")
        req(isinstance(node,str) and NODE.match(node), f"target{i} kernel node malformed")
        req(isinstance(comp,str) and OBJ.match(comp), f"target{i} composite path malformed")
        req(path not in paths and node not in nodes, f"target{i} mapping is ambiguous or duplicated")
        paths.add(path); nodes.add(node)
        req(t.get("device_type")=="xb360" and isinstance(t.get("name"),str) and t["name"], f"target{i} wrong type/name")
        req(t.get("source_path")==physical["source_path"] and t.get("source_vidpid")=="045e:028e",
            f"target{i} is unrouted or assigned to the wrong physical source")
        req(t.get("assignment_verified") is True and t.get("consumer_read_only") is True and
            t.get("selected_only") is True and t.get("production_dispatch") is True and
            t.get("production_save") is True and t.get("direct_assignment_dbus") is False,
            f"target{i} assignment/consumer/source evidence invalid")
        st=t.get("source_event_us"); tt=t.get("target_event_us"); oid=t.get("observation_id")
        req(type(st) is int and type(tt) is int and st>last_source and tt>=st and tt-st<=2_000_000,
            f"target{i} timestamps are stale, reused, or uncorrelated")
        req(isinstance(oid,str) and oid and oid not in observations, f"target{i} observation is reused/ambiguous")
        observations.add(oid); last_source=st
    cleanup=d.get("cleanup",{})
    req(cleanup.get("termination") in ("ok","fail") and cleanup.get("target_cleanup") in ("ok","fail") and
        type(cleanup.get("targets_absent")) is bool and type(cleanup.get("kernel_nodes_absent")) is bool and
        type(cleanup.get("deadline_ms")) is int and 0 < cleanup["deadline_ms"] <= 15000,
        "cleanup fields or bounded deadline malformed")
    observer=d.get("observer",{})
    req(observer.get("direct_injection") is not True, "direct InputEvent/synthetic injection recorded")
    req(observer.get("mode")=="read-only-evdev" and observer.get("human_generated") is True and observer.get("synthetic") is False,
        "observer must record human-generated read-only evdev evidence")
    confined(observer.get("event_stream",""),base,"event stream")
    arts=d.get("artifacts")
    req(isinstance(arts,dict) and arts, "retained artifacts are required")
    for label,ref in arts.items():
        req(isinstance(ref,dict) and isinstance(ref.get("path"),str) and isinstance(ref.get("sha256"),str) and HEX.match(ref["sha256"]), f"artifact {label} reference malformed")
        p=confined(ref["path"],base,f"artifact {label}")
        req(digest(p)==ref["sha256"], f"artifact {label} sha256 mismatch")

def main()->int:
    ap=argparse.ArgumentParser(); ap.add_argument("--facts",required=True); ap.add_argument("--fixture-dir",required=True); a=ap.parse_args()
    base=Path(a.fixture_dir); req(base.is_dir() and not base.is_symlink(),"fixture directory is missing or unsafe")
    p=Path(a.facts); req(p.is_file() and not p.is_symlink(),"facts file is missing or unsafe")
    try: d=json.loads(p.read_text())
    except Exception as e: fail(f"facts file invalid: {e}")
    req(isinstance(d,dict),"facts must be an object"); validate(d,base)
    print("production-routing-probe: four-target fact bundle verified"); return 0
if __name__=="__main__": raise SystemExit(main())
