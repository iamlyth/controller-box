#!/usr/bin/env python3
"""Static closure checks for the separately compiled mediation boundary.

Wire-level behavior is exercised on enrolled iprunner hardware; these checks
ensure a source change cannot silently restore destination-wide proxying or
move authorization after sd_bus_call().
"""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
src = (root / ".factory/runner/inputplumber-mediator.c").read_text()
broker = (root / ".factory/runner/factory-runner-broker.py").read_text()

assert "--call=org.shadowblip" not in broker[broker.index("class DbusProxy:"):broker.index("class UdevMonitor:")]
assert 'TrustedExecutable("/usr/libexec/inputplumber-mediator"' in broker
assert '"--mutations",mutate' in broker
assert "host_lock.snapshot_path" in broker
assert "root-dbus-preforward.jsonl" in broker

# Authorization must complete and be durably logged before the only upstream
# forwarding primitive. Unknown calls and InputEvent must fail closed.
filter_body = src[src.index("static int filter("):src.index("static int load_source(")]
policy_path = filter_body[filter_body.index("bool create=false"):]
assert policy_path.index("authorize(s,m,&create)") < policy_path.index("log_decision(s,m,true") < policy_path.index("forward_call(s,m,create)")
assert 'same(iface,IT)&&same(member,"InputEvent")' in src
assert "learned(s,target)" in src
assert 'same(path,s->source)' in src
assert 'same(kind,"xb360")' in src
assert 'same(sig,"ssv")' in src
assert "MAX_TARGETS 4" in src
assert "sd_bus_message_copy(call,incoming,true)" in src
assert "sd_bus_reply_method_error(incoming,&error)" in src
print("inputplumber pre-forward mediator policy: PASS")
