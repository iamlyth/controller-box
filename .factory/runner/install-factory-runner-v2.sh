#!/usr/bin/env bash
# Fail-closed, generation-journalled root installer for the factory runner.
# The installer snapshots every input once; no checkout path is imported,
# executed, or reopened after that snapshot has been validated.
set -euo pipefail
umask 077
usage() {
  echo 'usage:' >&2
  echo '  install-factory-runner-v2.sh install --source-root DIR --install-manifest FILE --commit SHA1 --tree SHA1 --policy FILE --transport-manifest FILE --ssh-launcher-manifest FILE --key devrunner=FILE --key iprunner=FILE --key gpurunner=FILE' >&2
  echo '  install-factory-runner-v2.sh verify|rollback' >&2
  exit 2
}
[[ $(/usr/bin/id -u) -eq 0 ]] || usage
ACTION=${1:-}; shift || true
STATE=/var/lib/factory-runner/install-transaction
if [[ $ACTION == rollback ]]; then
 [[ -d $STATE && ! -L $STATE ]] || { echo 'no recoverable factory-runner transaction' >&2; exit 1; }
 # The fsynced append-only journal is the sole recovery authority.  In
 # particular, never infer rollback scope from partially written side files.
 # A committed generation is durable even when power loss leaves STATE behind.
 if /usr/bin/python3 -I - "$STATE" <<'PY'
import json,os,pathlib,sys
s=pathlib.Path(sys.argv[1]);j=s/'journal'
try: lines=j.read_text().splitlines()
except FileNotFoundError: raise SystemExit(1)
committed=[x.split('\t') for x in lines if len(x.split('\t'))==4 and x.split('\t')[1:] == ['committed','generation','complete']]
if not committed: raise SystemExit(1)
generation=committed[-1][0]
try: installed=json.loads(pathlib.Path('/etc/factory-runner/installed-generation.json').read_bytes())
except Exception: raise SystemExit(2)
if installed.get('generation_id')!=generation: raise SystemExit(2)
PY
 then
   /bin/rm -rf -- "$STATE"
   /usr/bin/python3 -I - <<'PY'
import os
fd=os.open('/var/lib/factory-runner',os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC);os.fsync(fd);os.close(fd)
PY
   echo 'factory runner committed transaction finalized'; exit 0
 else
   rc=$?; [[ $rc -ne 2 ]] || { echo 'committed journal does not match installed generation' >&2; exit 1; }
 fi
 /usr/bin/python3 -I - "$STATE/journal" <<'PY' | while IFS=$'\t' read -r name target disposition; do
import pathlib,sys
p=pathlib.Path(sys.argv[1])
if not p.exists(): raise SystemExit(0) # no durable intent means no mutation
rows=[];by_name={}
for line in p.read_text().splitlines():
 f=line.split('\t')
 if len(f)!=4: raise SystemExit('malformed install journal')
 generation,phase,name,target=f
 if name not in by_name: by_name[name]=[target,set()];rows.append(name)
 elif by_name[name][0]!=target: raise SystemExit('install journal target changed')
 by_name[name][1].add(phase)
for name in reversed(rows):
 target,phases=by_name[name]
 disposition='backup' if 'old-durable' in phases else 'missing' if 'missing-durable' in phases else 'intent-only'
 print(name+'\t'+target+'\t'+disposition)
PY
   [[ -n $name && -n $target ]] || continue
   if [[ $name == authorized_keys-* ]]; then
    /usr/bin/python3 -I - "$name" "$target" "$STATE" "$disposition" <<'PY'
import os,pathlib,pwd,stat,sys
name,target,state,disposition=sys.argv[1:];account=name.removeprefix('authorized_keys-');a=pwd.getpwnam(account);home=pathlib.Path(a.pw_dir)
if target!=str(home/'.ssh/authorized_keys'):raise SystemExit('rollback authorized_keys target mismatch')
fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC);rootdev=os.fstat(fd).st_dev
try:
 for part in home.parts[1:]:n=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);os.close(fd);fd=n
 hi=os.fstat(fd);sfd=os.open('.ssh',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);si=os.fstat(sfd)
 if hi.st_dev!=rootdev or si.st_dev!=hi.st_dev or hi.st_uid!=a.pw_uid or si.st_uid!=a.pw_uid or hi.st_gid!=a.pw_gid or si.st_gid!=a.pw_gid or stat.S_IMODE(hi.st_mode)!=0o700 or stat.S_IMODE(si.st_mode)!=0o700:raise SystemExit('unsafe account path during rollback')
 backup=pathlib.Path(state)/'backup'/name
 if disposition=='backup':
  # Never remove the active pathname until the journal proves the original
  # reached the durable backup directory and that backup is still present.
  if not backup.exists() or backup.is_symlink():raise SystemExit('durable authorized_keys backup is absent')
  try:os.unlink('authorized_keys',dir_fd=sfd)
  except FileNotFoundError:pass
  bfd=os.open(backup.parent,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC);os.rename(name,'authorized_keys',src_dir_fd=bfd,dst_dir_fd=sfd);os.fsync(bfd);os.close(bfd)
 elif disposition=='missing':
  try:os.unlink('authorized_keys',dir_fd=sfd)
  except FileNotFoundError:pass
 else:
  # Intent was durable before any mutation, but no durable outcome exists.
  # Preserve the pathname rather than risking destruction of the original.
  raise SystemExit('authorized_keys transaction has intent without durable backup/missing state')
 os.fsync(sfd)
finally:
 for x in ('sfd','fd'):
  try:os.close(locals()[x])
  except (KeyError,OSError):pass
PY
    continue
   fi
   if [[ -e $STATE/backup/$name || -L $STATE/backup/$name ]]; then
     /bin/rm -rf -- "$target.new-generation"; /bin/mv -T -- "$target" "$target.new-generation" 2>/dev/null || true
     /bin/mv -T -- "$STATE/backup/$name" "$target"
     /bin/rm -rf -- "$target.new-generation"
   elif [[ -f $STATE/missing/$name ]]; then /bin/rm -rf -- "$target" "$target.new-generation"; fi
   /usr/bin/python3 -I - "$target" <<'PY'
