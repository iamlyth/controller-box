#!/usr/bin/env python3
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1];CHECK=ROOT/'scripts/check-workspace-credentials.py'
with tempfile.TemporaryDirectory() as td:
 root=pathlib.Path(td);(root/'.gitignore').write_text('.ollama-usage-env\n');
 assert subprocess.run([str(CHECK),'--root',str(root)],capture_output=True).returncode==0
 secret=root/'.ollama-usage-env';secret.write_text('secret');
 result=subprocess.run([str(CHECK),'--root',str(root)],capture_output=True,text=True)
 assert result.returncode!=0 and '.ollama-usage-env' in result.stderr
 secret.unlink();nested=root/'.factory-state';nested.mkdir();(nested/'ollama-cookie-secret').write_text('secret')
 assert subprocess.run([str(CHECK),'--root',str(root)],capture_output=True).returncode!=0
print('test: ignored workspace credential leakage rejected')
