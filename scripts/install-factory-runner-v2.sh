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
 [[ -f $STATE/targets ]] || { echo 'no recoverable factory-runner transaction' >&2; exit 1; }
 /usr/bin/tac "$STATE/targets" | while IFS=$'\t' read -r name target; do
   [[ -n $name && -n $target ]] || continue
   if [[ -e $STATE/backup/$name || -L $STATE/backup/$name ]]; then
     /bin/rm -rf -- "$target.new-generation"; /bin/mv -T -- "$target" "$target.new-generation" 2>/dev/null || true
     /bin/mv -T -- "$STATE/backup/$name" "$target"
   elif [[ -f $STATE/missing/$name ]]; then /bin/rm -rf -- "$target"; fi
   /usr/bin/python3 - "$target" <<'PY'
import os,sys
p=os.path.dirname(sys.argv[1]) or '/'; fd=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(fd);os.close(fd)
PY
 done
 /bin/rm -rf -- "$STATE"
 echo 'factory runner transaction rolled back'; exit 0
fi
if [[ $ACTION == verify ]]; then
 /usr/bin/python3 - <<'PY'
import grp,hashlib,json,os,pathlib,pwd,stat
m={'devrunner':'dev-runner-vm','iprunner':'iprunner','gpurunner':'gpurunner'}
p=pathlib.Path('/etc/factory-runner/runner-policy.json');d=json.loads(p.read_bytes());transport=json.loads(pathlib.Path('/etc/factory-runner/transport-manifest.json').read_bytes())
assert {x['name'] for x in d['classes']}==set(m.values()) and set(transport['classes'])==set(m)
for c in d['classes']:
 a=pwd.getpwuid(c['uid']);assert m[a.pw_name]==c['name']
 groups={grp.getgrgid(g).gr_name for g in os.getgrouplist(a.pw_name,a.pw_gid)};assert groups==set(c['approved_groups'])
 ak=pathlib.Path(a.pw_dir)/'.ssh/authorized_keys';s=ak.stat();lines=ak.read_text().splitlines()
 assert s.st_uid==c['uid'] and s.st_gid==a.pw_gid and stat.S_IMODE(s.st_mode)==0o600 and len(lines)==1
 prefix='command="/usr/local/libexec/factory-runner-server",restrict,no-agent-forwarding,no-port-forwarding,no-pty,no-user-rc,no-X11-forwarding '
 assert lines[0].startswith(prefix) and hashlib.sha256((lines[0][len(prefix):]+'\n').encode()).hexdigest()==transport['classes'][a.pw_name]['sha256']
 principal=pathlib.Path('/etc/factory-runner/principals')/a.pw_name;ps=principal.stat()
 assert ps.st_uid==0 and stat.S_IMODE(ps.st_mode)==0o600 and principal.read_text()==transport['classes'][a.pw_name]['principal']+'\n'
for p in ('/opt/factory-runner/authority/v1','/usr/local/libexec/factory-runner-v2.bundle','/etc/sudoers.d/factory-runner-broker'):
 assert pathlib.Path(p).exists() and not pathlib.Path(p).is_symlink()
print('factory runner installed generation verified')
PY
 exit
