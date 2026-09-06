#!/usr/bin/env python3
"""Adversarial replay fixtures for root-held dynamic D-Bus mediation."""
import copy,importlib.util,pathlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location("dbus_audit",ROOT/"scripts/inputplumber-dbus-audit.py")
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)
C="/org/shadowblip/InputPlumber/CompositeDevice/one"
T="/org/shadowblip/InputPlumber/TargetDevice/request_one"
S=":1.40";O=":1.2"
BEFORE={C:{a.IF_COMPOSITE:{"TargetDevices":[]}},a.MANAGER:{a.IF_MANAGER:{}},a.ROOT:{a.IF_OM:{}}}
AFTER=copy.deepcopy(BEFORE)

def call(seq,serial,path,iface,member,sig,body,sender=S):
 return {"type":"call","seq":seq,"serial":serial,"sender":sender,"destination":a.BUS,"path":path,"interface":iface,"member":member,"signature":sig,"body":body}
def reply(seq,serial,body=None,sig="",sender=O,destination=S):
 return {"type":"return","seq":seq,"reply_serial":serial,"sender":sender,"destination":destination,"signature":sig,"body":body or []}
def good():
 events=[
  call(1,1,a.ROOT,a.IF_OM,"GetManagedObjects","",[]),reply(2,1,[BEFORE],"a{oa{sa{sv}}}"),
  call(3,2,a.MANAGER,a.IF_MANAGER,"CreateTargetDevice","s",["xb360"]),reply(4,2,[T],"o"),
  call(5,3,C,a.IF_COMPOSITE,"SetTargetDevices","as",[["xb360"]]),reply(6,3),
  call(7,4,C,a.IF_COMPOSITE,"SetInterceptActivation","ass",[["Guide"],"Guide"]),reply(8,4),
  call(9,5,C,a.IF_PROPERTIES,"Set","ssv",[a.IF_COMPOSITE,"InterceptMode",{"signature":"s","value":"1"}]),reply(10,5),
  call(11,6,a.MANAGER,a.IF_MANAGER,"StopTargetDevice","s",[T]),reply(12,6),
  call(13,7,C,a.IF_COMPOSITE,"SetTargetDevices","as",[[]]),reply(14,7),
 ]
 return {"schema":a.SCHEMA,"complete":True,"overflow":False,"truncated":False,"monitor_started_ns":1,"proxy_started_ns":2,"monitor_pid":20,"proxy_pid":30,"proxy_starttime":99,"proxy_cgroup":"/factory/proxy","inputplumber_owner":O,"sender_pids":{S:30},"events":events}

def reject(doc,before=BEFORE,after=AFTER):
 try:a.replay(doc,before,after,{"target_count":1,"allowed_intercepts":[[["Guide"],"Guide"]]})
 except a.AuditError:return
 raise AssertionError("malicious D-Bus trace accepted")

binding=a.replay(good(),BEFORE,AFTER,{"target_count":1,"allowed_intercepts":[[["Guide"],"Guide"]]})
assert binding["learned_targets"]==[T] and binding["mutation_count"]==6 and binding["dedicated_host"] is True

# Foreign mutation and mutate+restore are rejected from the call stream even
# when the final ObjectManager snapshot equals the baseline.
x=good();x["events"][4]["path"]="/org/shadowblip/InputPlumber/CompositeDevice/foreign";reject(x)
x=good();x["events"].insert(4,call(5,90,"/org/shadowblip/InputPlumber/CompositeDevice/foreign",a.IF_COMPOSITE,"SetTargetDevices","as",[["xb360"]]));x["events"].insert(5,reply(6,90));
for i,e in enumerate(x["events"],1):e["seq"]=i
reject(x)
# InputEvent is permanently denied, including on a request-created target.
x=good();x["events"].insert(4,call(5,90,T,a.IF_TARGET,"InputEvent","ay",[[1,2,3]]));x["events"].insert(5,reply(6,90));
for i,e in enumerate(x["events"],1):e["seq"]=i
reject(x)
# Fake reply, reordered/gapped sequence, hidden sender, truncation/overflow,
# concurrent unrelated client, wrong arguments/DeviceType, and target reuse.
x=good();x["events"][3]["sender"]=":1.999";reject(x)
x=good();x["events"][4]["seq"]=99;reject(x)
x=good();x["events"][4]["sender"]=":1.41";reject(x)
x=good();x["truncated"]=True;reject(x)
x=good();x["overflow"]=True;reject(x)
x=good();x["sender_pids"][":1.41"]=31;reject(x)
x=good();x["events"][2]["body"]=["ds5"];reject(x)
x=good();x["events"][4]["body"]=[["ds5"]];reject(x)
pre=copy.deepcopy(BEFORE);pre[T]={a.IF_MANAGER:{}};reject(good(),pre,AFTER)
# Dedicated-host invariant: unrelated composites and pre-existing targets fail.
pre=copy.deepcopy(BEFORE);pre["/org/shadowblip/InputPlumber/CompositeDevice/two"]={a.IF_COMPOSITE:{}};reject(good(),pre,AFTER)
pre=copy.deepcopy(BEFORE);pre[T]={a.IF_TARGET:{}};reject(good(),pre,AFTER)
# A learned target retained after cleanup invalidates the trace.
after=copy.deepcopy(AFTER);after[T]={a.IF_TARGET:{"DeviceType":"xb360"}};reject(good(),BEFORE,after)
print("test: dynamic root-side D-Bus audit fixtures passed")
