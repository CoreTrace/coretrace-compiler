from __future__ import annotations
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Sequence

@dataclass(frozen=True)
class CompilePlan:
    name: str
    sources: Sequence[Path]
    out: Optional[Path] = None
    extra_args: Sequence[str] = field(default_factory=list)

    def argv(self) -> List[str]:
        args: List[str] = []
        # Exemple: ton driver peut être "cc"
        # À adapter à ton CLI.
        args.extend([str(s) for s in self.sources])
        if self.out is not None:
            # Exemple: -o out
            args.extend(["-o", str(self.out)])
        args.extend(list(self.extra_args))
        return args