from __future__ import annotations
from dataclasses import dataclass
from enum import Enum, auto
from pathlib import Path

class ArtifactKind(Enum):
    ELF = auto()
    MACHO = auto()
    LLVM_IR_TEXT = auto()
    LLVM_BITCODE = auto()
    UNKNOWN = auto()

@dataclass(frozen=True)
class ArtifactInfo:
    path: Path
    kind: ArtifactKind

def _read_prefix(p: Path, n: int = 64) -> bytes:
    with p.open("rb") as f:
        return f.read(n)

def detect_artifact_kind(p: Path) -> ArtifactInfo:
    data = _read_prefix(p, 64)

    # ELF: 0x7F 'E' 'L' 'F'
    if len(data) >= 4 and data[0:4] == b"\x7fELF":
        return ArtifactInfo(p, ArtifactKind.ELF)

    # Mach-O (several magics):
    # 0xFEEDFACE, 0xCEFAEDFE, 0xFEEDFACF, 0xCFFAEDFE, 0xCAFEBABE (fat)
    if len(data) >= 4:
        magic = int.from_bytes(data[0:4], byteorder="big", signed=False)
        magic_le = int.from_bytes(data[0:4], byteorder="little", signed=False)
        macho_magics = {
            0xFEEDFACE, 0xFEEDFACF, 0xCAFEBABE,
            0xCEFAEDFE, 0xCFFAEDFE, 0xBEBAFECA,  # includes swapped/fat
        }
        if magic in macho_magics or magic_le in macho_magics:
            return ArtifactInfo(p, ArtifactKind.MACHO)

    # LLVM bitcode often starts with 'BC' 0xC0 0xDE
    if len(data) >= 4 and data[0:4] == b"BC\xc0\xde":
        return ArtifactInfo(p, ArtifactKind.LLVM_BITCODE)

    # LLVM IR text is plain text; common first tokens:
    # ; ModuleID = ...
    # source_filename = ...
    # target triple = ...
    try:
        txt = data.decode("utf-8", errors="ignore")
        head = txt.lstrip()
        if head.startswith("; ModuleID =") or head.startswith("source_filename") or "target triple" in head[:120]:
            return ArtifactInfo(p, ArtifactKind.LLVM_IR_TEXT)
    except Exception:
        pass

    return ArtifactInfo(p, ArtifactKind.UNKNOWN)