import os,sys
p=os.path.dirname(sys.argv[1]) or '/'; fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
PY
 done
 /bin/rm -rf -- "$STATE"
 /usr/bin/python3 -I - <<'PY'
import os
fd=os.open('/var/lib/factory-runner',os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC);os.fsync(fd);os.close(fd)
PY
 echo 'factory runner transaction rolled back'; exit 0
fi
if [[ $ACTION == verify ]]; then
 /usr/bin/python3 -I - <<'PY'
import grp,hashlib,json,os,pathlib,pwd,stat,subprocess,sys,tempfile
m={'devrunner':'dev-runner-vm','iprunner':'iprunner','gpurunner':'gpurunner'}
p=pathlib.Path('/etc/factory-runner/runner-policy.json');d=json.loads(p.read_bytes());transport=json.loads(pathlib.Path('/etc/factory-runner/transport-manifest.json').read_bytes())
bundle='/usr/local/libexec/factory-runner-v2.bundle';sys.path.insert(0,bundle)
from factory_runner_policy import load_policy
from factory_runner_authority import load_authority
# Verification must exercise the same canonical strict loaders as production,
# not an installer-local approximation of their schemas.
assert load_policy()==d
for cls in d['classes']:
 authority=load_authority(pathlib.Path(cls['probe_authority']),cls['probe_authority_sha256'])
 try:
  assert authority.digest==cls['probe_authority_sha256']
  assert sorted(authority.class_contract(cls['name'])['capabilities'])==sorted(cls['allowed_capabilities'])
 finally:authority.close()
assert {x['name'] for x in d['classes']}==set(m.values()) and set(transport['classes'])==set(m)
gen=json.loads(pathlib.Path('/etc/factory-runner/installed-generation.json').read_bytes());assert set(gen)=={'schema','generation_id','objects'} and gen['schema']=='factory-runner-installed-generation/v1'
assert hashlib.sha256(json.dumps(gen['objects'],sort_keys=True,separators=(',',':')).encode()).hexdigest()==gen['generation_id']
assert not pathlib.Path('/etc/sudoers.d/factory-runner-signer').exists()
for raw,desc in gen['objects'].items():
 q=pathlib.Path(raw);i=os.lstat(q)
 assert i.st_uid==desc['uid'] and i.st_gid==desc['gid']
 if desc['type']=='symlink':assert stat.S_ISLNK(i.st_mode) and os.readlink(q)==desc['target']
 else:
  assert stat.S_ISREG(i.st_mode) and stat.S_IMODE(i.st_mode)==desc['mode'] and i.st_nlink==1
  fd=os.open(q,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC);h=hashlib.sha256()
  try:
   while True:
    b=os.read(fd,65536)
    if not b:break
    h.update(b)
  finally:os.close(fd)
  assert h.hexdigest()==desc['sha256']
def read_account_key(a):
 home=pathlib.Path(a.pw_dir);fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC);rootdev=os.fstat(fd).st_dev
 try:
  for part in home.parts[1:]:n=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);os.close(fd);fd=n
  hi=os.fstat(fd);sfd=os.open('.ssh',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);si=os.fstat(sfd)
  assert hi.st_dev==rootdev and si.st_dev==hi.st_dev and hi.st_uid==a.pw_uid and si.st_uid==a.pw_uid and hi.st_gid==a.pw_gid and si.st_gid==a.pw_gid and stat.S_IMODE(hi.st_mode)==0o700 and stat.S_IMODE(si.st_mode)==0o700
  kfd=os.open('authorized_keys',os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=sfd);ki=os.fstat(kfd);raw=os.read(kfd,65537);assert not os.read(kfd,1);os.close(kfd);os.close(sfd)
  assert stat.S_ISREG(ki.st_mode) and ki.st_nlink==1 and ki.st_uid==a.pw_uid and ki.st_gid==a.pw_gid and stat.S_IMODE(ki.st_mode)==0o600
  return raw.decode().splitlines()
 finally:os.close(fd)
