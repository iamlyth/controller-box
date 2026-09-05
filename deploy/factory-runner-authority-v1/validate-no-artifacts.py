#!/usr/bin/python3
import argparse,pathlib
p=argparse.ArgumentParser();p.add_argument('--capability');p.add_argument('--artifacts');p.add_argument('--commit');p.add_argument('--tree');a=p.parse_args()
root=pathlib.Path(a.artifacts)
if root.exists() and any(root.rglob('*')): raise SystemExit('unexpected artifact bytes')
print('root-authority-no-artifacts: PASS')
