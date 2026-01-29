from __future__ import annotations
from dataclasses import dataclass
from typing import Callable, Optional

from ..result import CompileResult

class AssertError(AssertionError):
    pass

@dataclass(frozen=True)
class Assertion:
    name: str
    check: Callable[[CompileResult], None]

def require(predicate: bool, msg: str) -> None:
    if not predicate:
        raise AssertError(msg)