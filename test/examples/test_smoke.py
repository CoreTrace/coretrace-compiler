from pathlib import Path
import shutil

from ctestfw.runner import CompilerRunner, RunnerConfig
from ctestfw.plan import CompilePlan
from ctestfw.framework.testcase import TestCase
from ctestfw.framework.suite import TestSuite
from ctestfw.framework.reporter import ConsoleReporter
from ctestfw.assertions.compiler import (
    assert_exit_code,
    assert_argv_contains,
    assert_output_exists,
    assert_output_name,
    assert_output_kind,
    assert_native_binary_kind,
    assert_output_exists_at,
    assert_native_binary_kind_at,
    assert_output_kind_at,
    assert_output_nonempty_at,
    assert_stdout_contains
)
from ctestfw.inspect.filetype import ArtifactKind
from ctestfw.platform import detect_platform, OS

ROOT = Path(__file__).resolve().parents[2]
FIXTURES = ROOT / "test" / "examples" / "fixtures"
WORK = ROOT / "test" / "examples" / ".work"

def copy_fixtures(ws: Path, files: list[Path]) -> None:
    for f in files:
        src = f
        dst = ws / f.name
        shutil.copy2(src, dst)

def main() -> int:
    cc_bin = (ROOT / "build" / "cc").resolve()
    runner = CompilerRunner(RunnerConfig(executable=cc_bin))
    if not cc_bin.exists():
        print(f"cc binary not found: {cc_bin}")
        return 1

    # Fixtures (ex: hello.c)
    src = FIXTURES / "hello.c"
    debug_src = FIXTURES / "debug.c"
    cpp_src = FIXTURES / "hello.cpp"
    cpp_as_c_src = FIXTURES / "cpp_as_c.c"
    vtable_src = FIXTURES / "vtable.cpp"

    def base_out_assertions(out_name: str):
        return [
            assert_exit_code(0),
            assert_argv_contains(["-o"]),          # check args passed
            assert_output_name(out_name),          # check binary name respected
            assert_output_exists(),
        ]

    tc_macho = TestCase(
        name="compile_macho_hello",
        plan=CompilePlan(
            name="compile_macho_hello",
            sources=[Path("hello.c")],   # sera copié dans workspace
            out=Path("hello.out"),
            extra_args=[],
        ),
        assertions=base_out_assertions("hello.out") + [
            assert_output_kind(ArtifactKind.MACHO),
        ],
    )

    tc_elf = TestCase(
        name="compile_elf_hello",
        plan=CompilePlan(
            name="compile_elf_hello",
            sources=[Path("hello.c")],
            out=Path("hello.out"),
            extra_args=[],
        ),
        assertions=base_out_assertions("hello.out") + [
            assert_output_kind(ArtifactKind.ELF),
        ],
    )

    tc_native = TestCase(
        name="compile_native_hello",
        plan=CompilePlan(
            name="compile_native_hello",
            sources=[Path("hello.c")],
            out=Path("hello.out"),
            extra_args=[],
        ),
        assertions=base_out_assertions("hello.out") + [
            assert_native_binary_kind(),
        ],
    )

    tc_cpp = TestCase(
        name="compile_cpp_hello",
        plan=CompilePlan(
            name="compile_cpp_hello",
            sources=[Path("hello.cpp")],
            out=Path("hello_cpp.out"),
            extra_args=[],
        ),
        assertions=base_out_assertions("hello_cpp.out") + [
            assert_native_binary_kind(),
        ],
    )

    tc_o_eq = TestCase(
        name="compile_o_equals",
        plan=CompilePlan(
            name="compile_o_equals",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["-o=main"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-o=main"]),
            assert_output_exists_at("main"),
            assert_native_binary_kind_at("main"),
        ],
    )

    tc_d_space = TestCase(
        name="compile_define_space",
        plan=CompilePlan(
            name="compile_define_space",
            sources=[Path("debug.c")],
            out=None,
            extra_args=["-D", "DEBUG", "-o=debug_space"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-D", "DEBUG"]),
            assert_output_exists_at("debug_space"),
            assert_native_binary_kind_at("debug_space"),
        ],
    )

    tc_d_compact = TestCase(
        name="compile_define_compact",
        plan=CompilePlan(
            name="compile_define_compact",
            sources=[Path("debug.c")],
            out=None,
            extra_args=["-DDEBUG", "-o=debug_compact"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-DDEBUG"]),
            assert_output_exists_at("debug_compact"),
            assert_native_binary_kind_at("debug_compact"),
        ],
    )

    tc_x_cxx = TestCase(
        name="compile_x_cxx",
        plan=CompilePlan(
            name="compile_x_cxx",
            sources=[],
            out=None,
            extra_args=["-x=c++", "cpp_as_c.c", "-o=hello_xcxx.out"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-x=c++"]),
            assert_output_exists_at("hello_xcxx.out"),
            assert_native_binary_kind_at("hello_xcxx.out"),
        ],
    )

    tc_instrument_c = TestCase(
        name="compile_instrument_c",
        plan=CompilePlan(
            name="compile_instrument_c",
            sources=[Path("hello.c")],
            out=Path("hello_instr_c.out"),
            extra_args=["--instrument"],
        ),
        assertions=base_out_assertions("hello_instr_c.out") + [
            assert_argv_contains(["--instrument"]),
            assert_native_binary_kind(),
        ],
    )

    tc_instrument_cpp = TestCase(
        name="compile_instrument_cpp",
        plan=CompilePlan(
            name="compile_instrument_cpp",
            sources=[Path("hello.cpp")],
            out=Path("hello_instr_cpp.out"),
            extra_args=["--instrument"],
        ),
        assertions=base_out_assertions("hello_instr_cpp.out") + [
            assert_argv_contains(["--instrument"]),
            assert_native_binary_kind(),
        ],
    )

    tc_instrument_x_cxx = TestCase(
        name="compile_instrument_x_cxx",
        plan=CompilePlan(
            name="compile_instrument_x_cxx",
            sources=[],
            out=None,
            extra_args=["--instrument", "-x=c++", "cpp_as_c.c", "-o=hello_instr_xcxx.out"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "-x=c++"]),
            assert_output_exists_at("hello_instr_xcxx.out"),
            assert_native_binary_kind_at("hello_instr_xcxx.out"),
        ],
    )

    tc_instrument_emit_llvm = TestCase(
        name="compile_instrument_emit_llvm",
        plan=CompilePlan(
            name="compile_instrument_emit_llvm",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--instrument", "-S", "-emit-llvm", "-o=hello_instr.ll"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "-S", "-emit-llvm"]),
            assert_output_exists_at("hello_instr.ll"),
            assert_output_kind_at("hello_instr.ll", ArtifactKind.LLVM_IR_TEXT),
            assert_output_nonempty_at("hello_instr.ll"),
        ],
    )

    tc_instrument_emit_bc = TestCase(
        name="compile_instrument_emit_bc",
        plan=CompilePlan(
            name="compile_instrument_emit_bc",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--instrument", "-c", "-emit-llvm", "-o=hello_instr.bc"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "-c", "-emit-llvm"]),
            assert_output_exists_at("hello_instr.bc"),
            assert_output_nonempty_at("hello_instr.bc"),
        ],
    )

    tc_readme_emit_llvm = TestCase(
        name="readme_emit_llvm",
        plan=CompilePlan(
            name="readme_emit_llvm",
            sources=[Path("hello.cpp")],
            out=None,
            extra_args=["-S", "-emit-llvm"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-S", "-emit-llvm"]),
            assert_output_kind_at("hello.ll", ArtifactKind.LLVM_IR_TEXT),
        ],
    )

    tc_readme_asm = TestCase(
        name="readme_asm",
        plan=CompilePlan(
            name="readme_asm",
            sources=[Path("hello.cpp")],
            out=None,
            extra_args=["-S"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-S"]),
            assert_output_nonempty_at("hello.s"),
        ],
    )

    tc_readme_c_obj = TestCase(
        name="readme_c_obj",
        plan=CompilePlan(
            name="readme_c_obj",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["-c"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-c"]),
            assert_native_binary_kind_at("hello.o"),
        ],
    )

    tc_readme_c_obj_o2 = TestCase(
        name="readme_c_obj_o2",
        plan=CompilePlan(
            name="readme_c_obj_o2",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["-c", "-O2"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["-c", "-O2"]),
            assert_native_binary_kind_at("hello.o"),
        ],
    )

    tc_readme_instrument = TestCase(
        name="readme_instrument",
        plan=CompilePlan(
            name="readme_instrument",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--instrument", "-o", "app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "-o", "app"]),
            assert_output_exists_at("app"),
            assert_native_binary_kind_at("app"),
        ],
    )

    tc_readme_shadow = TestCase(
        name="readme_shadow",
        plan=CompilePlan(
            name="readme_shadow",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--instrument", "--ct-shadow", "-o", "app_shadow"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "--ct-shadow"]),
            assert_output_exists_at("app_shadow"),
            assert_native_binary_kind_at("app_shadow"),
        ],
    )

    tc_readme_shadow_aggr = TestCase(
        name="readme_shadow_aggr",
        plan=CompilePlan(
            name="readme_shadow_aggr",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--instrument", "--ct-shadow-aggressive", "--ct-bounds-no-abort", "-o", "app_shadow_aggr"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "--ct-shadow-aggressive", "--ct-bounds-no-abort"]),
            assert_output_exists_at("app_shadow_aggr"),
            assert_native_binary_kind_at("app_shadow_aggr"),
        ],
    )

    tc_readme_vtable = TestCase(
        name="readme_vtable",
        plan=CompilePlan(
            name="readme_vtable",
            sources=[Path("vtable.cpp")],
            out=None,
            extra_args=["--instrument", "--ct-modules=vtable", "--ct-vcall-trace", "-o", "app_vtable"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "--ct-modules=vtable", "--ct-vcall-trace"]),
            assert_output_exists_at("app_vtable"),
            assert_native_binary_kind_at("app_vtable"),
        ],
    )

    tc_readme_inmem = TestCase(
        name="readme_inmem",
        plan=CompilePlan(
            name="readme_inmem",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--in-mem", "-S", "-emit-llvm"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--in-mem", "-S", "-emit-llvm"]),
            assert_stdout_contains("target triple"),
        ],
    )

    platform = detect_platform()
    common_cases = [tc_o_eq, tc_d_space, tc_d_compact, tc_cpp, tc_x_cxx]
    instrument_cases = [
        tc_instrument_c,
        tc_instrument_cpp,
        tc_instrument_x_cxx,
        tc_instrument_emit_llvm,
        tc_instrument_emit_bc,
    ]
    readme_cases = [
        tc_readme_emit_llvm,
        tc_readme_asm,
        tc_readme_c_obj,
        tc_readme_c_obj_o2,
        tc_readme_instrument,
        tc_readme_shadow,
        tc_readme_shadow_aggr,
        tc_readme_vtable,
        tc_readme_inmem,
    ]
    if platform.os == OS.MACOS:
        cases = [tc_macho, *common_cases, *instrument_cases, *readme_cases]
    elif platform.os == OS.LINUX:
        cases = [tc_elf, *common_cases, *instrument_cases, *readme_cases]
    else:
        cases = [tc_native, *common_cases]

    suite = TestSuite(name="compiler_smoke", cases=cases)

    reports = []
    WORK.mkdir(parents=True, exist_ok=True)
    for case in suite.cases:
        import tempfile
        with tempfile.TemporaryDirectory(prefix=f"{case.name}_", dir=str(WORK)) as d:
            ws = Path(d)
            copy_fixtures(ws, [src, debug_src, cpp_src, cpp_as_c_src, vtable_src])
            reports.append(case.run(runner, ws))

    rep = type("Tmp", (), {"name": suite.name, "reports": reports})()
    return ConsoleReporter().render(rep)

if __name__ == "__main__":
    raise SystemExit(main())
