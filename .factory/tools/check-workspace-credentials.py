#!/usr/bin/env python3
"""Reject credential-bearing files anywhere below a workspace, including ignored."""
import argparse,os,pathlib,re,stat,sys
p=argparse.ArgumentParser();p.add_argument('--root',default='.');a=p.parse_args();root=pathlib.Path(a.root).resolve()
for current,dirs,files in os.walk(root,topdown=True,followlinks=False):
 dirs[:]=[d for d in dirs if d!='.git']
 for name in files:
  folded=name.casefold()
  if folded in {'update-ollama-cookies.sh','ollama-usage-guard.sh'}:continue
  if (folded in {'.ollama-usage-env','ollama-cookies','ollama-credentials','.env.credentials'}
      or re.fullmatch(r'.*ollama.*(?:cookie|credential|secret).*',folded)):
   print(f'workspace-credentials: forbidden credential file: {pathlib.Path(current,name).relative_to(root)}',file=sys.stderr);raise SystemExit(1)
print('workspace-credentials: clean')
