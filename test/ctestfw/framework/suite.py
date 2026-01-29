from __future__ import annotations
from dataclasses import dataclass, field
from pathlib import Path
import tempfile
from typing import List

from .testcase import TestCase, TestReport
from ..runner import CompilerRunner

@dataclass
class TestSuite:
    name: str
    cases: List[TestCase] = field(default_factory=list)

    def run(self, runner: CompilerRunner, root_workspace: Path) -> "SuiteReport":
        root_workspace.mkdir(parents=True, exist_ok=True)
        reports: List[TestReport] = []
        for tc in self.cases:
            with tempfile.TemporaryDirectory(prefix=f"{tc.name}_", dir=str(root_workspace)) as d:
                ws = Path(d)
                # Caller copies sources into ws if needed (see example)
                reports.append(tc.run(runner, ws))
        return SuiteReport(self.name, reports)

@dataclass(frozen=True)
class SuiteReport:
    name: str
    reports: List[TestReport]

    def ok(self) -> bool:
        return all(r.ok for r in self.reports)