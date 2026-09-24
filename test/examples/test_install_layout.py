# SPDX-License-Identifier: Apache-2.0
"""Installs the build into a temporary prefix, moves the prefix, and checks that the
installed ``cc`` links the instrumentation runtime from the moved prefix rather than
from the build tree."""
from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "test" / "examples" / "fixtures"


def resolve_build_dir() -> tuple[Path, str | None] | None:
    """Build tree of the compiler under test and, for a multi-config generator (Ninja
    Multi-Config, Visual Studio), the configuration it was built in: those place the
    compiler in ``<build>/<Config>/`` and install only with ``--config``."""
    env_cc = os.environ.get("CORETRACE_COMPILER_TEST_CC")
    if env_cc:
        cc_dir = Path(env_cc).resolve().parent
        if (cc_dir / "CMakeCache.txt").exists():
            return cc_dir, None
        if (cc_dir.parent / "CMakeCache.txt").exists():
            return cc_dir.parent, cc_dir.name
        return None
    candidate = ROOT / "build"
    return (candidate, None) if (candidate / "cc").exists() else None


def printed_by_verbose_driver(text: str) -> str:
    # clang -v quotes arguments containing a backslash and doubles the backslash,
    # so Windows paths appear escaped in the verbose link line.
    return text.replace("\\\\", "\\")


def run(cmd: list[str], cwd: Path, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, timeout=600)


def check(cond: bool, message: str, details: str = "") -> bool:
    print(("- PASS " if cond else "- FAIL ") + message)
    if not cond and details:
        print(details)
    return cond


def main() -> int:
    resolved = resolve_build_dir()
    if resolved is None:
        print("build directory not found; build first or set CORETRACE_COMPILER_TEST_CC")
        return 1
    build_dir, config = resolved

    ok = True
    with tempfile.TemporaryDirectory(prefix="ct_install_") as tmp:
        tmp_path = Path(tmp)
        prefix = tmp_path / "prefix"
        install = ["cmake", "--install", str(build_dir), "--prefix", str(prefix)]
        if config:
            install += ["--config", config]
        res = run(install, ROOT)
        ok &= check(res.returncode == 0, "cmake --install succeeds", res.stdout + res.stderr)

        exe = "cc.exe" if os.name == "nt" else "cc"
        runtime_lib = "ct_instrument_runtime.lib" if os.name == "nt" else "libct_instrument_runtime.a"
        logger_lib = "coretrace_logger.lib" if os.name == "nt" else "libcoretrace_logger.a"
        for rel in (Path("bin") / exe, Path("lib") / runtime_lib, Path("lib") / logger_lib):
            ok &= check((prefix / rel).exists(), f"installed prefix contains {rel}")
        if not ok:
            return 1

        # Relocate the prefix: nothing in the installed binary may point at the
        # original install location or at the build tree.
        moved = tmp_path / "moved"
        shutil.move(str(prefix), str(moved))
        cc = moved / "bin" / exe
        work = tmp_path / "work"
        work.mkdir()
        shutil.copy2(FIXTURES / "hello.c", work / "hello.c")

        env = dict(os.environ)
        env.pop("CT_RUNTIME_LIB_DIR", None)
        res = run([str(cc), "--instrument", "-v", "-o", "app", "hello.c"], work, env)
        ok &= check(res.returncode == 0, "installed cc compiles with --instrument", res.stderr)
        # Matched by its tail: only the relocated prefix has a moved/lib directory, and
        # the tail does not depend on how the temporary directory is spelled (Windows
        # runners give an 8.3 short name such as RUNNER~1, which cc may report in its
        # long form).
        expected = str(Path("moved") / "lib" / runtime_lib)
        ok &= check(expected in printed_by_verbose_driver(res.stderr),
                    "link line uses the runtime archive from the moved prefix",
                    f"expected '{expected}' in verbose output:\n{res.stderr[-3000:]}")
        # The driver writes -o verbatim, without adding .exe on Windows.
        app = work / "app"
        res = run([str(app)], work, env)
        ok &= check(res.returncode == 0 and "hello" in res.stdout,
                    "instrumented program runs from the moved prefix", res.stderr)

        # The environment override takes precedence over the executable-relative lookup.
        override = tmp_path / "override"
        shutil.copytree(moved / "lib", override)
        env["CT_RUNTIME_LIB_DIR"] = str(override)
        res = run([str(cc), "--instrument", "-v", "-o", "app2", "hello.c"], work, env)
        ok &= check(res.returncode == 0
                    and str(override / runtime_lib) in printed_by_verbose_driver(res.stderr),
                    "CT_RUNTIME_LIB_DIR overrides the executable-relative runtime lookup",
                    res.stderr[-3000:])

    print("== Result: " + ("PASS" if ok else "FAIL") + " ==")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
