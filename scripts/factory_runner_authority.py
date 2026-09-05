#!/usr/bin/env python3
"""Immutable root-owned probe-authority bundle loader.

Candidate archives contain only the product under test.  Contracts, probes,
analyzers, schemas, licensed oracle and executable pins are loaded from this
root-owned closure and are held by descriptor while a request is processed.
"""
from __future__ import annotations
import hashlib, json, os, re, stat
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

SHA256=re.compile(r"^[0-9a-f]{64}$")
NAME=re.compile(r"^[a-z0-9][a-z0-9._-]*$")
MAX_FILE=8*1024*1024
MAX_FILES=256
MAX_TOTAL=64*1024*1024

class AuthorityError(RuntimeError): pass

@dataclass
class AuthorityBundle:
    root: Path
    document: dict
    files: dict[str,tuple[int,bytes]]
    digest: str
    fixture: bool = False
    def close(self)->None:
        for fd,_ in self.files.values():
            try: os.close(fd)
            except OSError: pass
        self.files.clear()
    def bytes(self,relative:str)->bytes:
        try:return self.files[relative][1]
        except KeyError as exc: raise AuthorityError(f"authority file is not pinned: {relative}") from exc
    def path(self,relative:str)->Path:
        if relative not in self.files: raise AuthorityError(f"authority file is not pinned: {relative}")
        path=self.root/relative; fd,data=self.files[relative]; _chain(path,fixture=self.fixture)
        named=os.lstat(path); opened=os.fstat(fd)
        if (named.st_dev,named.st_ino)!=(opened.st_dev,opened.st_ino) or hashlib.sha256(data).hexdigest()!=self.document["files"][relative]:
            raise AuthorityError(f"authority file changed after enrollment: {relative}")
        return path
    def revalidate(self)->None:
        _chain(self.root,fixture=self.fixture)
        for relative in self.files: self.path(relative)
    def class_contract(self,name:str)->dict:
        value=self.document["classes"].get(name)
        if not isinstance(value,dict): raise AuthorityError(f"authority has no class {name}")
        return value

def _chain(path:Path, *, fixture:bool)->None:
    if not path.is_absolute(): raise AuthorityError("authority path must be absolute")
    expected={0,os.getuid()} if fixture else {0}
    current=Path(path.anchor)
    for part in path.parts[1:]:
        current/=part; info=os.lstat(current)
        if stat.S_ISLNK(info.st_mode) or info.st_uid not in expected:
            raise AuthorityError(f"authority component ownership/type is unsafe: {current}")
        if info.st_mode&0o022: raise AuthorityError(f"authority component is writable: {current}")

def _canonical(raw:bytes)->dict:
    try:data=json.loads(raw)
    except (UnicodeError,json.JSONDecodeError) as exc: raise AuthorityError("authority manifest is invalid JSON") from exc
    if not isinstance(data,dict) or set(data)!={"schema","version","files","classes","trusted_path","licensed_oracle"} or data.get("schema")!="factory-probe-authority/v1" or data.get("version")!=1:
        raise AuthorityError("authority manifest schema/fields are invalid")
    return data

