# SPDX-License-Identifier: Apache-2.0
from __future__ import annotations
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

from ctestfw.runner import CompilerRunner, RunnerConfig
from ctestfw.plan import CompilePlan
from ctestfw.framework.testcase import TestCase
from ctestfw.framework.reporter import ConsoleReporter
from ctestfw.assertions.core import Assertion, require
from ctestfw.assertions.compiler import (
    assert_exit_code,
    assert_output_name,
    assert_output_exists,
    assert_native_binary_kind,
)

ROOT = Path(__file__).resolve().parents[2]
EXTERN = ROOT / "extern-project"
FIXTURES = EXTERN / "tests"
WORK = EXTERN / ".work"
BUILD = EXTERN / ".build-python"


def _platform_executable(path: Path) -> Path:
    if os.name == "nt" and path.suffix.lower() != ".exe":
        return path.with_suffix(".exe")
    return path


def _powershell_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _read_artifact_bytes(res, path: str) -> bytes:
    artifact = Path(path)
    if not artifact.is_absolute():
        artifact = res.run.cwd / artifact
    require(artifact.exists(), f"output does not exist: {artifact}")
    return artifact.read_bytes()


def _is_windows_native_artifact(data: bytes) -> bool:
    if data.startswith(b"MZ"):
        return True
    if len(data) < 2:
        return False
    return data[:2] in {b"\x64\x86", b"\x4c\x01", b"\x64\xaa"}


def assert_windows_native_artifact(path: str) -> Assertion:
    def _check(res) -> None:
        data = _read_artifact_bytes(res, path)
        require(
            _is_windows_native_artifact(data),
            f"expected PE/COFF artifact at {path}, got unrecognized header",
        )
    return Assertion(name=f"windows_native_artifact_{Path(path).name}", check=_check)


def run_cmd(argv: list[str], cwd: Path) -> tuple[int, str, str]:
    try:
        p = subprocess.run(
            argv,
            cwd=str(cwd),
            capture_output=True,
            text=True,
        )
        return p.returncode, p.stdout or "", p.stderr or ""
    except FileNotFoundError:
        return 127, "", f"command not found: {argv[0]}"


def find_windows_dev_shell() -> str | None:
    program_files_x86 = os.environ.get("ProgramFiles(x86)")
    if not program_files_x86:
        return None

    vswhere = Path(program_files_x86) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if not vswhere.exists():
        return None

    rc, out, _ = run_cmd([
        str(vswhere),
        "-latest",
        "-products", "*",
        "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationPath",
    ], ROOT)
    if rc != 0 or not out.strip():
        return None

    dev_shell = Path(out.strip()) / "Common7" / "Tools" / "Launch-VsDevShell.ps1"
    if not dev_shell.exists():
        return None
    return str(dev_shell)


def run_cmd_in_vsdev(argv: list[str], cwd: Path) -> tuple[int, str, str]:
    dev_shell = find_windows_dev_shell()
    if dev_shell is None:
        return 127, "", "Visual Studio developer shell not found"

    quoted_args = ", ".join(_powershell_quote(arg) for arg in argv)
    script = (
        "$ErrorActionPreference = 'Stop'; "
        f". {_powershell_quote(dev_shell)} -Arch amd64 -HostArch amd64 | Out-Null; "
        f"$cmd = @({quoted_args}); "
        "& $cmd[0] @($cmd[1..($cmd.Length - 1)])"
    )
    return run_cmd(
        ["powershell", "-ExecutionPolicy", "Bypass", "-Command", script],
        cwd,
    )


def read_cache_var(cache_path: Path, key: str) -> str | None:
    if not cache_path.exists():
        return None
    prefix = f"{key}:"
    try:
        for line in cache_path.read_text().splitlines():
            if line.startswith(prefix) and "=" in line:
                return line.split("=", 1)[1].strip()
    except OSError:
        return None
    return None


def detect_llvm_clang_dirs() -> tuple[str | None, str | None]:
    llvm_dir = os.environ.get("LLVM_DIR")
    clang_dir = os.environ.get("Clang_DIR")

    # Try to reuse the main build configuration if present.
    if not llvm_dir or not clang_dir:
        for cache in (ROOT / "build" / "CMakeCache.txt", ROOT / "build-win" / "CMakeCache.txt"):
            llvm_dir = llvm_dir or read_cache_var(cache, "LLVM_DIR")
            clang_dir = clang_dir or read_cache_var(cache, "Clang_DIR")
            if llvm_dir and clang_dir:
                break

    # Try llvm-config if still missing.
    if not llvm_dir:
        rc, out, _ = run_cmd(["llvm-config", "--cmakedir"], ROOT)
        if rc == 0 and out.strip():
            llvm_dir = out.strip()

    # Derive clang dir from llvm dir if possible.
    if llvm_dir and not clang_dir:
        llvm_path = Path(llvm_dir)
        if llvm_path.name == "llvm":
            candidate = llvm_path.parent / "clang"
            if candidate.exists():
                clang_dir = str(candidate)

    # Brew fallback (macOS) if still missing.
    if not llvm_dir or not clang_dir:
        for formula in ("llvm@20", "llvm@19", "llvm"):
            rc, out, _ = run_cmd(["brew", "--prefix", formula], ROOT)
            if rc == 0 and out.strip():
                prefix = Path(out.strip())
                llvm_candidate = prefix / "lib" / "cmake" / "llvm"
                clang_candidate = prefix / "lib" / "cmake" / "clang"
                if not llvm_dir and llvm_candidate.exists():
                    llvm_dir = str(llvm_candidate)
                if not clang_dir and clang_candidate.exists():
                    clang_dir = str(clang_candidate)
            if llvm_dir and clang_dir:
                break

    return llvm_dir, clang_dir


