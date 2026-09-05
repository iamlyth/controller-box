#!/usr/bin/env python3
"""Adversarial unit coverage for retained runner artifact framing/inodes."""
import base64, hashlib, os, pathlib, tempfile
import sys
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import factory_runner_artifacts as a

def rejected(fn):
    try: fn()
    except (a.ArtifactError,OSError): return
    raise AssertionError('hostile artifact accepted')

def desc(path='cap/a.json', data=b'x'):
    return {'path':path,'capability':'cap','media_type':'application/json','type':'file','mode':0o600,'size':len(data),'sha256':hashlib.sha256(data).hexdigest()}

for bad in ('../x','/x','cap/../x','cap//x','Cap/x','cap/X'):
    rejected(lambda bad=bad:a.canonical_path(bad))
rejected(lambda:a.validate_descriptors([desc(),desc()],['cap']))
rejected(lambda:a.validate_descriptors([desc('cap/a'),desc('cap/A')],['cap']))
rejected(lambda:a.validate_descriptors([desc() for _ in range(a.MAX_ARTIFACTS+1)],['cap']))
large=desc(); large['size']=a.MAX_ARTIFACT_FILE+1
rejected(lambda:a.validate_descriptors([large],['cap']))
rejected(lambda:a.decode_payload([], [desc()]))
rejected(lambda:a.decode_payload([{'path':'cap/a.json','data_b64':'eA'}],[desc()]))
rejected(lambda:a.decode_payload([{'path':'cap/a.json','data_b64':base64.b64encode(b'y').decode()}],[desc()]))
with tempfile.TemporaryDirectory() as td:
    root=pathlib.Path(td); root.chmod(0o700); cap=root/'cap'; cap.mkdir(mode=0o700)
    req={'cap':{'required':['a.json'],'files':{'a.json':'application/json'}}}
    (cap/'a.json').write_bytes(b'x'); (cap/'a.json').chmod(0o600)
    descriptors,payload=a.collect(root,['cap'],req)
    assert a.decode_payload(payload,descriptors)[0][1]==b'x'
    (cap/'a.json').unlink(); os.symlink('/etc/passwd',cap/'a.json')
    rejected(lambda:a.collect(root,['cap'],req)); (cap/'a.json').unlink()
    source=cap/'source'; source.write_bytes(b'x'); os.link(source,cap/'a.json')
    rejected(lambda:a.collect(root,['cap'],req)); (cap/'a.json').unlink(); source.unlink()
    os.mkfifo(cap/'a.json'); rejected(lambda:a.collect(root,['cap'],req)); (cap/'a.json').unlink()
    (cap/'extra').write_bytes(b'x'); rejected(lambda:a.collect(root,['cap'],req))
print('test: retained runner artifact adversarial checks passed')