for c in d['classes']:
 a=pwd.getpwuid(c['uid']);assert m[a.pw_name]==c['name']
 for pin in c['executable_pins'].values():
  q=pathlib.Path(pin['path']);i=os.stat(q);h=hashlib.sha256(q.read_bytes()).hexdigest()
  assert pin['status']=='enrolled' and (i.st_dev,i.st_ino)==(pin['device'],pin['inode']) and h==pin['sha256']
 if any(x in c['allowed_capabilities'] for x in ('inputplumber-system-dbus','target-consumer','controller-production-routing','gpu-compositor','installed-licensed-diagram')):
  pin=c['inputplumber_pin'];q=pathlib.Path(pin['path']);i=os.stat(q)
  assert pin['status']=='enrolled' and (i.st_dev,i.st_ino)==(pin['device'],pin['inode']) and hashlib.sha256(q.read_bytes()).hexdigest()==pin['sha256']
  assert subprocess.check_output(['/usr/bin/dpkg-query','-W','-f=${Version}','inputplumber'],text=True)==pin['package_version']
 groups={grp.getgrgid(g).gr_name for g in os.getgrouplist(a.pw_name,a.pw_gid)};assert groups==set(c['approved_groups'])
 lines=read_account_key(a);assert len(lines)==1
 prefix='command="/usr/local/libexec/factory-runner-server",restrict,no-agent-forwarding,no-port-forwarding,no-pty,no-user-rc,no-X11-forwarding '
 assert lines[0].startswith(prefix) and hashlib.sha256((lines[0][len(prefix):]+'\n').encode()).hexdigest()==transport['classes'][a.pw_name]['sha256']
 principal=pathlib.Path('/etc/factory-runner/principals')/a.pw_name;ps=principal.stat()
 assert ps.st_uid==0 and stat.S_IMODE(ps.st_mode)==0o600 and principal.read_text()==transport['classes'][a.pw_name]['principal']+'\n'
 key=pathlib.Path(c['signer_key']);ks=os.lstat(key)
 assert stat.S_ISREG(ks.st_mode) and ks.st_uid==0 and ks.st_nlink==1 and stat.S_IMODE(ks.st_mode)==0o600
 policy_principal=pathlib.Path(c['signer_principal_file']);pps=os.lstat(policy_principal)
 assert stat.S_ISREG(pps.st_mode) and pps.st_uid==0 and pps.st_nlink==1 and not pps.st_mode&0o022
 # Controlled, local proof that the enrolled key can sign and verify. The
 # one-use challenge/signature live only in root-private temporary storage.
 with tempfile.TemporaryDirectory(prefix='factory-signer-selftest-') as td:
  challenge=pathlib.Path(td)/'challenge';challenge.write_bytes(os.urandom(32)+gen['generation_id'].encode())
  subprocess.run(['/usr/bin/ssh-keygen','-Y','sign','-q','-f',str(key),'-n','factory-runner-self-test',str(challenge)],check=True,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
  public=subprocess.check_output(['/usr/bin/ssh-keygen','-y','-f',str(key)],text=True).strip();allowed=pathlib.Path(td)/'allowed';allowed.write_text(transport['classes'][a.pw_name]['principal']+' '+public+'\n')
  with challenge.open('rb') as inp:subprocess.run(['/usr/bin/ssh-keygen','-Y','verify','-q','-f',str(allowed),'-I',transport['classes'][a.pw_name]['principal'],'-n','factory-runner-self-test','-s',str(challenge)+'.sig'],stdin=inp,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
for p in ('/opt/factory-runner/authority/v1','/usr/local/libexec/factory-runner-v2.bundle','/etc/sudoers.d/factory-runner-broker'):
 assert pathlib.Path(p).exists() and not pathlib.Path(p).is_symlink()
for root in ('/opt/factory-runner/authority/v1','/usr/local/libexec/factory-runner-v2.bundle'):
 actual={str(x) for x in pathlib.Path(root).rglob('*') if x.is_file() or x.is_symlink()}
 expected={x for x in gen['objects'] if x.startswith(root+'/')}
 assert actual==expected
# Import/start checks use the canonical installed bundle explicitly, never the
# compatibility symlink's dirname.  They expose no request or signing channel.
for script in ('factory-runner-server.py','factory-runner-broker.py','factory-runner-signer.py','inputplumber-dbus-audit.py'):
 code="import importlib.util; p=%r+'/'+%r; s=importlib.util.spec_from_file_location('installed_dry',p); m=importlib.util.module_from_spec(s); s.loader.exec_module(m)"%(bundle,script)
 subprocess.run(['/usr/bin/python3','-I','-c',code],check=True,stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL)
print('factory runner installed generation verified')
PY
 exit
fi
[[ $ACTION == install ]] || usage
[[ ${FACTORY_RUNNER_AUTHENTICATED_BOOTSTRAP:-} == 1 ]] || { echo 'refusing unauthenticated checkout execution; use the root-owned factory-runner-root-bootstrap' >&2; exit 1; }
SOURCE=''; MANIFEST=''; COMMIT=''; TREE=''; POLICY=''; TRANSPORT=''; LAUNCHER=''
declare -A KEYS=()
while (($#)); do
 case $1 in
  --source-root) SOURCE=$2;shift 2;; --install-manifest) MANIFEST=$2;shift 2;; --commit) COMMIT=$2;shift 2;; --tree) TREE=$2;shift 2;; --policy) POLICY=$2;shift 2;; --transport-manifest) TRANSPORT=$2;shift 2;;
  --ssh-launcher-manifest) LAUNCHER=$2;shift 2;; --key) [[ $2 == *=* ]] || usage; KEYS[${2%%=*}]=${2#*=};shift 2;; *) usage;; esac
done
[[ -n $SOURCE && -n $MANIFEST && -n $POLICY && -n $TRANSPORT && -n $LAUNCHER ]] || usage
[[ ${#KEYS[@]} -eq 3 && -n ${KEYS[devrunner]:-} && -n ${KEYS[iprunner]:-} && -n ${KEYS[gpurunner]:-} ]] || usage
if [[ -e $STATE ]]; then
 echo 'recovering interrupted factory-runner transaction by deterministic rollback' >&2
 "$0" rollback
fi
/bin/mkdir -p /var/lib/factory-runner; /bin/chmod 0700 /var/lib/factory-runner
/bin/mkdir -m 0700 "$STATE" "$STATE/snapshot" "$STATE/stage" "$STATE/backup" "$STATE/missing"
/usr/bin/python3 -I - <<'PY'
import os
for p in ('/var/lib/factory-runner/install-transaction','/var/lib/factory-runner'):
 fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC);os.fsync(fd);os.close(fd)
PY
rollback_signal(){ rc=$?; trap - EXIT INT TERM HUP; "$0" rollback || true; exit "$rc"; }
trap rollback_signal EXIT INT TERM HUP
# Snapshot/validate all source and external authority through no-follow held FDs.
/usr/bin/python3 -I - "$SOURCE" "$MANIFEST" "$COMMIT" "$TREE" "$POLICY" "$TRANSPORT" "$LAUNCHER" "${KEYS[devrunner]}" "${KEYS[iprunner]}" "${KEYS[gpurunner]}" "$STATE" <<'PY'
import base64,grp,hashlib,json,os,pathlib,pwd,re,shutil,stat,subprocess,sys
source,mp,commit,tree,pp,tp,lp,*rest=sys.argv[1:]; keypaths=dict(zip(('devrunner','iprunner','gpurunner'),rest[:3]));state=pathlib.Path(rest[3]);source=pathlib.Path(source)
def die(s):raise SystemExit('installer preflight: '+s)
def protected(path):
 p=pathlib.Path(path)
 if not p.is_absolute():die(f'external descriptor is not absolute: {p}')
 cur=pathlib.Path('/')
 for part in p.parts[1:-1]:
  cur/=part;i=os.lstat(cur)
  if stat.S_ISLNK(i.st_mode) or i.st_uid!=0 or i.st_mode&0o022:die(f'unsafe external descriptor ancestor: {cur}')
 before=os.lstat(p);fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC);opened=os.fstat(fd)
 if (before.st_dev,before.st_ino)!=(opened.st_dev,opened.st_ino) or not stat.S_ISREG(opened.st_mode) or opened.st_uid!=0 or opened.st_nlink!=1 or opened.st_mode&0o022:die(f'unsafe external descriptor: {p}')
 data=b''
 while True:
  b=os.read(fd,65536)
  if not b:break
  data+=b
  if len(data)>128*1024*1024:die(f'external descriptor too large: {p}')
 after=os.lstat(p);os.close(fd)
 if (after.st_dev,after.st_ino,after.st_size)!=(opened.st_dev,opened.st_ino,opened.st_size):die(f'external descriptor raced: {p}')
 return data
raw_manifest=protected(mp)
try:m=json.loads(raw_manifest)
except Exception:die('installation manifest is invalid JSON')
expected={'schema','commit','tree','commit_object_b64','source_merkle_sha256','files','installer','authority_builder','symlinks','submodules'}
if set(m)!=expected or m.get('schema')!='factory-runner-install-manifest/v2':die('installation manifest fields/schema invalid')
if not commit:commit=m['commit']
if not tree:tree=m['tree']
if m['commit']!=commit or m['tree']!=tree or not re.fullmatch('[0-9a-f]{40,64}',commit) or not re.fullmatch('[0-9a-f]{40,64}',tree):die('explicit commit/tree binding mismatch')
if m['installer']!='.factory/runner/install-factory-runner-v2.sh' or m['authority_builder']!='.factory/runner/build-runner-probe-authority.py' or m['symlinks']!='reject' or m['submodules']!='reject':die('source policy binding mismatch')
try:co=base64.b64decode(m['commit_object_b64'],validate=True)
except Exception:die('commit object encoding invalid')
def git_hash(kind,data,nhex):
 framed=kind.encode()+b' '+str(len(data)).encode()+b'\0'+data
 return (hashlib.sha1(framed) if nhex==40 else hashlib.sha256(framed)).hexdigest()
if git_hash('commit',co,len(commit))!=commit or not co.startswith(f'tree {tree}\n'.encode()):die('commit object/tree binding invalid')
files=m['files']
required={'.factory/runner/install-factory-runner-v2.sh','.factory/runner/factory-runner-root-bootstrap','.factory/runner/factory-runner-broker.py','.factory/runner/factory-runner-signer.py','.factory/runner/factory-runner-server.py','.factory/runner/inputplumber-dbus-audit.py','.factory/runner/inputplumber-mediator.c','.factory/runner/factory_runner_policy.py','.factory/runner/factory_runner_artifacts.py','.factory/runner/factory_runner_authority.py','.factory/runner/build-runner-probe-authority.py','deploy/factory-runner-authority-v1/authority.json'}
if not isinstance(files,dict) or not required.issubset(files):die('installation closure is incomplete')
closure=hashlib.sha256()
for rel,desc in sorted(files.items()):
 closure.update(rel.encode()+b'\0');closure.update(json.dumps(desc,sort_keys=True,separators=(',',':')).encode()+b'\0')
if closure.hexdigest()!=m['source_merkle_sha256']:die('source Merkle digest mismatch')
snap=state/'snapshot'/'source';snap.mkdir();trie={}
for rel,desc in sorted(files.items()):
 expected_desc={'blob','sha256','mode','size'}
 if not isinstance(rel,str) or pathlib.PurePosixPath(rel).is_absolute() or '..' in pathlib.PurePosixPath(rel).parts or set(desc)!=expected_desc:die('invalid source manifest entry')
 p=source/rel;before=os.lstat(p)
 if stat.S_ISLNK(before.st_mode) or not stat.S_ISREG(before.st_mode):die(f'source path is not regular/no-follow: {rel}')
 fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC);opened=os.fstat(fd);data=b''
 while True:
  b=os.read(fd,65536)
  if not b:break
  data+=b
 after=os.lstat(p);os.close(fd)
 if (before.st_dev,before.st_ino)!=(opened.st_dev,opened.st_ino) or (after.st_dev,after.st_ino,after.st_size)!=(opened.st_dev,opened.st_ino,opened.st_size):die(f'mutable source race: {rel}')
 if hashlib.sha256(data).hexdigest()!=desc['sha256'] or len(data)!=desc['size'] or stat.S_IMODE(opened.st_mode)!=desc['mode']:die(f'source digest/mode mismatch: {rel}')
 if not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}',str(desc['blob'])) or len(desc['blob'])!=len(tree) or git_hash('blob',data,len(desc['blob']))!=desc['blob']:die(f'Git blob mismatch: {rel}')
 parts=pathlib.PurePosixPath(rel).parts;node=trie
 for part in parts[:-1]:
  if part in node and not isinstance(node[part],dict):die('Git tree file/directory collision')
  node=node.setdefault(part,{})
 if parts[-1] in node:die('duplicate Git tree path')
 node[parts[-1]]=(desc['mode'],desc['blob'])
 out=snap/rel;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data);out.chmod(desc['mode'])