fi
[[ $ACTION == install ]] || usage
SOURCE=''; MANIFEST=''; COMMIT=''; TREE=''; POLICY=''; TRANSPORT=''; LAUNCHER=''
declare -A KEYS=()
while (($#)); do
 case $1 in
  --source-root) SOURCE=$2;shift 2;; --install-manifest) MANIFEST=$2;shift 2;; --commit) COMMIT=$2;shift 2;; --tree) TREE=$2;shift 2;; --policy) POLICY=$2;shift 2;; --transport-manifest) TRANSPORT=$2;shift 2;;
  --ssh-launcher-manifest) LAUNCHER=$2;shift 2;; --key) [[ $2 == *=* ]] || usage; KEYS[${2%%=*}]=${2#*=};shift 2;; *) usage;; esac
done
[[ -n $SOURCE && -n $MANIFEST && -n $COMMIT && -n $TREE && -n $POLICY && -n $TRANSPORT && -n $LAUNCHER ]] || usage
[[ ${#KEYS[@]} -eq 3 && -n ${KEYS[devrunner]:-} && -n ${KEYS[iprunner]:-} && -n ${KEYS[gpurunner]:-} ]] || usage
[[ ! -e $STATE ]] || { echo 'unfinished transaction exists; run rollback first' >&2; exit 1; }
/bin/mkdir -p /var/lib/factory-runner; /bin/chmod 0700 /var/lib/factory-runner
/bin/mkdir -m 0700 "$STATE" "$STATE/snapshot" "$STATE/stage" "$STATE/backup" "$STATE/missing"
rollback_signal(){ rc=$?; trap - EXIT INT TERM HUP; "$0" rollback || true; exit "$rc"; }
trap rollback_signal EXIT INT TERM HUP
# Snapshot/validate all source and external authority through no-follow held FDs.
/usr/bin/python3 - "$SOURCE" "$MANIFEST" "$COMMIT" "$TREE" "$POLICY" "$TRANSPORT" "$LAUNCHER" "${KEYS[devrunner]}" "${KEYS[iprunner]}" "${KEYS[gpurunner]}" "$STATE" <<'PY'
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
if set(m)!={'schema','commit','tree','commit_object_b64','files','executables','host_requirements'} or m['schema']!='factory-runner-install-manifest/v1':die('installation manifest fields/schema invalid')
if m['commit']!=commit or m['tree']!=tree or not re.fullmatch('[0-9a-f]{40}',commit) or not re.fullmatch('[0-9a-f]{40}',tree):die('explicit commit/tree binding mismatch')
try:co=base64.b64decode(m['commit_object_b64'],validate=True)
except Exception:die('commit object encoding invalid')
if hashlib.sha1(b'commit '+str(len(co)).encode()+b'\0'+co).hexdigest()!=commit or not co.startswith(f'tree {tree}\n'.encode()):die('commit object/tree binding invalid')
files=m['files']
required={'scripts/factory-runner-broker.py','scripts/factory-runner-signer.py','scripts/factory-runner-server.py','scripts/factory_runner_policy.py','scripts/factory_runner_artifacts.py','scripts/factory_runner_authority.py','deploy/factory-runner-authority-v1/authority.json'}
if not isinstance(files,dict) or not required.issubset(files):die('installation closure is incomplete')
snap=state/'snapshot'/'source';snap.mkdir()
for rel,desc in sorted(files.items()):
 if not isinstance(rel,str) or pathlib.PurePosixPath(rel).is_absolute() or '..' in pathlib.PurePosixPath(rel).parts or set(desc)!={'sha256','mode'}:die('invalid source manifest entry')
 p=source/rel;before=os.lstat(p)
 if stat.S_ISLNK(before.st_mode) or not stat.S_ISREG(before.st_mode):die(f'source path is not regular/no-follow: {rel}')
 fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC);opened=os.fstat(fd);data=b''
 while True:
  b=os.read(fd,65536)
  if not b:break
  data+=b
 after=os.lstat(p);os.close(fd)
 if (before.st_dev,before.st_ino)!=(opened.st_dev,opened.st_ino) or (after.st_dev,after.st_ino,after.st_size)!=(opened.st_dev,opened.st_ino,opened.st_size):die(f'mutable source race: {rel}')
 if hashlib.sha256(data).hexdigest()!=desc['sha256'] or stat.S_IMODE(opened.st_mode)!=desc['mode']:die(f'source digest/mode mismatch: {rel}')
 out=snap/rel;out.parent.mkdir(parents=True,exist_ok=True);out.write_bytes(data);out.chmod(desc['mode'])
# Exact set: the trusted manifest, not a later checkout walk, defines the closure.
(state/'snapshot'/'install-manifest.json').write_bytes(raw_manifest)
policy_raw=protected(pp);transport_raw=protected(tp);launcher_raw=protected(lp);(state/'snapshot'/'policy.json').write_bytes(policy_raw);(state/'snapshot'/'transport.json').write_bytes(transport_raw);(state/'snapshot'/'ssh-launcher.json').write_bytes(launcher_raw)
for a,p in keypaths.items():(state/'snapshot'/f'{a}.pub').write_bytes(protected(p))
# From here onward import only immutable root-owned snapshot bytes.
sys.path.insert(0,str(snap/'scripts'));import factory_runner_policy as fp
fp.DEFAULT_POLICY_PATH=state/'snapshot'/'policy.json';policy=fp.load_policy()
if any(c['probe_authority_status']!='enrolled' for c in policy['classes']):die('probe authority is not independently enrolled')
account_map={'devrunner':'dev-runner-vm','iprunner':'iprunner','gpurunner':'gpurunner'}
if {c['name'] for c in policy['classes']}!=set(account_map.values()):die('policy class set is not exact')
# Executable pins are external authority, including the SSH launcher and broker dependencies.
needed={'/usr/bin/xdg-dbus-proxy','/usr/bin/systemd-run','/usr/bin/systemctl','/usr/bin/sudo','/usr/bin/git','/usr/bin/python3','/usr/bin/bash','/usr/bin/ssh-keygen','/usr/bin/busctl','/usr/sbin/visudo'}
if not needed.issubset(m['executables']):die('executable pin closure incomplete')
for name,d in m['executables'].items():
 p=pathlib.Path(name);data=protected(p);i=os.stat(p)
 if set(d)!={'sha256','dev','ino'} or hashlib.sha256(data).hexdigest()!=d['sha256'] or [i.st_dev,i.st_ino]!=[d['dev'],d['ino']] or not i.st_mode&0o111:die(f'executable pin mismatch: {name}')
try:launcher=json.loads(launcher_raw)
except Exception:die('SSH launcher manifest invalid JSON')
if set(launcher)!={'schema','path','sha256','dev','ino'} or launcher['schema']!='factory-ssh-launcher/v1' or launcher['path'] not in m['executables'] or m['executables'][launcher['path']]!={k:launcher[k] for k in ('sha256','dev','ino')}:die('trusted SSH launcher is not exactly executable-pinned')
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
 if not cg.startswith('/') or values.get('PrivatePIDs')!='yes' or values.get('PrivateMounts')!='yes' or values.get('ProtectSystem')!='strict' or values.get('ProtectHome')!='yes' or values.get('DevicePolicy')!='closed' or values.get('NoNewPrivileges')!='yes' or values.get('KillMode')!='control-group':die('systemd did not apply exact containment properties')
 cgp=pathlib.Path('/sys/fs/cgroup'+cg)
 for n in ('cgroup.procs','cgroup.events'):
  if not (cgp/n).is_file():die('transient unit cgroup cleanup state unreadable')
finally:
 subprocess.run(['/usr/bin/systemctl','stop',unit],check=False);subprocess.run(['/usr/bin/systemctl','reset-failed',unit],check=False)
for _ in range(50):
 if not cgp.exists():break
 import time;time.sleep(.1)
else:die('transient probe cgroup was not cleaned up')
# Account/class/group and class-specific device/tool/library prerequisites.
requirements=m['host_requirements']
if not isinstance(requirements,dict) or set(requirements)!=set(account_map.values()):die('host requirement class set is not exact')
for klass,rq in requirements.items():
 if set(rq)!={'required_paths','allowed_absent_until_evidence'} or rq['allowed_absent_until_evidence']!=[] or not isinstance(rq['required_paths'],list):die(f'host requirements for {klass} are malformed or attempt an undeclared deferral')
 for raw in rq['required_paths']:
  p=pathlib.Path(raw)
  if not p.is_absolute() or not p.exists() or p.is_symlink():die(f'required {klass} host resource absent/unsafe: {p}')
for c in policy['classes']:
 a=pwd.getpwuid(c['uid'])
 if account_map.get(a.pw_name)!=c['name']:die('numeric UID/account/class mapping mismatch')
 groups={grp.getgrgid(x).gr_name for x in os.getgrouplist(a.pw_name,a.pw_gid)}
 if groups!=set(c['approved_groups']) or grp.getgrgid(a.pw_gid).gr_name not in groups:die('primary/supplementary group set mismatch')
 for p in (pathlib.Path(c['workspace_root']),pathlib.Path(c['nonce_ledger']).parent):
  if not p.exists():die(f'required policy resource absent: {p}')
# Real InputPlumber and GPU prerequisites are mandatory when their classes are enrolled.
if any('inputplumber-system-dbus' in c['allowed_capabilities'] for c in policy['classes']):
 if subprocess.run(['/usr/bin/systemctl','is-active','--quiet','inputplumber.service']).returncode:die('real InputPlumber service is not active')
 if not pathlib.Path('/run/dbus/system_bus_socket').is_socket() or not pathlib.Path('/dev/uinput').exists():die('InputPlumber system bus/uinput prerequisites absent')
 expect=json.loads((snap/'deploy/factory-runner-authority-v1/iprunner-probes/inputplumber-expectations.json').read_bytes())
 version=subprocess.check_output(['/usr/bin/busctl','--system','get-property','org.shadowblip.InputPlumber','/org/shadowblip/InputPlumber/Manager','org.shadowblip.InputManager','Version'],text=True).strip().split(maxsplit=1)[-1].strip('"')
 if version!=expect['manager']['version']:die('active InputPlumber version differs from authority')
if any(c['name']=='gpurunner' for c in policy['classes']):
 if not pathlib.Path('/dev/dri/renderD128').exists():die('GPU DRM render node absent')
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
for n in ('factory-runner-broker.py','factory-runner-signer.py','factory-runner-server.py','factory_runner_policy.py','factory_runner_artifacts.py','factory_runner_authority.py'):
 shutil.copyfile(snap/'scripts'/n,lib/n);(lib/n).chmod(0o700 if '-' in n and n!='factory-runner-server.py' else 0o755)
(stage/'policy').write_bytes(policy_raw);(stage/'policy').chmod(0o640)
(stage/'transport').write_bytes(transport_raw);(stage/'transport').chmod(0o600)
(stage/'ssh-launcher').write_bytes(launcher_raw);(stage/'ssh-launcher').chmod(0o600)
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
/bin/mkdir -p /opt/factory-runner/authority /usr/local/libexec /etc/factory-runner /etc/sudoers.d /etc/factory-runner/principals /etc/controller-box
# Stage compatibility links before any cutover.
for name in factory-runner-broker factory-runner-signer factory-runner-server; do /bin/ln -s "factory-runner-v2.bundle/$name.py" "$STATE/stage/$name"; done
cutover(){
 local name=$1 staged=$2 target=$3
 /bin/mkdir -p -- "$(/usr/bin/dirname -- "$target")"
 printf '%s\t%s\n' "$name" "$target" >> "$STATE/targets"; /usr/bin/python3 - "$STATE/targets" <<'PY'
import os,sys
f=os.open(sys.argv[1],os.O_RDONLY);os.fsync(f);os.close(f);d=os.open(os.path.dirname(sys.argv[1]),os.O_RDONLY|os.O_DIRECTORY);os.fsync(d);os.close(d)
PY
 if [[ -e $target || -L $target ]]; then
  /usr/bin/python3 - "$staged" "$target" <<'PY'
import ctypes,os,sys
s,d=sys.argv[1:];libc=ctypes.CDLL(None,use_errno=True)
if libc.renameat2(-100,s.encode(),-100,d.encode(),2):raise OSError(ctypes.get_errno(),'RENAME_EXCHANGE')
for p in (os.path.dirname(s),os.path.dirname(d)):
 f=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(f);os.close(f)
PY
  /bin/mv -T -- "$staged" "$STATE/backup/$name"
 else
  /usr/bin/touch "$STATE/missing/$name"
  /usr/bin/python3 - "$staged" "$target" <<'PY'
import ctypes,os,sys
s,d=sys.argv[1:];libc=ctypes.CDLL(None,use_errno=True)
if libc.renameat2(-100,s.encode(),-100,d.encode(),1):raise OSError(ctypes.get_errno(),'RENAME_NOREPLACE')
for p in (os.path.dirname(s),os.path.dirname(d)):
 f=os.open(p,os.O_RDONLY|os.O_DIRECTORY);os.fsync(f);os.close(f)
PY
 fi
}
cutover authority "$STATE/stage/authority" /opt/factory-runner/authority/v1
cutover libexec "$STATE/stage/libexec" /usr/local/libexec/factory-runner-v2.bundle
cutover policy "$STATE/stage/policy" /etc/factory-runner/runner-policy.json
cutover transport "$STATE/stage/transport" /etc/factory-runner/transport-manifest.json
cutover ssh-launcher "$STATE/stage/ssh-launcher" /etc/controller-box/factory-ssh-launcher.json
cutover broker-sudoers "$STATE/stage/sudoers" /etc/sudoers.d/factory-runner-broker
for name in factory-runner-broker factory-runner-signer factory-runner-server; do cutover "$name" "$STATE/stage/$name" "/usr/local/libexec/$name"; done
for account in devrunner iprunner gpurunner; do
 home=$(/usr/bin/getent passwd "$account" | /usr/bin/cut -d: -f6); /bin/mkdir -p "$home/.ssh"; /bin/chmod 0700 "$home/.ssh"
 cutover "authorized_keys-$account" "$STATE/stage/authorized_keys.$account" "$home/.ssh/authorized_keys"
 cutover "principal-$account" "$STATE/stage/principal.$account" "/etc/factory-runner/principals/$account"
done
# Obsolete signer sudoers participates in rollback rather than being deleted.
if [[ -e /etc/sudoers.d/factory-runner-signer ]]; then /bin/mv -T /etc/sudoers.d/factory-runner-signer "$STATE/backup/old-signer-sudoers"; printf '%s\t%s\n' old-signer-sudoers /etc/sudoers.d/factory-runner-signer >> "$STATE/targets"; else /usr/bin/touch "$STATE/missing/old-signer-sudoers"; fi
/usr/sbin/visudo -cf /etc/sudoers.d/factory-runner-broker
"$0" verify
trap - EXIT INT TERM HUP
/bin/rm -rf -- "$STATE"
echo 'factory runner v2 complete generation committed; external enrollment/evidence unchanged'
