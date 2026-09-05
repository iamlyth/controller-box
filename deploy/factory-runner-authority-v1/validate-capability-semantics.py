#!/usr/bin/python3
"""Validate mandatory dev capability markers from held broker bytes."""
import argparse,json,pathlib
p=argparse.ArgumentParser();p.add_argument('--capability',required=True);p.add_argument('--artifacts',required=True);p.add_argument('--commit',required=True);p.add_argument('--tree',required=True);a=p.parse_args()
path=pathlib.Path(a.artifacts)/a.capability/'authority-result.json'
if path.is_symlink() or not path.is_file():raise SystemExit('capability-authority: held result absent')
d=json.loads(path.read_bytes())
expected={"schema","capability","must_execute","executed","must_not_skip","skipped","deny_simulated","simulated","command_sha256"}
if set(d)!=expected or d['schema']!='factory-capability-semantics/v1' or d['capability']!=a.capability or d['must_execute'] is not True or d['executed'] is not True or d['must_not_skip'] is not True or d['skipped'] is not False or d['deny_simulated'] is not True or d['simulated'] is not False:
 raise SystemExit('capability-authority: execution/skip/simulation contract rejected')
print('root-authority-capability-semantics: PASS')