def tree_oid(node):
 body=bytearray()
 for name,value in sorted(node.items(),key=lambda x:x[0].encode()+(b'/' if isinstance(x[1],dict) else b'')):
  if isinstance(value,dict):mode='40000';oid=tree_oid(value)
  else:mode='100755' if value[0]==0o755 else '100644';oid=value[1]
  body.extend(mode.encode()+b' '+name.encode()+b'\0'+bytes.fromhex(oid))
 return git_hash('tree',bytes(body),len(tree))
if tree_oid(trie)!=tree:die('manifest does not reconstruct declared Git tree')
# Exact set: the trusted manifest, not a later checkout walk, defines the closure.
(state/'snapshot'/'install-manifest.json').write_bytes(raw_manifest)
policy_raw=protected(pp);transport_raw=protected(tp);launcher_raw=protected(lp);(state/'snapshot'/'policy.json').write_bytes(policy_raw);(state/'snapshot'/'transport.json').write_bytes(transport_raw);(state/'snapshot'/'ssh-launcher.json').write_bytes(launcher_raw)
for a,p in keypaths.items():(state/'snapshot'/f'{a}.pub').write_bytes(protected(p))
# Policy is parsed by this fixed bootstrap implementation.  Root never imports
# Python from the candidate tree, even after signature verification.
try:policy=json.loads(policy_raw)
except Exception:die('runner policy invalid JSON')
if not isinstance(policy,dict) or set(policy)!={'schema','namespace','classes','authority_pins'} or policy['schema']!='factory-runner-policy/v3' or policy['namespace']!='factory-runner-receipt' or not isinstance(policy['classes'],list):die('runner policy schema invalid')
required_class_fields={'name','uid','workspace_root','allowed_capabilities','broker_helper','probe_authority','probe_authority_sha256','probe_authority_status','signer_key','signer_principal_file','nonce_ledger','systemd_run','systemctl','cgroup_root','dbus_proxy','approved_groups','executable_pins','inputplumber_pin'}
for c in policy['classes']:
 if not isinstance(c,dict) or set(c)!=required_class_fields or type(c['uid']) is not int or c['uid']<=0 or c['probe_authority_status']!='enrolled' or not isinstance(c['approved_groups'],list) or not c['approved_groups']:die('runner policy class invalid or not enrolled')
 for field in ('workspace_root','broker_helper','probe_authority','signer_key','signer_principal_file','nonce_ledger','systemd_run','systemctl','cgroup_root','dbus_proxy'):
  if not isinstance(c[field],str) or not c[field].startswith('/') or '..' in pathlib.PurePosixPath(c[field]).parts:die('runner policy path invalid')
 required_pins={'systemd-run','systemctl','xdg-dbus-proxy','git','bash','python3','ssh-keygen','sudo','busctl','mount','umount','udevadm','stdbuf','dpkg-query','inputplumber-mediator'}
 if not isinstance(c['executable_pins'],dict) or set(c['executable_pins'])!=required_pins:die('executable enrollment set is incomplete')
 for pin in c['executable_pins'].values():
  if not isinstance(pin,dict) or set(pin)!={'path','sha256','device','inode','status'} or pin['status']!='enrolled':die('executable enrollment is invalid or pending')
  raw=protected(pin['path']);i=os.stat(pin['path'])
  if hashlib.sha256(raw).hexdigest()!=pin['sha256'] or (i.st_dev,i.st_ino)!=(pin['device'],pin['inode']):die('executable enrollment mismatch')
 needs_ip=any(x in c['allowed_capabilities'] for x in ('inputplumber-system-dbus','target-consumer','controller-production-routing','gpu-compositor','installed-licensed-diagram'))
 if needs_ip and (not isinstance(c['inputplumber_pin'],dict) or c['inputplumber_pin'].get('status')!='enrolled'):die('InputPlumber enrollment is absent or pending')
 if not needs_ip and c['inputplumber_pin'] is not None:die('unexpected InputPlumber enrollment')
