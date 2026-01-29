from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
from typing import Optional, Sequence, Dict

from .platform import PlatformInfo

@dataclass(frozen=True)
class RunResult:
    argv: Sequence[str]
    cwd: Path
    exit_code: int
    stdout: str
    stderr: str
    env: Optional[Dict[str, str]] = None

    def ok(self) -> bool:
        return self.exit_code == 0

@dataclass(frozen=True)
class CompileResult:
    run: RunResult
    output_path: Optional[Path] = None  # main artifact, if any
    platform: Optional[PlatformInfo] = None