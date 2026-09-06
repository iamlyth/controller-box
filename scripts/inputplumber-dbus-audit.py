#!/usr/bin/env python3
"""Replay the root-held InputPlumber D-Bus mediation audit.

The candidate never writes this stream.  The broker records complete messages
from a monitor started before the candidate proxy, pins the proxy process, and
normalises the monitor output to ``factory-inputplumber-dbus-audit/v1``.  This
module is deliberately dependency-free so both the installed root broker and
the checkout-side receipt validator can replay the identical policy.
"""
from __future__ import annotations
import argparse, hashlib, json, os, pathlib, re, sys

SCHEMA="factory-inputplumber-dbus-audit/v1"
BUS="org.shadowblip.InputPlumber"
OWNER=re.compile(r"^:[0-9]+\.[0-9]+$")
OBJ=re.compile(r"^/org/shadowblip/InputPlumber(?:/[A-Za-z0-9_]+)+$")
MANAGER="/org/shadowblip/InputPlumber/Manager"
ROOT="/org/shadowblip/InputPlumber"
IF_MANAGER="org.shadowblip.InputManager"
IF_COMPOSITE="org.shadowblip.Input.CompositeDevice"
IF_TARGET="org.shadowblip.Input.Target"
IF_PROPERTIES="org.freedesktop.DBus.Properties"
IF_OM="org.freedesktop.DBus.ObjectManager"
IF_INTROSPECT="org.freedesktop.DBus.Introspectable"
MAX_EVENTS=20000

class AuditError(ValueError): pass

def _need(ok,msg):
 if not ok: raise AuditError(msg)

def _variant(v):
 # Normalised traces may retain a typed variant wrapper, but no arbitrary
 # nested wrapper is accepted.
 if isinstance(v,dict) and set(v)=={"signature","value"} and isinstance(v["signature"],str):
  return v["value"]
 return v

def _interfaces(snapshot):
 _need(isinstance(snapshot,dict),"ObjectManager snapshot is not an object map")
 out={}
 for path,interfaces in snapshot.items():
  _need(isinstance(path,str) and (path==ROOT or OBJ.fullmatch(path) is not None),"snapshot object path malformed")
  _need(isinstance(interfaces,dict),"snapshot interface map malformed")
  out[path]=set(interfaces)
 return out

def _dedicated(before):
 interfaces=_interfaces(before)
 composites=[p for p,v in interfaces.items() if IF_COMPOSITE in v]
 targets=[p for p,v in interfaces.items() if IF_TARGET in v]
 # xdg-dbus-proxy cannot restrict a global call allowlist by dynamic object
 # path.  The enrolled iprunner is consequently a dedicated host: one and
 # only one approved physical composite, with no pre-existing targets.
 _need(len(composites)==1,"dedicated host must expose exactly one approved physical composite")
 _need(not targets,"dedicated host must start with zero target devices")
 return interfaces,composites[0]

def _body(event):
 value=event.get("body")
 _need(isinstance(value,list),"message body must be a JSON array")
 return value

def _reply_ok(event,call,owner,sender):
 _need(event.get("sender")==owner,"reply is not from the held InputPlumber owner")
 _need(event.get("destination")==sender,"reply destination does not equal the proxy connection")
 _need(event.get("reply_serial")==call["serial"],"reply serial mismatch")
 _need(event.get("error_name") in (None,""),"method call returned an error")