account_map={'devrunner':'dev-runner-vm','iprunner':'iprunner','gpurunner':'gpurunner'}
if {c['name'] for c in policy['classes']}!=set(account_map.values()):die('policy class set is not exact')
# System tools must be immutable root-owned executables.  The coordinator
# launcher additionally has the one canonical device/inode/digest schema used
# by runtime and installer.
needed={'/usr/bin/xdg-dbus-proxy','/usr/bin/systemd-run','/usr/bin/systemctl','/usr/bin/sudo','/usr/bin/python3','/usr/bin/bash','/usr/bin/ssh-keygen','/usr/bin/busctl','/usr/sbin/visudo'}
for name in needed:
 data=protected(name);i=os.stat(name)
 if not i.st_mode&0o111:die(f'executable is not executable: {name}')
try:launcher=json.loads(launcher_raw)
except Exception:die('SSH launcher manifest invalid JSON')
if set(launcher)!={'schema','path','sha256','device','inode'} or launcher['schema']!='factory-ssh-launcher/v1' or type(launcher['device']) is not int or type(launcher['inode']) is not int:die('trusted SSH launcher schema invalid')
lpinned=pathlib.Path(launcher['path']);ldata=protected(lpinned);li=os.stat(lpinned)
if hashlib.sha256(ldata).hexdigest()!=launcher['sha256'] or (li.st_dev,li.st_ino)!=(launcher['device'],launcher['inode']) or not li.st_mode&0o111:die('trusted SSH launcher pin mismatch')
# cgroup v2 must be the actual unified mount and expose cleanup state.
mounts=pathlib.Path('/proc/self/mountinfo').read_text().splitlines()
if not any(' - cgroup2 ' in x and ' /sys/fs/cgroup ' in x for x in mounts):die('unified cgroup v2 is not mounted at /sys/fs/cgroup')
for n in ('cgroup.controllers','cgroup.procs','cgroup.events'):
 if not (pathlib.Path('/sys/fs/cgroup')/n).is_file():die(f'cgroup v2 control unreadable: {n}')
# Deep disposable transient unit: failure of any production property aborts before mutation.
props=['PrivatePIDs=yes','PrivateMounts=yes','ProtectSystem=strict','ProtectHome=yes','BindReadOnlyPaths=/usr','DevicePolicy=closed','RestrictAddressFamilies=AF_UNIX','NoNewPrivileges=yes','CapabilityBoundingSet=','TasksMax=16','MemoryMax=64M','KillMode=control-group']
unit='factory-runner-install-preflight-'+os.urandom(6).hex()+'.service'
cmd=['/usr/bin/systemd-run','--quiet','--unit',unit,*sum((['--property',x] for x in props),[]),'/usr/bin/sleep','30']
r=subprocess.run(cmd,capture_output=True)
if r.returncode:die('systemd containment property probe failed')
try:
 show=subprocess.check_output(['/usr/bin/systemctl','show',unit,'--property=ControlGroup,PrivatePIDs,PrivateMounts,ProtectSystem,ProtectHome,DevicePolicy,RestrictAddressFamilies,NoNewPrivileges,CapabilityBoundingSet,TasksMax,MemoryMax,KillMode'],text=True)
 values=dict(x.split('=',1) for x in show.splitlines() if '=' in x);cg=values.get('ControlGroup','')
 expected={'PrivatePIDs':'yes','PrivateMounts':'yes','ProtectSystem':'strict','ProtectHome':'yes','DevicePolicy':'closed','RestrictAddressFamilies':'AF_UNIX','NoNewPrivileges':'yes','CapabilityBoundingSet':'','TasksMax':'16','MemoryMax':str(64*1024*1024),'KillMode':'control-group'}
 if not cg.startswith('/') or any(values.get(k)!=v for k,v in expected.items()):die('systemd did not apply exact containment properties')
 cgp=pathlib.Path('/sys/fs/cgroup'+cg)
 for n in ('cgroup.procs','cgroup.events'):
  if not (cgp/n).is_file():die('transient unit cgroup cleanup state unreadable')
