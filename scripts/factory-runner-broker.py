#!/usr/bin/env python3
"""Privileged, fail-closed factory runner execution and signing broker (v2).

The broker is the sole sudo command.  It issues/consumes nonces, reconstructs
candidate product trees, executes only root-owned probe-authority code, runs
candidate processes in mandatory systemd containment, independently validates
retained artifacts, proves the cgroup empty, constructs the manifest, signs it,
and only then exports bytes.  The SSH UID can never submit a manifest to a
signer or execute a candidate-supplied probe.
"""
from __future__ import annotations
import base64, hashlib, io, json, os, re, resource, shutil, stat, subprocess, sys, tarfile, tempfile, time
from pathlib import Path, PurePosixPath
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
from factory_runner_policy import PolicyError,class_for_uid,load_policy
from factory_runner_authority import AuthorityError,load_authority
from factory_runner_artifacts import ArtifactError,PROTOCOL,MAX_ARTIFACTS,MAX_ARTIFACT_FILE,MAX_ARTIFACT_BYTES,collect,descriptors_digest

SHA1=re.compile(r"^[0-9a-f]{40}$"); SHA256=re.compile(r"^[0-9a-f]{64}$"); NAME=re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
MAX_HEADER=65536; MAX_ARCHIVE=128*1024*1024; MAX_FILES=10000; MAX_CONTENT=256*1024*1024; MAX_LOG=4*1024*1024
SIGNER="/usr/local/libexec/factory-runner-signer"

def fail(msg):
 print(json.dumps({"schema":"factory-runner-receipt/v2","result":"fail","error":msg},sort_keys=True,separators=(",",":"))); raise SystemExit(1)
def emit(v): print(json.dumps(v,sort_keys=True,separators=(",",":")),flush=True)
def caller_uid():
 try: uid=int(os.environ["SUDO_UID"])
 except (KeyError,ValueError): fail("broker requires an exact sudo caller UID")
 if uid<=0 or os.getuid()!=0 or os.geteuid()!=0: fail("broker must run as root for a non-root sudo caller")
 return uid

def ledger_dir(entry):
 p=Path(entry["nonce_ledger"]); p.mkdir(mode=0o700,parents=True,exist_ok=True)
 i=p.stat()
 if i.st_uid!=0 or i.st_mode&0o077 or p.is_symlink(): fail("nonce ledger is not private root state")
 return p

