#!/usr/bin/python3
"""Root-authority dev capability semantics; no generic gate substitution."""
import hashlib,json,os,pathlib,subprocess,sys
if len(sys.argv)!=2: raise SystemExit("usage: probe-dev-capability.py CAPABILITY")
cap=sys.argv[1]
tests={
 "remote-project-gate":["test_interaction_inventory|test_docs_sync"],
 "systemd-user":["test_service_install|test_user_unit"],
 "kernel-uinput":["test_kernel_controller"],
 "installed-package":["test_packaging_install|test_installed_smoke"],
}
if cap not in tests: raise SystemExit("unapproved dev capability")
root=pathlib.Path(os.environ["FACTORY_PRODUCT_ROOT"]);build=pathlib.Path(os.environ["FACTORY_BUILD_ROOT"])
env={k:v for k,v in os.environ.items() if k in {"HOME","PATH","LANG","LC_ALL","NIX_REMOTE","GIT_CONFIG_NOSYSTEM","GIT_CONFIG_GLOBAL","GIT_ATTR_NOSYSTEM"}}
commands=[["cmake","-S",str(root),"-B",str(build),"-DCMAKE_BUILD_TYPE=Debug"],["cmake","--build",str(build),"--parallel"],["ctest","--test-dir",str(build),"-R",tests[cap][0],"--output-on-failure"]]
for command in commands:
 r=subprocess.run(command,env=env,cwd="/")
 if r.returncode: raise SystemExit(r.returncode)
out=pathlib.Path(os.environ["FACTORY_RUNNER_ARTIFACT_DIR"]);out.mkdir(mode=0o700,parents=True,exist_ok=True)
marker={"schema":"factory-capability-semantics/v1","capability":cap,"must_execute":True,"executed":True,"must_not_skip":True,"skipped":False,"deny_simulated":True,"simulated":False,"command_sha256":hashlib.sha256(json.dumps(commands,separators=(",",":" )).encode()).hexdigest()}
(out/"authority-result.json").write_text(json.dumps(marker,sort_keys=True)+"\n");(out/"authority-result.json").chmod(0o600)
print(f"root-authority-{cap}: EXECUTED NONSKIP NONSIMULATED PASS")