finally:
 subprocess.run(['/usr/bin/systemctl','stop',unit],check=False);subprocess.run(['/usr/bin/systemctl','reset-failed',unit],check=False)
for _ in range(50):
 if not cgp.exists():break
 import time;time.sleep(.1)
else:die('transient probe cgroup was not cleaned up')
# Account/class/group, authentication, shell, home, and .ssh are checked
# before staging or mutation.  Directory descriptors are opened one component
# at a time with O_NOFOLLOW; a device change is a mount escape.
def trusted_account_dir(path,uid,gid,mode):
 p=pathlib.Path(path)
 if not p.is_absolute():die('account directory is not absolute')
 fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC);rootdev=os.fstat(fd).st_dev
 try:
  for idx,part in enumerate(p.parts[1:]):
   nfd=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);os.close(fd);fd=nfd;i=os.fstat(fd)
   if i.st_dev!=rootdev:die(f'account path mount escape: {p}')
   if idx==len(p.parts)-2 and (i.st_uid!=uid or i.st_gid!=gid or stat.S_IMODE(i.st_mode)!=mode):die(f'account directory metadata mismatch: {p}')
  return os.dup(fd)
 finally:os.close(fd)
for c in policy['classes']:
 a=pwd.getpwuid(c['uid'])
 if account_map.get(a.pw_name)!=c['name']:die('numeric UID/account/class mapping mismatch')
 if a.pw_shell not in ('/usr/sbin/nologin','/sbin/nologin','/bin/false') or not (a.pw_passwd.startswith('!') or a.pw_passwd.startswith('*')):die('runner account must be locked with an approved non-login shell')
 groups={grp.getgrgid(x).gr_name for x in os.getgrouplist(a.pw_name,a.pw_gid)}
 if groups!=set(c['approved_groups']) or grp.getgrgid(a.pw_gid).gr_name not in groups:die('primary/supplementary group set mismatch')
 hfd=trusted_account_dir(a.pw_dir,a.pw_uid,a.pw_gid,0o700)
 try:
  sfd=os.open('.ssh',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=hfd);si=os.fstat(sfd)
  if si.st_dev!=os.fstat(hfd).st_dev or si.st_uid!=a.pw_uid or si.st_gid!=a.pw_gid or stat.S_IMODE(si.st_mode)!=0o700:die('unsafe account .ssh directory')
  os.close(sfd)
 except FileNotFoundError:die('dedicated account .ssh must be pre-created by trusted account provisioning')
 finally:os.close(hfd)
 for p in (pathlib.Path(c['workspace_root']),pathlib.Path(c['nonce_ledger']).parent):
  if not p.exists() or p.is_symlink():die(f'required policy resource absent/unsafe: {p}')
# Real InputPlumber and GPU prerequisites are mandatory when their classes are enrolled.
if any('inputplumber-system-dbus' in c['allowed_capabilities'] for c in policy['classes']):
 if subprocess.run(['/usr/bin/systemctl','is-active','--quiet','inputplumber.service']).returncode:die('real InputPlumber service is not active')
 if not pathlib.Path('/run/dbus/system_bus_socket').is_socket():die('InputPlumber system bus prerequisite absent')
 ui=os.stat('/dev/uinput',follow_symlinks=False)
 if not stat.S_ISCHR(ui.st_mode):die('/dev/uinput is not the exact character device')
 expect=json.loads((snap/'deploy/factory-runner-authority-v1/iprunner-probes/inputplumber-expectations.json').read_bytes())
 version=subprocess.check_output(['/usr/bin/busctl','--system','get-property','org.shadowblip.InputPlumber','/org/shadowblip/InputPlumber/Manager','org.shadowblip.InputManager','Version'],text=True).strip().split(maxsplit=1)[-1].strip('"')
 if version!=expect['manager']['version']:die('active InputPlumber version differs from authority')
if any(c['name']=='gpurunner' for c in policy['classes']):
 dri=os.stat('/dev/dri/renderD128',follow_symlinks=False)
 if not stat.S_ISCHR(dri.st_mode):die('GPU DRM render node is not a character device')
# Transport authority is exact, independent, and supplies one pinned key/principal per account.
try:t=json.loads(transport_raw)
except Exception:die('transport manifest invalid JSON')
if set(t)!={'schema','classes'} or t['schema']!='factory-runner-transport/v1' or set(t['classes'])!=set(account_map):die('transport manifest class set invalid')
stage=state/'stage';auth=snap/'deploy/factory-runner-authority-v1';authority=json.loads((auth/'authority.json').read_bytes())
actual={p.relative_to(auth).as_posix() for p in auth.rglob('*') if p.is_file()}
if actual!={'authority.json',*authority['files'].keys()}:die('authority file set differs from manifest')
for rel,d in authority['files'].items():
 if hashlib.sha256((auth/rel).read_bytes()).hexdigest()!=d:die(f'authority digest mismatch: {rel}')
shutil.copytree(auth,stage/'authority')
lib=stage/'libexec';lib.mkdir()
for n in ('factory-runner-broker.py','factory-runner-signer.py','factory-runner-server.py','inputplumber-dbus-audit.py','factory_runner_policy.py','factory_runner_artifacts.py','factory_runner_authority.py'):
 shutil.copyfile(snap/'scripts'/n,lib/n);(lib/n).chmod(0o700 if '-' in n and n!='factory-runner-server.py' else 0o755)
