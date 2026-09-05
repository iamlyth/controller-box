#!/usr/bin/env python3
"""Fixture-only regression checks for deployment/provenance trust boundaries."""
import pathlib,re
ROOT=pathlib.Path(__file__).resolve().parents[1]
installer=(ROOT/'scripts/install-factory-runner-v2.sh').read_text()
for marker in ('factory-runner-install-manifest/v2','commit_object_b64','mutable source race',
 'os.O_NOFOLLOW','RENAME_NOREPLACE','os.fsync','install-transaction','source Merkle digest mismatch',
 'factory-runner-transport/v1','PrivatePIDs=yes',
 'cgroup.procs','cgroup.events','InputPlumber version differs','/dev/dri/renderD128',
 "name='authorized_keys-'+account",'for account in devrunner iprunner gpurunner',
 'old-signer-sudoers','ssh-launcher','verify|rollback','secure_ssh_cutover'):
 assert marker in installer,marker
assert "account_map={'devrunner':'dev-runner-vm','iprunner':'iprunner','gpurunner':'gpurunner'}" in installer
assert "key_raw!=key+'\\n'" in installer and 'transport key fingerprint mismatch' in installer
assert 'RENAME_EXCHANGE' not in installer
assert 'import factory_runner_policy' not in installer
bootstrap=(ROOT/'scripts/factory-runner-root-bootstrap').read_text()
for marker in ('ssh-keygen -Y verify','archive is not the exact signed tree','FACTORY_RUNNER_AUTHENTICATED_BOOTSTRAP=1'):
 assert marker in bootstrap,marker
generator=(ROOT/'scripts/generate-runner-install-manifest.py').read_text()
for marker in ('--untracked-files=all','ls-tree','cat-file','gitlink/submodule','source_merkle_sha256'):
 assert marker in generator,marker
assert 'factory-runner-signer"' not in (ROOT/'deploy/factory-runner-authority-v1/forced-command-v2.txt').read_text()
assert all('BUNDLE_PATH = "/usr/local/libexec/factory-runner-v2.bundle"' in (ROOT/'scripts'/n).read_text() for n in ('factory-runner-broker.py','factory-runner-signer.py','factory-runner-server.py'))
broker=(ROOT/'scripts/factory-runner-broker.py').read_text()
for marker in ('GetNameOwner','unique_owner','starttime','os.O_NOFOLLOW','os.pread(self.fd',
 'InputPlumber D-Bus owner restarted or changed during routing','provenance_identity.verify()'):
 assert marker in broker,marker
policy=(ROOT/'scripts/factory_runner_policy.py').read_text()
assert 'REQUIRED_CLASSES = {"dev-runner-vm", "iprunner", "gpurunner"}' in policy
# Bare v3 parsers are permitted only behind canonical aggregate validation or
# in issuance/transfer components; acceptance consumers must mention v4/strong validation.
allowed={'factory-runner-broker.py','factory-runner-signer.py','run-factory-runners.py','check-factory-runner-evidence.py'}
for path in (ROOT/'scripts').glob('*.py'):
 text=path.read_text()
 if 'factory-runner-receipt/v3' not in text or path.name in allowed:continue
 assert ('factory-runner-aggregate/v4' in text or 'strong_runner_evidence' in text or
         'check-factory-runner-evidence.py' in text),f'weak receipt-v3 consumer: {path.name}'
print('test: installer transaction, transport, provenance, and consumer boundaries passed')