def issue_nonce(entry,req,uid):
 if set(req)!={"schema","runner","campaign_id","readiness_nonce"} or req.get("schema")!="factory-runner-nonce-request/v1": fail("nonce request is invalid")
 if req["runner"]!=entry["name"] or not NAME.fullmatch(req["campaign_id"]) or not SHA256.fullmatch(req["readiness_nonce"]): fail("nonce request binding is invalid")
 nonce=hashlib.sha256(os.urandom(64)).hexdigest(); root=ledger_dir(entry); (root/"issued").mkdir(mode=0o700,exist_ok=True)
 path=root/"issued"/nonce
 fd=os.open(path,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
 record={"uid":uid,"runner":entry["name"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"issued_at":int(time.time())}
 os.write(fd,(json.dumps(record,sort_keys=True)+"\n").encode()); os.fsync(fd); os.close(fd)
 emit({"schema":"factory-runner-nonce/v1","nonce":nonce,"runner":entry["name"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"]})

def consume_nonce(entry,request,uid):
 root=ledger_dir(entry); issued=root/"issued"/request["nonce"]; consumed=root/"consumed"; consumed.mkdir(mode=0o700,exist_ok=True)
 try: raw=issued.read_bytes(); record=json.loads(raw); os.rename(issued,consumed/request["nonce"])
 except (OSError,ValueError): fail("nonce was not issued or was already consumed")
 expected={"uid":uid,"runner":entry["name"],"campaign_id":request["campaign_id"],"readiness_nonce":request["readiness_nonce"]}
 if any(record.get(k)!=v for k,v in expected.items()) or int(time.time())-record.get("issued_at",0)>900: fail("nonce issuance binding is stale or mismatched")

def extract(archive:bytes,dest:Path):
 count=total=0
 with tarfile.open(fileobj=io.BytesIO(archive),mode="r:") as stream:
  for member in stream:
   count+=1
   if count>MAX_FILES: fail("archive file count exceeded before extraction")
   p=PurePosixPath(member.name)
   if p.is_absolute() or not p.parts or p.parts[0]==".git" or any(x in ("",".","..") for x in p.parts) or not(member.isfile() or member.isdir()): fail("archive member is unsafe")
   if member.isfile():
    total+=member.size
    if member.size>MAX_ARCHIVE or total>MAX_CONTENT: fail("archive byte bound exceeded before extraction")
   target=dest.joinpath(*p.parts)
   if member.isdir(): target.mkdir(mode=0o755,parents=True,exist_ok=True); continue
   target.parent.mkdir(mode=0o755,parents=True,exist_ok=True); src=stream.extractfile(member)
   if src is None: fail("archive member cannot be read")
   with target.open("xb") as out:
    remaining=member.size
    while remaining:
     chunk=src.read(min(65536,remaining))
     if not chunk: fail("archive member shortened")
     out.write(chunk); remaining-=len(chunk)
   target.chmod(0o755 if member.mode&0o111 else 0o644)

def clean_env(home:Path,authority,product:Path,artifacts:Path):
 return {"HOME":str(home),"GIT_CONFIG_NOSYSTEM":"1","GIT_CONFIG_GLOBAL":"/dev/null","GIT_ATTR_NOSYSTEM":"1","XDG_CACHE_HOME":str(home/".cache"),"XDG_CONFIG_HOME":str(home/".config"),"XDG_DATA_HOME":str(home/".local/share"),"PATH":":".join(authority.document["trusted_path"]),"LANG":"C.UTF-8","LC_ALL":"C.UTF-8","NIX_REMOTE":"daemon","FACTORY_PRODUCT_ROOT":str(product),"FACTORY_RUNNER_ARTIFACT_DIR":str(artifacts),"CONTROLLER_PRODUCTION_ROUTING_ARTIFACTS":str(artifacts/"controller-production-routing"),"CBX_GPU_PROBE_ARTIFACTS":str(artifacts/"gpu-compositor")}
def argv_for(authority,argv,product,artifacts,commit="",tree=""):
 out=[]
 for token in argv:
  token=token.replace("{product}",str(product)).replace("{artifacts}",str(artifacts)).replace("{commit}",commit).replace("{tree}",tree)
  if token.startswith("@/"): token=str(authority.path(token[2:]))
  out.append(token)
 return out

def exact_tree(product,request,git,env):
 def run(args,input=None):
  r=subprocess.run([git,*args],cwd=product,env=env,input=input,capture_output=True,timeout=120)
  if r.returncode: fail("candidate tree reconstruction/revalidation failed")
  return r.stdout
 if not (product/".git").exists():
  run(["init","-q"]); run(["add","-f","--all"]); tree=run(["write-tree"]).decode().strip()
  commit=run(["hash-object","-t","commit","-w","--stdin"],base64.b64decode(request["commit_object_b64"],validate=True)).decode().strip(); run(["update-ref","HEAD",commit])
 else: tree=run(["write-tree"]).decode().strip(); commit=request["commit"]
 if tree!=request["tree"] or commit!=request["commit"] or run(["status","--porcelain","--untracked-files=normal"]): fail("candidate product differs from exact requested tree")

def run_contained(entry,authority,descriptor,product,artifacts,env,unit):
 systemd=entry["systemd_run"]; systemctl=entry["systemctl"]
 for p in (systemd,systemctl):
  i=os.stat(p)
  if not stat.S_ISREG(i.st_mode) or not i.st_mode&0o111 or i.st_uid!=0 or i.st_mode&0o022: fail("containment executable is not immutable root authority")
 argv=argv_for(authority,descriptor["argv"],product,artifacts)
 cmd=[systemd,"--quiet","--wait","--pipe","--unit",unit,"--service-type=exec",f"--uid={entry['uid']}","--property=PrivateTmp=yes","--property=PrivateMounts=yes","--property=PrivatePIDs=yes","--property=KillMode=control-group","--property=RuntimeMaxSec=7200","--property=TimeoutStopSec=10",*argv]
 try:r=subprocess.run(cmd,cwd=product,env=env,capture_output=True,timeout=7300)
 except (OSError,subprocess.TimeoutExpired): fail("mandatory namespace/cgroup containment unavailable")
 if len(r.stdout)>MAX_LOG or len(r.stderr)>MAX_LOG: fail("contained output exceeds bound")
 cg=subprocess.run([systemctl,"show",unit,"--property=ControlGroup","--property=ActiveState","--property=SubState","--property=MainPID"],capture_output=True,text=True,timeout=30)
 values=dict(line.split("=",1) for line in cg.stdout.splitlines() if "=" in line)
 if (cg.returncode or not values.get("ControlGroup","").startswith("/")
         or values.get("ActiveState") not in {"inactive","failed"}
         or values.get("SubState") not in {"dead","failed"}
         or values.get("MainPID") != "0"):
  fail("cannot prove request service termination/cgroup identity")
 cgroup=Path(entry["cgroup_root"])/values["ControlGroup"].lstrip("/")
 if cgroup.exists():
  try:
   procs=(cgroup/"cgroup.procs").read_text().strip(); events=(cgroup/"cgroup.events").read_text()
  except OSError: fail("cannot prove request cgroup cleanup")
  if procs or "populated 0" not in events: fail("request cgroup retained descendants")
 # A removed cgroup after inactive/dead/MainPID=0 is systemd's stronger empty
 # postcondition: cgroup v2 refuses rmdir while any descendant remains.
 subprocess.run([systemctl,"reset-failed",unit],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=30)
 return r.returncode,r.stdout,r.stderr

def analyze(authority,descriptor,product,artifacts,env,commit,tree):
 argv=argv_for(authority,descriptor["analyzer_argv"],product,artifacts,commit,tree)
 r=subprocess.run(argv,cwd="/",env=env,capture_output=True,timeout=300)
 if r.returncode or len(r.stdout)>MAX_LOG or len(r.stderr)>MAX_LOG: fail("root authority semantic analyzer rejected raw artifacts")

def sign(evidence,entry):
 payload=(json.dumps({"schema":"factory-runner-sign-request/v1","manifest":evidence},separators=(",",":"))+"\n").encode()
 r=subprocess.run([SIGNER],input=payload,capture_output=True,timeout=120,env={"SUDO_UID":str(entry["uid"]),"SUDO_USER":entry["name"],"FACTORY_BROKER_SIGNING":"1","PATH":"/usr/bin:/bin","LANG":"C.UTF-8"})
 if r.returncode: fail("broker-owned signing failed")
 try:return json.loads(r.stdout)
 except ValueError: fail("broker-owned signer response malformed")

def main():
 uid=caller_uid()
 try: policy=load_policy(); entry=class_for_uid(policy,uid)
 except PolicyError as e: fail(f"root runner policy rejected caller: {e}")
 line=sys.stdin.buffer.readline(MAX_HEADER+1)
 try:req=json.loads(line)
 except Exception: fail("request header is invalid")
 if req.get("schema")=="factory-runner-nonce-request/v1": issue_nonce(entry,req,uid); return 0
 fields={"schema","runner","class","commit","commit_object_b64","tree","environment_blob","archive_sha256","archive_size","capabilities","campaign_id","readiness_nonce","nonce","authority_sha256"}
 if not isinstance(req,dict) or set(req)!=fields or req.get("schema")!="factory-runner-request/v2": fail("request fields/schema invalid")
 if req["runner"]!=entry["name"] or req["class"]!=entry["name"] or req["capabilities"]!=sorted(entry["allowed_capabilities"]): fail("request does not equal root class policy")
 if req["authority_sha256"]!=entry["probe_authority_sha256"]: fail("request authority binding mismatch")
 if not all(SHA1.fullmatch(req[x]) for x in ("commit","tree","environment_blob")) or not NAME.fullmatch(req["campaign_id"]) or not all(SHA256.fullmatch(req[x]) for x in ("archive_sha256","readiness_nonce","nonce","authority_sha256")): fail("request identity binding invalid")
 consume_nonce(entry,req,uid)
 size=req["archive_size"]
 if type(size)is not int or not 0<size<=MAX_ARCHIVE: fail("archive size invalid")
 archive=sys.stdin.buffer.read(size+1)
 if len(archive)!=size or hashlib.sha256(archive).hexdigest()!=req["archive_sha256"]: fail("archive framing/digest mismatch")
 try: authority=load_authority(Path(entry["probe_authority"]),entry["probe_authority_sha256"],fixture=bool(os.environ.get("FACTORY_BROKER_TEST_MODE")))
 except AuthorityError as e: fail(f"probe authority unavailable: {e}")
 contract=authority.class_contract(entry["name"])
 if sorted(contract["capabilities"])!=req["capabilities"]: fail("probe authority capability set differs from root policy")
 workroot=Path(entry["workspace_root"]); started=int(time.time()); allout=b""; allerr=b""; payload=[]; descriptors=[]
 request_dir=Path(tempfile.mkdtemp(prefix=f"request-{req['nonce']}-",dir=workroot)); os.chown(request_dir,uid,-1); request_dir.chmod(0o700)
 try:
  for index,(name,descriptor) in enumerate([("gate",contract["gate"]),*sorted(contract["capabilities"].items())]):
   runroot=request_dir/f"run-{index}"; product=runroot/"product"; home=runroot/"home"; artifacts=runroot/"artifacts"
   for p in (runroot,product,home,artifacts): p.mkdir(mode=0o700); os.chown(p,uid,-1)
   extract(archive,product)
   for parent,dirs,files in os.walk(product):
    os.chown(parent,uid,-1)
    for n in dirs+files: os.chown(Path(parent)/n,uid,-1)
   env=clean_env(home,authority,product,artifacts)
   if name != "gate":
    env["FACTORY_RUNNER_ARTIFACT_DIR"]=str(artifacts/name)
    env["CONTROLLER_PRODUCTION_ROUTING_ARTIFACTS"]=str(artifacts/name)
    env["CBX_GPU_PROBE_ARTIFACTS"]=str(artifacts/name)
   git=next((str(Path(x)/"git") for x in authority.document["trusted_path"] if (Path(x)/"git").is_file()),"")
   if not git: fail("trusted control closure has no pinned git")
   exact_tree(product,req,git,env)
   unit=f"factory-runner-{entry['name']}-{req['nonce'][:20]}-{index}.service"
   rc,out,err=run_contained(entry,authority,descriptor,product,artifacts,env,unit); allout+=out; allerr+=err
   exact_tree(product,req,git,env)
   if rc: fail(f"{name} candidate execution failed")
   if name!="gate":
    analyze(authority,descriptor,product,artifacts,env,req["commit"],req["tree"])
    requirements={name:descriptor["artifacts"]}
    d,p=collect(artifacts,[name],requirements,expected_uid=uid); descriptors.extend(d); payload.extend(p)
   shutil.rmtree(runroot)
  descriptors.sort(key=lambda x:x["path"]); payload.sort(key=lambda x:x["path"]); total=sum(x["size"] for x in descriptors); digest=descriptors_digest(descriptors)
  scope=hashlib.sha256(json.dumps({"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"runner":entry["name"],"commit":req["commit"],"nonce":req["nonce"],"artifact_manifest_sha256":digest},sort_keys=True,separators=(",",":")).encode()).hexdigest()
  evidence={"schema":"factory-runner-receipt/v3","result":"pass","runner":entry["name"],"commit":req["commit"],"tree":req["tree"],"environment_blob":req["environment_blob"],"archive_sha256":req["archive_sha256"],"campaign_id":req["campaign_id"],"readiness_nonce":req["readiness_nonce"],"nonce":req["nonce"],"authority_sha256":authority.digest,"capabilities":req["capabilities"],"exit_code":0,"timed_out":False,"started_at":started,"finished_at":int(time.time()),"cleanup":True,"stdout_sha256":hashlib.sha256(allout).hexdigest(),"stderr_sha256":hashlib.sha256(allerr).hexdigest(),"artifact_protocol":PROTOCOL,"artifact_limits":{"count":MAX_ARTIFACTS,"file_bytes":MAX_ARTIFACT_FILE,"aggregate_bytes":MAX_ARTIFACT_BYTES},"artifact_count":len(descriptors),"artifact_bytes":total,"artifact_manifest_sha256":digest,"artifact_scope_sha256":scope,"artifacts":descriptors}
  signed=sign(evidence,entry)
  emit({**evidence,"stdout_b64":base64.b64encode(allout).decode(),"stderr_b64":base64.b64encode(allerr).decode(),"manifest_b64":signed["manifest_b64"],"signature_b64":signed["signature_b64"],"signer_principal":signed["signer_principal"],"signer_key_sha256":signed["signer_key_sha256"],"signature_algorithm":signed["signature_algorithm"],"namespace":signed["namespace"],"signature_sha256":signed["signature_sha256"],"artifact_payload":payload})
 finally:
  authority.close(); shutil.rmtree(request_dir,ignore_errors=True)
 return 0
if __name__=="__main__": raise SystemExit(main())