(stage/'policy').write_bytes(policy_raw);(stage/'policy').chmod(0o640)
(stage/'transport').write_bytes(transport_raw);(stage/'transport').chmod(0o600)
(stage/'ssh-launcher').write_bytes(launcher_raw);(stage/'ssh-launcher').chmod(0o600)
(stage/'installer').write_bytes((snap/'.factory/runner/install-factory-runner-v2.sh').read_bytes());(stage/'installer').chmod(0o700)
lines=[]
for c in policy['classes']:
 a=pwd.getpwuid(c['uid']);lines.append(f'{a.pw_name} ALL=(root) NOPASSWD: /usr/local/libexec/factory-runner-broker')
 tc=t['classes'][a.pw_name]
 if set(tc)!={'class','sha256','fingerprint','principal'} or tc['class']!=c['name']:die('transport class/principal map mismatch')
 key_raw=(state/'snapshot'/f'{a.pw_name}.pub').read_text();key=key_raw.removesuffix('\n')
 if key_raw!=key+'\n' or '\n' in key or not re.fullmatch(r'ssh-ed25519 [A-Za-z0-9+/]+={0,2}(?: [^\r\n]+)?',key):die('transport key must be exactly one newline-terminated ed25519 public key')
 if hashlib.sha256((key+'\n').encode()).hexdigest()!=tc['sha256']:die('transport key digest mismatch')
 kf=state/'snapshot'/f'{a.pw_name}.pub';got=subprocess.check_output(['/usr/bin/ssh-keygen','-lf',str(kf),'-E','sha256'],text=True).split()[1]
 if got!=tc['fingerprint']:die('transport key fingerprint mismatch')
 forced='command="/usr/local/libexec/factory-runner-server",restrict,no-agent-forwarding,no-port-forwarding,no-pty,no-user-rc,no-X11-forwarding '+key+'\n'
 (stage/f'authorized_keys.{a.pw_name}').write_text(forced);(stage/f'authorized_keys.{a.pw_name}').chmod(0o600);os.chown(stage/f'authorized_keys.{a.pw_name}',a.pw_uid,a.pw_gid)
 (stage/f'principal.{a.pw_name}').write_text(tc['principal']+'\n');(stage/f'principal.{a.pw_name}').chmod(0o600)
(stage/'sudoers').write_text('\n'.join(lines)+'\n');(stage/'sudoers').chmod(0o440);subprocess.run(['/usr/sbin/visudo','-cf',str(stage/'sudoers')],check=True)
for p in stage.rglob('*'):
 if p.is_symlink():die('staged closure contains symlink')
 if not p.name.startswith('authorized_keys.'):os.chown(p,0,0)
 if p.is_file():
  fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW);os.fsync(fd);os.close(fd)
for p in sorted((x for x in stage.rglob('*') if x.is_dir()),key=lambda x:len(x.parts),reverse=True):
 fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW);os.fsync(fd);os.close(fd)
