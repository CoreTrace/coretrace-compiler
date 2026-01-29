from __future__ import annotations
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence

from .result import RunResult, CompileResult
from .platform import detect_platform

@dataclass(frozen=True)
class RunnerConfig:
    executable: Path
    default_timeout_s: float = 20.0
    default_env: Optional[Dict[str, str]] = None

class CompilerRunner:
    def __init__(self, cfg: RunnerConfig) -> None:
        self._cfg = cfg

    def run(
        self,
        args: Sequence[str],
        cwd: Path,
        env: Optional[Dict[str, str]] = None,
        timeout_s: Optional[float] = None,
    ) -> RunResult:
        argv = [str(self._cfg.executable), *map(str, args)]
        merged_env = dict(self._cfg.default_env or {})
        if env:
            merged_env.update(env)

        p = subprocess.run(
            argv,
            cwd=str(cwd),
            env=merged_env if merged_env else None,
            capture_output=True,
            text=True,
            timeout=timeout_s or self._cfg.default_timeout_s,
        )
        return RunResult(
            argv=argv,
            cwd=cwd,
            exit_code=p.returncode,
            stdout=p.stdout or "",
            stderr=p.stderr or "",
            env=merged_env if merged_env else None,
        )

    def compile(
        self,
        args: Sequence[str],
        cwd: Path,
        output_path: Optional[Path] = None,
        env: Optional[Dict[str, str]] = None,
        timeout_s: Optional[float] = None,
    ) -> CompileResult:
        rr = self.run(args=args, cwd=cwd, env=env, timeout_s=timeout_s)
        return CompileResult(
            run=rr,
            output_path=output_path,
            platform=detect_platform()
        )