from __future__ import annotations
from dataclasses import dataclass
from enum import Enum, auto
import platform

class OS(Enum):
    LINUX = auto()
    MACOS = auto()
    WINDOWS = auto()
    OTHER = auto()

class Arch(Enum):
    X86_64 = auto()
    ARM64 = auto()
    X86_32 = auto()
    ARM32 = auto()
    OTHER = auto()

@dataclass(frozen=True)
class PlatformInfo:
    os: OS
    arch: Arch
    machine: str
    system: str

def detect_platform() -> PlatformInfo:
    system = platform.system().lower()
    machine = platform.machine().lower()

    if system == "darwin":
        os_ = OS.MACOS
    elif system == "linux":
        os_ = OS.LINUX
    elif system == "windows":
        os_ = OS.WINDOWS
    else:
        os_ = OS.OTHER

    if machine in ("x86_64", "amd64"):
        arch = Arch.X86_64
    elif machine in ("arm64", "aarch64"):
        arch = Arch.ARM64
    elif machine in ("i386", "i686", "x86"):
        arch = Arch.X86_32
    elif machine.startswith("armv7") or machine == "arm":
        arch = Arch.ARM32
    else:
        arch = Arch.OTHER

    return PlatformInfo(os=os_, arch=arch, machine=machine, system=system)