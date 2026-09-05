#!/usr/bin/python3
"""Root-authority project gate; candidate supplies CMake product inputs only."""
import os, pathlib, subprocess, sys
root=pathlib.Path(os.environ.get("FACTORY_PRODUCT_ROOT", ""))
if not root.is_absolute() or not (root/"CMakeLists.txt").is_file(): raise SystemExit("product-gate: invalid product root")
build=pathlib.Path(os.environ.get("FACTORY_BUILD_ROOT", "")); install=build/"install"
if not build.is_absolute() or build == root or root in build.parents: raise SystemExit("product-gate: invalid separate writable build root")
env={k:v for k,v in os.environ.items() if k in {"HOME","PATH","LANG","LC_ALL","NIX_REMOTE","GIT_CONFIG_NOSYSTEM","GIT_CONFIG_GLOBAL","GIT_ATTR_NOSYSTEM"}}
commands=[
 ["cmake","-S",str(root),"-B",str(build),"-DCMAKE_BUILD_TYPE=Debug",f"-DCMAKE_INSTALL_PREFIX={install}"],
 ["cmake","--build",str(build),"--parallel"],
 ["ctest","--test-dir",str(build),"--output-on-failure"],
 ["cmake","--install",str(build)],
]
for command in commands:
 result=subprocess.run(command,cwd=root,env=env)
 if result.returncode: raise SystemExit(result.returncode)
print("root-authority-product-gate: PASS")
