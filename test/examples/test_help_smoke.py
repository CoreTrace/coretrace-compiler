from pathlib import Path
import shutil
import tempfile

from ctestfw.runner import CompilerRunner, RunnerConfig
from ctestfw.plan import CompilePlan
from ctestfw.framework.testcase import TestCase
from ctestfw.framework.reporter import ConsoleReporter
from ctestfw.assertions.compiler import (
    assert_exit_code,
    assert_stdout_contains,
)

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "test" / "examples" / "fixtures"
WORK = ROOT / "test" / "examples" / ".work"


def copy_fixtures(ws: Path, files: list[Path]) -> None:
    for f in files:
        dst = ws / f.name
        shutil.copy2(f, dst)


def main() -> int:
    cc_bin = (ROOT / "build" / "cc").resolve()
    runner = CompilerRunner(RunnerConfig(executable=cc_bin))
    if not cc_bin.exists():
        print(f"cc binary not found: {cc_bin}")
        return 1

    src = FIXTURES / "hello.c"
    if not src.exists():
        print(f"fixture not found: {src}")
        return 1

    tc_help = TestCase(
        name="help_smoke",
        plan=CompilePlan(
            name="help_smoke",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--help"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_stdout_contains("Usage:"),
            assert_stdout_contains("Core options:"),
            assert_stdout_contains("--instrument"),
            assert_stdout_contains("Exit codes:"),
        ],
    )

    WORK.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=f"{tc_help.name}_", dir=str(WORK)) as d:
        ws = Path(d)
        copy_fixtures(ws, [src])
        report = tc_help.run(runner, ws)

    rep = type("Tmp", (), {"name": "help_smoke", "reports": [report]})()
    return ConsoleReporter().render(rep)


if __name__ == "__main__":
    raise SystemExit(main())