def configure_and_build() -> Path:
    if os.name == "nt" and BUILD.exists():
        shutil.rmtree(BUILD, ignore_errors=True)

    if BUILD.exists():
        cache = BUILD / "CMakeCache.txt"
        cached_src = read_cache_var(cache, "CMAKE_HOME_DIRECTORY")
        if cached_src and Path(cached_src).resolve() != EXTERN.resolve():
            shutil.rmtree(BUILD, ignore_errors=True)

    cmake_args = [
        "cmake",
        "-S", str(EXTERN),
        "-B", str(BUILD),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DFETCHCONTENT_SOURCE_DIR_CC={ROOT}",
    ]
    llvm_dir, clang_dir = detect_llvm_clang_dirs()
    runner = run_cmd
    if os.name == "nt":
        llvm_root = Path(llvm_dir).parents[2] if llvm_dir else None
        clang_cl = llvm_root / "bin" / "clang-cl.exe" if llvm_root else None
        cmake_args.extend([
            "-G", "Ninja Multi-Config",
        ])
        if clang_cl and clang_cl.exists():
            cmake_args.append(f"-DCMAKE_C_COMPILER={clang_cl}")
            cmake_args.append(f"-DCMAKE_CXX_COMPILER={clang_cl}")
        runner = run_cmd_in_vsdev
    if llvm_dir:
        cmake_args.append(f"-DLLVM_DIR={llvm_dir}")
    if clang_dir:
        cmake_args.append(f"-DClang_DIR={clang_dir}")
    logger_source_dir = os.environ.get("CORETRACE_LOGGER_SOURCE_DIR")
    if logger_source_dir:
        cmake_args.append(f"-DFETCHCONTENT_SOURCE_DIR_CORETRACE_LOGGER={Path(logger_source_dir).resolve()}")
    if not llvm_dir or not clang_dir:
        print("LLVM/Clang CMake dirs not found. Set LLVM_DIR and Clang_DIR, or build the main project first.")

    rc, out, err = runner(cmake_args, ROOT)
    if rc != 0:
        print("cmake configure failed")
        print(out)
        print(err)
        raise SystemExit(1)

    build_args = ["cmake", "--build", str(BUILD), "--config", "Release"]
    if os.name != "nt":
        build_args.append("-j")
    rc, out, err = runner(build_args, ROOT)
    if rc != 0:
        print("cmake build failed")
        print(out)
        print(err)
        raise SystemExit(1)

    cc1 = _platform_executable(BUILD / "cc1")
    if not cc1.exists():
        cc1 = _platform_executable(BUILD / "Release" / "cc1")
    if not cc1.exists():
        cc1 = _platform_executable(BUILD / "Debug" / "cc1")
    if not cc1.exists():
        print(f"cc1 binary not found after build: {cc1}")
        raise SystemExit(1)
    return cc1.resolve()


def copy_fixtures(ws: Path, files: list[Path]) -> None:
    for f in files:
        dst = ws / f.name
        shutil.copy2(f, dst)


def main() -> int:
    cc1 = configure_and_build()
    runner = CompilerRunner(RunnerConfig(executable=cc1))

    src_c = FIXTURES / "hello.c"
    src_cpp = FIXTURES / "hello.cpp"
    for src in (src_c, src_cpp):
        if not src.exists():
            print(f"fixture not found: {src}")
            return 1

    tc_c = TestCase(
        name="extern_project_compile_c",
        plan=CompilePlan(
            name="extern_project_compile_c",
            sources=[Path("hello.c")],
            out=Path("hello_ext_c.o"),
            extra_args=["-c"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_name("hello_ext_c.o"),
            assert_output_exists(),
            assert_windows_native_artifact("hello_ext_c.o") if os.name == "nt" else assert_native_binary_kind(),
        ],
    )

    tc_cpp = TestCase(
        name="extern_project_compile_cpp",
        plan=CompilePlan(
            name="extern_project_compile_cpp",
            sources=[Path("hello.cpp")],
            out=Path("hello_ext_cpp.o"),
            extra_args=["-c"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_name("hello_ext_cpp.o"),
            assert_output_exists(),
            assert_windows_native_artifact("hello_ext_cpp.o") if os.name == "nt" else assert_native_binary_kind(),
        ],
    )

    WORK.mkdir(parents=True, exist_ok=True)
    reports = []
    for tc in (tc_c, tc_cpp):
        with tempfile.TemporaryDirectory(prefix=f"{tc.name}_", dir=str(WORK)) as d:
            ws = Path(d)
            copy_fixtures(ws, [src_c, src_cpp])
            reports.append(tc.run(runner, ws))

    rep = type("Tmp", (), {"name": "extern_project", "reports": reports})()
    return ConsoleReporter().render(rep)


if __name__ == "__main__":
    raise SystemExit(main())
