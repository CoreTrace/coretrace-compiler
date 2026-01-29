from __future__ import annotations
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence, Union

from .core import Assertion, require
from ..result import CompileResult
from ..platform import OS
from ..inspect.filetype import detect_artifact_kind, ArtifactKind

def assert_exit_code(expected: int = 0) -> Assertion:
    def _check(res: CompileResult) -> None:
        require(res.run.exit_code == expected,
                f"exit_code expected {expected}, got {res.run.exit_code}\n"
                f"stderr:\n{res.run.stderr}")
    return Assertion(name=f"exit_code_is_{expected}", check=_check)

def assert_argv_contains(subseq: Sequence[str]) -> Assertion:
    def _check(res: CompileResult) -> None:
        argv = list(res.run.argv)
        ssub = list(map(str, subseq))
        # simple containment check (order independent or dependent? -> here: dependent-ish)
        it = iter(argv)
        ok = True
        for needle in ssub:
            for x in it:
                if x == needle:
                    break
            else:
                ok = False
                break
        require(ok, f"argv does not contain subsequence {ssub}\nargv={argv}")
    return Assertion(name="argv_contains", check=_check)

def assert_output_name(expected_name: str) -> Assertion:
    def _check(res: CompileResult) -> None:
        require(res.output_path is not None, "no output_path provided to test")
        require(res.output_path.name == expected_name,
                f"output name expected '{expected_name}', got '{res.output_path.name}'")
    return Assertion(name="output_name", check=_check)

def assert_output_exists() -> Assertion:
    def _check(res: CompileResult) -> None:
        require(res.output_path is not None, "no output_path provided to test")
        require(res.output_path.exists(), f"output does not exist: {res.output_path}")
    return Assertion(name="output_exists", check=_check)

def assert_output_kind(expected: ArtifactKind) -> Assertion:
    def _check(res: CompileResult) -> None:
        require(res.output_path is not None, "no output_path provided to test")
        require(res.output_path.exists(), f"output does not exist: {res.output_path}")
        info = detect_artifact_kind(res.output_path)
        require(info.kind == expected,
                f"output kind expected {expected.name}, got {info.kind.name} ({info.path})")
    return Assertion(name=f"output_kind_{expected.name}", check=_check)

def _resolve_output_path(res: CompileResult, path: Union[str, Path]) -> Path:
    p = Path(path)
    if not p.is_absolute():
        p = res.run.cwd / p
    return p

def assert_output_exists_at(path: Union[str, Path]) -> Assertion:
    def _check(res: CompileResult) -> None:
        p = _resolve_output_path(res, path)
        require(p.exists(), f"output does not exist: {p}")
    return Assertion(name=f"output_exists_at_{Path(path).name}", check=_check)

def assert_output_kind_at(path: Union[str, Path], expected: ArtifactKind) -> Assertion:
    def _check(res: CompileResult) -> None:
        p = _resolve_output_path(res, path)
        require(p.exists(), f"output does not exist: {p}")
        info = detect_artifact_kind(p)
        require(info.kind == expected,
                f"output kind expected {expected.name}, got {info.kind.name} ({info.path})")
    return Assertion(name=f"output_kind_at_{expected.name}", check=_check)

def assert_output_nonempty_at(path: Union[str, Path]) -> Assertion:
    def _check(res: CompileResult) -> None:
        p = _resolve_output_path(res, path)
        require(p.exists(), f"output does not exist: {p}")
        require(p.stat().st_size > 0, f"output is empty: {p}")
    return Assertion(name=f"output_nonempty_at_{Path(path).name}", check=_check)

def assert_stdout_contains(text: str) -> Assertion:
    def _check(res: CompileResult) -> None:
        require(text in (res.run.stdout or ""),
                f"stdout does not contain '{text}'\nstdout:\n{res.run.stdout}\nstderr:\n{res.run.stderr}")
    return Assertion(name=f"stdout_contains_{text}", check=_check)

def assert_native_binary_kind() -> Assertion:
    def _check(res: CompileResult) -> None:
        require(res.output_path is not None, "no output_path provided to test")
        require(res.platform is not None, "platform info missing in CompileResult")
        require(res.output_path.exists(), f"output does not exist: {res.output_path}")

        expected = ArtifactKind.MACHO if res.platform.os == OS.MACOS else ArtifactKind.ELF
        info = detect_artifact_kind(res.output_path)

        require(
            info.kind == expected,
            f"native output kind expected {expected.name} on {res.platform.os.name}, got {info.kind.name} ({info.path})"
        )
    return Assertion(name="native_binary_kind", check=_check)

def assert_native_binary_kind_at(path: Union[str, Path]) -> Assertion:
    def _check(res: CompileResult) -> None:
        require(res.platform is not None, "platform info missing in CompileResult")
        p = _resolve_output_path(res, path)
        require(p.exists(), f"output does not exist: {p}")
        expected = ArtifactKind.MACHO if res.platform.os == OS.MACOS else ArtifactKind.ELF
        info = detect_artifact_kind(p)
        require(
            info.kind == expected,
            f"native output kind expected {expected.name} on {res.platform.os.name}, got {info.kind.name} ({info.path})"
        )
    return Assertion(name="native_binary_kind_at", check=_check)
