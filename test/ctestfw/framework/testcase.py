from __future__ import annotations
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

from ..plan import CompilePlan
from ..runner import CompilerRunner
from ..result import CompileResult
from ..assertions.core import Assertion, AssertError

@dataclass
class TestCase:
    name: str
    plan: CompilePlan
    assertions: List[Assertion] = field(default_factory=list)

    def run(self, runner: CompilerRunner, workspace: Path) -> "TestReport":
        # workspace is per-test temp dir
        out_path = (workspace / self.plan.out) if self.plan.out else None
        argv = self.plan.argv()
        if self.plan.out:
            # ensure "-o workspace/out" rather than relative confusion
            # Here we patch argv: replace the -o argument with the workspace path
            # Simple strategy: rebuild properly.
            argv = []
            argv.extend([str(workspace / s) if not str(s).startswith("/") else str(s) for s in self.plan.sources])
            argv.extend(["-o", str(out_path)])
            argv.extend(list(self.plan.extra_args))

        res = runner.compile(args=argv, cwd=workspace, output_path=out_path)

        errors: List[str] = []
        for a in self.assertions:
            try:
                a.check(res)
            except AssertError as e:
                errors.append(f"[{a.name}] {e}")

        return TestReport(
            name=self.name,
            ok=(len(errors) == 0),
            errors=errors,
            result=res,
        )

@dataclass(frozen=True)
class TestReport:
    name: str
    ok: bool
    errors: List[str]
    result: CompileResult