def load_authority(root:Path, expected_digest:str, *, fixture:bool=False)->AuthorityBundle:
    if not root.is_absolute() or not SHA256.fullmatch(expected_digest): raise AuthorityError("authority root/digest is invalid")
    _chain(root,fixture=fixture)
    manifest=root/"authority.json"; _chain(manifest,fixture=fixture)
    mfd=os.open(manifest,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC); mi=os.fstat(mfd)
    try:
        if not stat.S_ISREG(mi.st_mode) or mi.st_nlink!=1 or mi.st_size>MAX_FILE: raise AuthorityError("authority manifest inode is unsafe")
        raw=os.read(mfd,mi.st_size+1)
        final=os.lstat(manifest)
        if (final.st_dev,final.st_ino)!=(mi.st_dev,mi.st_ino): raise AuthorityError("authority manifest changed while opening")
    finally: os.close(mfd)
    if hashlib.sha256(raw).hexdigest()!=expected_digest: raise AuthorityError("authority manifest digest mismatch")
    data=_canonical(raw); file_pins=data["files"]
    if not isinstance(file_pins,dict) or not file_pins or len(file_pins)>MAX_FILES: raise AuthorityError("authority file table is invalid")
    held={}; total=0
    try:
        for rel,digest in sorted(file_pins.items()):
            p=PurePosixPath(rel)
            if not isinstance(rel,str) or p.is_absolute() or not p.parts or any(x in ("",".","..") for x in p.parts) or not isinstance(digest,str) or not SHA256.fullmatch(digest): raise AuthorityError("authority file descriptor is invalid")
            path=root.joinpath(*p.parts); _chain(path,fixture=fixture)
            fd=os.open(path,os.O_RDONLY|os.O_NOFOLLOW|os.O_CLOEXEC); info=os.fstat(fd)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink!=1 or info.st_size>MAX_FILE: raise AuthorityError(f"authority file inode is unsafe: {rel}")
            total+=info.st_size
            if total>MAX_TOTAL: raise AuthorityError("authority closure exceeds aggregate bound")
            chunks=[]; remaining=info.st_size; h=hashlib.sha256()
            while remaining:
                chunk=os.read(fd,min(65536,remaining))
                if not chunk: raise AuthorityError(f"authority file shortened: {rel}")
                remaining-=len(chunk); h.update(chunk); chunks.append(chunk)
            if os.read(fd,1) or h.hexdigest()!=digest: raise AuthorityError(f"authority file digest mismatch: {rel}")
            held[rel]=(fd,b"".join(chunks))
        classes=data["classes"]
        if not isinstance(classes,dict) or not classes: raise AuthorityError("authority classes are invalid")
        for cname,entry in classes.items():
            if not NAME.fullmatch(cname) or not isinstance(entry,dict) or set(entry)!={"capabilities","gate"}: raise AuthorityError("authority class descriptor is invalid")
            caps=entry["capabilities"]
            if not isinstance(caps,dict): raise AuthorityError("authority capability map is invalid")
            for cap,contract in caps.items():
                if not NAME.fullmatch(cap) or not isinstance(contract,dict) or set(contract)!={"argv","artifacts","analyzer_argv"}: raise AuthorityError("authority capability descriptor is invalid")
                for key in ("argv","analyzer_argv"):
                    argv=contract[key]
                    if not isinstance(argv,list) or not argv or not all(isinstance(x,str) and x and "\0" not in x for x in argv): raise AuthorityError("authority argv is invalid")
                    first=argv[0]
                    if first.startswith("@/") and first[2:] not in held: raise AuthorityError("authority argv references an unpinned file")
            gate=entry["gate"]
            if not isinstance(gate,dict) or set(gate)!={"argv","artifacts","analyzer_argv"}: raise AuthorityError("authority gate is invalid")
        trusted=data["trusted_path"]
        if not isinstance(trusted,list) or not trusted or not all(isinstance(x,str) and x.startswith("/") for x in trusted): raise AuthorityError("authority trusted PATH is invalid")
        for directory in trusted:
            _chain(Path(directory), fixture=fixture)
            info=os.stat(directory)
            if not stat.S_ISDIR(info.st_mode) or info.st_mode&0o022:
                raise AuthorityError("authority trusted PATH root is mutable or not a directory")
        oracle=data["licensed_oracle"]
        if oracle is not None and (not isinstance(oracle,dict) or set(oracle)!={"path","sha256"} or oracle["path"] not in held or held[oracle["path"]][1] is None or hashlib.sha256(held[oracle["path"]][1]).hexdigest()!=oracle["sha256"]): raise AuthorityError("licensed oracle pin is invalid")
        return AuthorityBundle(root,data,held,expected_digest,fixture)
    except Exception:
        for fd,_ in held.values():
            try:os.close(fd)
            except OSError:pass
        raise
