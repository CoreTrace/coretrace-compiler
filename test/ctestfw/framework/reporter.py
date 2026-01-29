from __future__ import annotations
from dataclasses import dataclass
from typing import Iterable
from .suite import SuiteReport

@dataclass(frozen=True)
class ConsoleReporter:
    def render(self, rep: SuiteReport) -> int:
        print(f"== Suite: {rep.name} ==")
        failed = 0
        for r in rep.reports:
            status = "PASS" if r.ok else "FAIL"
            print(f"- {status} {r.name}")
            if not r.ok:
                failed += 1
                for e in r.errors:
                    print(f"    {e}")
        print(f"== Result: {len(rep.reports)-failed}/{len(rep.reports)} passed ==")
        return 0 if failed == 0 else 1