def replay(document,before,after,contract=None):
 """Validate one normalised complete monitor stream and return its binding."""
 contract=contract or {}
 _need(isinstance(document,dict) and document.get("schema")==SCHEMA,"audit schema invalid")
 exact={"schema","complete","overflow","truncated","monitor_started_ns","proxy_started_ns","monitor_pid","proxy_pid","proxy_starttime","proxy_cgroup","inputplumber_owner","sender_pids","events"}
 _need(set(document)==exact,"audit top-level field set is not exact")
 for key in ("monitor_started_ns","proxy_started_ns","monitor_pid","proxy_pid","proxy_starttime"):
  _need(type(document.get(key)) is int and document[key]>0,f"audit {key} invalid")
 _need(isinstance(document.get('proxy_cgroup'),str) and document['proxy_cgroup'].startswith('/') and '\n' not in document['proxy_cgroup'],'audit proxy cgroup invalid')
 _need(document["monitor_started_ns"]<document["proxy_started_ns"],"monitor did not start before proxy")
 _need(document["complete"] is True and document["overflow"] is False and document["truncated"] is False,"monitor gap/truncation/overflow")
 owner=document["inputplumber_owner"]
 _need(isinstance(owner,str) and OWNER.fullmatch(owner),"held InputPlumber owner malformed")
 sender_pids=document["sender_pids"]
 _need(isinstance(sender_pids,dict) and len(sender_pids)==1,"multiple or absent proxy senders")
 sender,pid=next(iter(sender_pids.items()))
 _need(OWNER.fullmatch(sender) is not None and pid==document["proxy_pid"],"GetConnectionUnixProcessID does not bind sender to held proxy PID")
 events=document["events"]
 _need(isinstance(events,list) and 0<len(events)<=MAX_EVENTS,"audit event cardinality invalid")
 before_if,composite=_dedicated(before)
 after_if=_interfaces(after)
 approved={ROOT,MANAGER,composite}
 pending={};learned=[];mutations=[];last_seq=0
 allowed_intercepts=contract.get("allowed_intercepts",[[["Guide"],"Guide"]])
 _need(isinstance(allowed_intercepts,list),"allowed intercept contract malformed")
 for event in events:
  _need(isinstance(event,dict),"audit event malformed")
  _need(set(event).issubset({"type","seq","serial","reply_serial","sender","destination","path","interface","member","signature","body","error_name"}),"audit event has unknown fields")
  seq=event.get("seq");_need(type(seq)is int and seq==last_seq+1,"audit sequence has a gap or reorder");last_seq=seq
  typ=event.get("type")
  if typ=="call":
   _need(event.get("sender")==sender,"hidden or unrelated D-Bus sender observed")
   _need(event.get("destination")==BUS,"call destination is not exact InputPlumber")
   serial=event.get("serial");_need(type(serial)is int and serial>0 and serial not in pending,"call serial invalid/reused")
   path=event.get("path");iface=event.get("interface");member=event.get("member");body=_body(event)
   _need(isinstance(path,str) and isinstance(iface,str) and isinstance(member,str),"call identity malformed")
   _need(not (iface==IF_TARGET and member=="InputEvent"),"Target.InputEvent is permanently forbidden to candidate")
   key=(iface,member)
   mut=False
   if key==(IF_MANAGER,"CreateTargetDevice"):
    _need(path==MANAGER and event.get("signature")=="s" and body==["xb360"],"CreateTargetDevice path/arguments are not approved xb360")
    mut=True
   elif key==(IF_MANAGER,"StopTargetDevice"):
    _need(path==MANAGER and event.get("signature")=="s" and len(body)==1 and body[0] in learned,"StopTargetDevice does not name a request-created target")
    mut=True
   elif key==(IF_COMPOSITE,"SetTargetDevices"):
    _need(path==composite and event.get("signature")=="as" and len(body)==1 and isinstance(body[0],list),"SetTargetDevices source/signature malformed")
    _need(body[0] in (["xb360"],[]),"SetTargetDevices value is outside production assignment/cleanup")
    mut=True
   elif key==(IF_COMPOSITE,"SetInterceptActivation"):
    _need(path==composite and event.get("signature")=="ass" and len(body)==2 and [body[0],body[1]] in allowed_intercepts,"intercept mutation differs from approved production trigger")
    mut=True
   elif key==(IF_PROPERTIES,"Set"):
    _need(path==composite and event.get("signature")=="ssv" and len(body)==3 and body[0]==IF_COMPOSITE,"Properties.Set source/interface malformed")
    value=_variant(body[2]);prop=body[1]
    _need((prop=="InterceptMode" and value in ("1",1)) or (prop=="TargetDevices" and isinstance(value,list) and (not value or all(x in learned for x in value))),"Properties.Set value/property is not required by production")
    mut=True
   elif key==(IF_OM,"GetManagedObjects"):
    _need(path==ROOT and event.get("signature") in ("",None) and body==[],"ObjectManager read path/args malformed")
   elif key in ((IF_PROPERTIES,"Get"),(IF_PROPERTIES,"GetAll")):
    _need(path in approved|set(learned) and event.get("signature") in ("ss","s"),"Properties read path/signature not approved")
    _need(len(body) in (1,2) and body[0] in (IF_MANAGER,IF_COMPOSITE,IF_TARGET),"Properties read interface/args not approved")
   elif key==(IF_INTROSPECT,"Introspect"):
    _need(path in approved|set(learned) and body==[] and event.get("signature") in ("",None),"Introspect read path/args not approved")
   else: raise AuditError("unknown or unapproved D-Bus method/interface")
   pending[serial]=(event,mut)
   if mut:mutations.append((seq,iface,member,path,body))
  elif typ in ("return","error"):
   serial=event.get("reply_serial");_need(type(serial)is int and serial in pending,"fake, duplicate, or unmatched method reply")
   call,mut=pending.pop(serial);_reply_ok(event,call,owner,sender)
   body=_body(event)
   if call["interface"]==IF_MANAGER and call["member"]=="CreateTargetDevice":
    _need(typ=="return" and event.get("signature") in ("o","s") and len(body)==1 and isinstance(body[0],str) and OBJ.fullmatch(body[0]),"CreateTargetDevice reply malformed")
    target=body[0];_need(target not in before_if and target not in learned,"target path was reused or pre-existing")
    learned.append(target);approved.add(target)
   else:_need(typ=="return","approved call failed")
  else:raise AuditError("unknown audit event type")
 _need(not pending,"audit contains unmatched calls")
 # Any learned request target must be absent after cleanup.  This catches a
 # candidate that leaves a target behind; the mutation replay catches
 # collateral mutate+restore even when before and after hashes match.
 _need(all(p not in after_if for p in learned),"request-created target remains after cleanup")
 expected_count=contract.get("target_count")
 if expected_count is not None:_need(type(expected_count)is int and len(learned)==expected_count,"created target count differs from root contract")
 raw=(json.dumps(document,sort_keys=True,separators=(",",":"))+"\n").encode()
 return {"schema":"factory-inputplumber-dbus-audit-binding/v1","sha256":hashlib.sha256(raw).hexdigest(),"event_count":len(events),"sender":sender,"proxy_pid":pid,"learned_targets":learned,"mutation_count":len(mutations),"dedicated_host":True,"input_event_observed":False}

def load(path):
 p=pathlib.Path(path)
 if p.is_symlink() or not p.is_file() or p.stat().st_size>16*1024*1024:raise AuditError("unsafe/oversized audit input")
 return json.loads(p.read_bytes())

def main():
 ap=argparse.ArgumentParser();ap.add_argument("--audit",required=True);ap.add_argument("--before",required=True);ap.add_argument("--after",required=True);ap.add_argument("--contract");ap.add_argument("--binding-out")
 a=ap.parse_args()
 try: binding=replay(load(a.audit),load(a.before),load(a.after),load(a.contract) if a.contract else None)
 except (AuditError,ValueError,OSError) as exc:raise SystemExit("inputplumber-dbus-audit: "+str(exc))
 raw=json.dumps(binding,sort_keys=True,separators=(",",":"))+"\n"
 if a.binding_out:
  fd=os.open(a.binding_out,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600);os.write(fd,raw.encode());os.fsync(fd);os.close(fd)
 print(raw,end="")
if __name__=="__main__":main()