fd=os.open(stage,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW);os.fsync(fd);os.close(fd)
PY
/bin/mkdir -p /opt/factory-runner/authority /usr/local/libexec /usr/local/sbin /etc/factory-runner /etc/sudoers.d /etc/factory-runner/principals /etc/controller-box
# Stage compatibility links before any cutover.
for name in factory-runner-broker factory-runner-signer factory-runner-server; do /bin/ln -s "factory-runner-v2.bundle/$name.py" "$STATE/stage/$name"; done
# Seal the complete staged generation. Installed verify rehashes this exact set.
/usr/bin/python3 -I - "$STATE/stage" <<'PY'
import hashlib,json,os,pathlib,stat,sys
s=pathlib.Path(sys.argv[1]);mapping={'authority':'/opt/factory-runner/authority/v1','libexec':'/usr/local/libexec/factory-runner-v2.bundle','policy':'/etc/factory-runner/runner-policy.json','transport':'/etc/factory-runner/transport-manifest.json','ssh-launcher':'/etc/controller-box/factory-ssh-launcher.json','sudoers':'/etc/sudoers.d/factory-runner-broker','installer':'/usr/local/sbin/install-factory-runner-v2'}
for n in ('factory-runner-broker','factory-runner-signer','factory-runner-server'):mapping[n]='/usr/local/libexec/'+n
for n in ('devrunner','iprunner','gpurunner'):mapping['principal.'+n]='/etc/factory-runner/principals/'+n
objects={}
for src,dst in mapping.items():
 p=s/src
 if p.is_dir():
  for q in sorted(p.rglob('*')):
   if q.is_file():
    i=q.stat();objects[dst+'/'+q.relative_to(p).as_posix()]={'type':'file','sha256':hashlib.sha256(q.read_bytes()).hexdigest(),'mode':stat.S_IMODE(i.st_mode),'uid':i.st_uid,'gid':i.st_gid}
 elif p.is_symlink():objects[dst]={'type':'symlink','target':os.readlink(p),'uid':0,'gid':0}
 else:
  i=p.stat();objects[dst]={'type':'file','sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'mode':stat.S_IMODE(i.st_mode),'uid':i.st_uid,'gid':i.st_gid}
canonical=json.dumps(objects,sort_keys=True,separators=(',',':')).encode()
d={'schema':'factory-runner-installed-generation/v1','generation_id':hashlib.sha256(canonical).hexdigest(),'objects':objects}
(s/'generation').write_text(json.dumps(d,sort_keys=True,separators=(',',':'))+'\n');(s/'generation').chmod(0o600)
PY
GENERATION_ID=$(/usr/bin/python3 -I -c 'import json;print(json.load(open("/var/lib/factory-runner/install-transaction/stage/generation"))["generation_id"])')
journal(){
 printf '%s\t%s\t%s\t%s\n' "$GENERATION_ID" "$1" "$2" "$3" >> "$STATE/journal"
 /usr/bin/python3 -I - "$STATE/journal" <<'PY'
import os,sys
fd=os.open(sys.argv[1],os.O_RDONLY);os.fsync(fd);os.close(fd);fd=os.open(os.path.dirname(sys.argv[1]),os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
PY
 [[ -z ${FACTORY_INSTALL_CRASH_AFTER_STEP:-} || ${FACTORY_INSTALL_CRASH_AFTER_STEP} != "$1:$2" ]] || /bin/kill -KILL $$
}
rename_noreplace(){ /usr/bin/python3 -I - "$1" "$2" <<'PY'
import ctypes,os,sys
s,d=sys.argv[1:];libc=ctypes.CDLL(None,use_errno=True)
if libc.renameat2(-100,os.fsencode(s),-100,os.fsencode(d),1):raise OSError(ctypes.get_errno(),'RENAME_NOREPLACE')
for p in {os.path.dirname(s),os.path.dirname(d)}:
 fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
PY
}
cutover(){
 local name=$1 staged=$2 target=$3 backup="$STATE/backup/$1"
 journal intent "$name" "$target"
 /bin/mkdir -p -- "$(/usr/bin/dirname -- "$target")"
 /usr/bin/python3 -I - "$target" <<'PY'
import os,sys
p=os.path.dirname(sys.argv[1]) or '/';fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC);os.fsync(fd);os.close(fd)
PY
 if [[ -e $target || -L $target ]]; then
  journal before-backup "$name" "$target"; rename_noreplace "$target" "$backup"; journal old-durable "$name" "$target"
 else
  journal before-missing "$name" "$target"
  /usr/bin/python3 -I - "$STATE/missing" "$name" <<'PY'
import os,sys
root,name=sys.argv[1:];d=os.open(root,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
try:
 fd=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW|os.O_CLOEXEC,0o600,dir_fd=d);os.fsync(fd);os.close(fd);os.fsync(d)
finally:os.close(d)
PY
  journal missing-durable "$name" "$target"
 fi
 journal before-activate "$name" "$target"; rename_noreplace "$staged" "$target"; journal active-durable "$name" "$target"
}
cutover authority "$STATE/stage/authority" /opt/factory-runner/authority/v1
cutover libexec "$STATE/stage/libexec" /usr/local/libexec/factory-runner-v2.bundle
cutover installer "$STATE/stage/installer" /usr/local/sbin/install-factory-runner-v2
cutover policy "$STATE/stage/policy" /etc/factory-runner/runner-policy.json
cutover transport "$STATE/stage/transport" /etc/factory-runner/transport-manifest.json
cutover ssh-launcher "$STATE/stage/ssh-launcher" /etc/controller-box/factory-ssh-launcher.json
cutover generation "$STATE/stage/generation" /etc/factory-runner/installed-generation.json
cutover broker-sudoers "$STATE/stage/sudoers" /etc/sudoers.d/factory-runner-broker
for name in factory-runner-broker factory-runner-signer factory-runner-server; do cutover "$name" "$STATE/stage/$name" "/usr/local/libexec/$name"; done
secure_ssh_cutover(){
 /usr/bin/python3 -I - "$1" "$2" "$STATE" <<'PY'
import json,os,pathlib,pwd,stat,sys
account,staged,state=sys.argv[1:];a=pwd.getpwnam(account);state=pathlib.Path(state);home=pathlib.Path(a.pw_dir);name='authorized_keys-'+account;target=str(home/'.ssh/authorized_keys');generation=json.loads((state/'stage/generation').read_bytes())['generation_id']
def sync(fd):os.fsync(fd)
def event(phase):
 fd=os.open(state/'journal',os.O_WRONLY|os.O_APPEND|os.O_CLOEXEC);os.write(fd,f'{generation}\t{phase}\t{name}\t{target}\n'.encode());sync(fd);os.close(fd);d=os.open(state,os.O_RDONLY|os.O_DIRECTORY);sync(d);os.close(d)
 if os.environ.get('FACTORY_INSTALL_CRASH_AFTER_STEP')==phase+':'+name:os.kill(os.getppid(),9)
fd=os.open('/',os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC);rootdev=os.fstat(fd).st_dev
try:
 for part in home.parts[1:]:n=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);os.close(fd);fd=n
 hi=os.fstat(fd)
 if hi.st_dev!=rootdev or hi.st_uid!=a.pw_uid or hi.st_gid!=a.pw_gid or stat.S_IMODE(hi.st_mode)!=0o700:raise SystemExit('unsafe account home during cutover')
 sfd=os.open('.ssh',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC,dir_fd=fd);si=os.fstat(sfd)
 if si.st_dev!=hi.st_dev or si.st_uid!=a.pw_uid or si.st_gid!=a.pw_gid or stat.S_IMODE(si.st_mode)!=0o700:raise SystemExit('unsafe account .ssh during cutover')
 bfd=os.open(state/'backup',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC);mfd=os.open(state/'missing',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC);stagefd=os.open(pathlib.Path(staged).parent,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW|os.O_CLOEXEC)
 event('intent')
 try:os.stat('authorized_keys',dir_fd=sfd,follow_symlinks=False);exists=True
 except FileNotFoundError:exists=False
 if exists:
  event('before-backup');os.rename('authorized_keys',name,src_dir_fd=sfd,dst_dir_fd=bfd);sync(sfd);sync(bfd);event('old-durable')
 else:
  event('before-missing');x=os.open(name,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,dir_fd=mfd);sync(x);os.close(x);sync(mfd);event('missing-durable')
 event('before-activate');os.rename(pathlib.Path(staged).name,'authorized_keys',src_dir_fd=stagefd,dst_dir_fd=sfd);sync(stagefd);sync(sfd);event('active-durable')
finally:
 for x in ('sfd','bfd','mfd','stagefd','fd'):
  try:os.close(locals()[x])
  except (KeyError,OSError):pass
PY
}
for account in devrunner iprunner gpurunner; do
 secure_ssh_cutover "$account" "$STATE/stage/authorized_keys.$account"
 cutover "principal-$account" "$STATE/stage/principal.$account" "/etc/factory-runner/principals/$account"
done
# Obsolete signer sudoers participates in the same journal.  A harmless empty
# staged marker lets cutover preserve old bytes durably; the marker is removed
# only after the generation verify succeeds.
/bin/touch "$STATE/stage/obsolete-signer-marker"; /bin/chmod 000 "$STATE/stage/obsolete-signer-marker"
cutover old-signer-sudoers "$STATE/stage/obsolete-signer-marker" /etc/sudoers.d/factory-runner-signer
/usr/sbin/visudo -cf /etc/sudoers.d/factory-runner-broker
journal before-obsolete-removal old-signer-sudoers /etc/sudoers.d/factory-runner-signer
/bin/rm -f /etc/sudoers.d/factory-runner-signer
/usr/bin/python3 -I - <<'PY'
import os
fd=os.open('/etc/sudoers.d',os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
PY
journal obsolete-removed old-signer-sudoers /etc/sudoers.d/factory-runner-signer
"$0" verify
journal committed generation complete
trap - EXIT INT TERM HUP
/bin/rm -rf -- "$STATE"
/usr/bin/python3 -I - <<'PY'
import os
fd=os.open('/var/lib/factory-runner',os.O_RDONLY|os.O_DIRECTORY|os.O_CLOEXEC);os.fsync(fd);os.close(fd)
PY
echo 'factory runner v2 complete generation committed; external enrollment/evidence unchanged'
