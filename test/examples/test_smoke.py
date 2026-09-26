# SPDX-License-Identifier: Apache-2.0
import os
from pathlib import Path
import shutil

from ctestfw.runner import CompilerRunner, RunnerConfig
from ctestfw.plan import CompilePlan
from ctestfw.framework.testcase import TestCase
from ctestfw.framework.suite import TestSuite
from ctestfw.framework.reporter import ConsoleReporter
from ctestfw.assertions.core import Assertion, require
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

def assert_file_contains(path: str, text: str) -> Assertion:
    def _check(res) -> None:
        p = Path(path)
        if not p.is_absolute():
            p = res.run.cwd / p
        require(p.exists(), f"output does not exist: {p}")
        data = p.read_text(encoding="utf-8", errors="ignore")
        require(text in data, f"file does not contain '{text}': {p}")
    return Assertion(name=f"file_contains_{Path(path).name}", check=_check)

def assert_stderr_contains(text: str) -> Assertion:
    def _check(res) -> None:
        require(text in (res.run.stderr or ""),
                f"stderr does not contain '{text}'\nstderr:\n{res.run.stderr}")
    return Assertion(name=f"stderr_contains_{text}", check=_check)

def assert_run_artifact(path: str, expected_exit: int | None, stderr_contains: str | list[str],
                        env: dict[str, str] | None = None) -> Assertion:
    """Run the artifact produced by the compile step and check its exit code and stderr.

    expected_exit None requires a failing status: a fatal signal or exception is reported
    as a platform-specific code. env adds variables to the artifact's environment."""
    expected_texts = [stderr_contains] if isinstance(stderr_contains, str) else stderr_contains
    def _check(res) -> None:
        import subprocess
        artifact = Path(path)
        if not artifact.is_absolute():
            artifact = res.run.cwd / artifact
        require(artifact.exists(), f"output does not exist: {artifact}")
        proc = subprocess.run([str(artifact)], cwd=res.run.cwd, env={**os.environ, **(env or {})},
                              capture_output=True, text=True, timeout=60)
        if expected_exit is None:
            require(proc.returncode != 0,
                    f"expected a failing exit status, got 0\nstderr:\n{proc.stderr}")
        else:
            require(proc.returncode == expected_exit,
                    f"expected exit {expected_exit}, got {proc.returncode}\nstderr:\n{proc.stderr}")
        for text in expected_texts:
            require(text in proc.stderr, f"stderr does not contain '{text}'\nstderr:\n{proc.stderr}")
    return Assertion(name=f"run_artifact_{Path(path).name}", check=_check)

def assert_stdout_matches(pattern: str) -> Assertion:
    def _check(res) -> None:
        import re
        out = res.run.stdout or ""
        require(re.search(pattern, out) is not None,
                f"stdout does not match /{pattern}/\nstdout:\n{out}")
    return Assertion(name=f"stdout_matches", check=_check)

def assert_stdout_count(text: str, count: int) -> Assertion:
    def _check(res) -> None:
        out = res.run.stdout or ""
        found = out.count(text)
        require(found == count,
                f"stdout contains '{text}' {found} times, expected {count}\nstdout:\n{out}")
    return Assertion(name="stdout_count", check=_check)

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

def assert_windows_native_artifact_at(path: str) -> Assertion:
    def _check(res) -> None:
        data = _read_artifact_bytes(res, path)
        require(
            _is_windows_native_artifact(data),
            f"expected PE/COFF artifact at {path}, got unrecognized header",
        )
    return Assertion(name=f"windows_native_artifact_{Path(path).name}", check=_check)

def native_artifact_assert_at(path: str, platform_os: OS) -> Assertion:
    if platform_os == OS.WINDOWS:
        return assert_windows_native_artifact_at(path)
    return assert_native_binary_kind_at(path)

def resolve_compiler_binary() -> Path | None:
    candidates: list[Path] = []

    env_override = os.environ.get("CORETRACE_COMPILER_TEST_CC")
    if env_override:
        candidates.append(Path(env_override))

    candidates.extend([
        ROOT / "dist" / "windows" / "bin" / "cc.exe",
        ROOT / "build" / "cc",
        ROOT / "build" / "Release" / "cc.exe",
        ROOT / "build-win" / "cc.exe",
        ROOT / "build-win" / "Release" / "cc.exe",
    ])

    for candidate in candidates:
        if candidate.exists():
            return candidate.resolve()

    return None

def main() -> int:
    platform = detect_platform()
    cc_bin = resolve_compiler_binary()
    if cc_bin is None:
        print("cc binary not found. Tried:")
        for candidate in [
            os.environ.get("CORETRACE_COMPILER_TEST_CC", ""),
            str(ROOT / "dist" / "windows" / "bin" / "cc.exe"),
            str(ROOT / "build" / "cc"),
            str(ROOT / "build" / "Release" / "cc.exe"),
            str(ROOT / "build-win" / "cc.exe"),
            str(ROOT / "build-win" / "Release" / "cc.exe"),
        ]:
            if candidate:
                print(f"  - {candidate}")
        return 1

    runner = CompilerRunner(RunnerConfig(executable=cc_bin))

    # Fixtures (ex: hello.c)
    src = FIXTURES / "hello.c"
    debug_src = FIXTURES / "debug.c"
    cpp_src = FIXTURES / "hello.cpp"
    cpp_as_c_src = FIXTURES / "cpp_as_c.c"
    vtable_src = FIXTURES / "vtable.cpp"
    leak_src = FIXTURES / "leak.c"
    overflow_src = FIXTURES / "overflow.c"
    broken_src = FIXTURES / "broken.c"
    codegen_error_src = FIXTURES / "codegen_error.c"
    undefined_ref_src = FIXTURES / "undefined_ref.c"
    alloc_site_src = FIXTURES / "alloc_site.c"
    new_delete_src = FIXTURES / "new_delete.cpp"
    crash_src = FIXTURES / "crash.c"
    trace_threads_src = FIXTURES / "trace_threads.cpp"
    trace_objc_src = FIXTURES / "trace_objc.m"
    leak_objc_src = FIXTURES / "leak_objc.m"
    new_delete_objc_src = FIXTURES / "new_delete_objc.mm"
    objc_alloc_forms_src = FIXTURES / "objc_alloc_forms.m"
    objc_objects_src = FIXTURES / "objc_objects.m"
    objcxx_objects_src = FIXTURES / "objc_objects.mm"
    objc_autofree_scan_src = FIXTURES / "objc_autofree_scan.m"
    stack_overflow_src = FIXTURES / "stack_overflow.c"
    stack_objects_src = FIXTURES / "stack_objects.c"

    def base_out_assertions(out_name: str):
        assertions = [
            assert_exit_code(0),
            assert_argv_contains(["-o"]),          # check args passed
            assert_output_name(out_name),          # check binary name respected
            assert_output_exists(),
        ]
        if platform.os == OS.WINDOWS:
            assertions.append(assert_windows_native_artifact_at(out_name))
        return assertions

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
            assert_windows_native_artifact_at("hello.out") if platform.os == OS.WINDOWS else assert_native_binary_kind(),
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
            assert_windows_native_artifact_at("hello_cpp.out") if platform.os == OS.WINDOWS else assert_native_binary_kind(),
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
            native_artifact_assert_at("main", platform.os),
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
            native_artifact_assert_at("debug_space", platform.os),
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
            native_artifact_assert_at("debug_compact", platform.os),
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
            native_artifact_assert_at("hello_xcxx.out", platform.os),
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
            assert_windows_native_artifact_at("hello_instr_c.out") if platform.os == OS.WINDOWS else assert_native_binary_kind(),
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
            assert_windows_native_artifact_at("hello_instr_cpp.out") if platform.os == OS.WINDOWS else assert_native_binary_kind(),
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
            native_artifact_assert_at("hello_instr_xcxx.out", platform.os),
        ],
    )

    tc_instrument_o_eq_trailing = TestCase(
        name="compile_instrument_o_equals_trailing",
        plan=CompilePlan(
            name="compile_instrument_o_equals_trailing",
            sources=[],
            out=None,
            extra_args=["--instrument", "-c", "hello.c", "-o=hello_instr_trail.o"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "-c", "-o=hello_instr_trail.o"]),
            assert_output_exists_at("hello_instr_trail.o"),
            native_artifact_assert_at("hello_instr_trail.o", platform.os),
        ],
    )

    # -g0 must not be mistaken for a debug request: instrumentation still needs
    # line tables to name allocation sites (malloc is on line 5 of the fixture).
    tc_instrument_g0_inmem = TestCase(
        name="compile_instrument_g0_inmem",
        plan=CompilePlan(
            name="compile_instrument_g0_inmem",
            sources=[Path("alloc_site.c")],
            out=None,
            extra_args=["--instrument", "-g0", "--in-mem", "-S", "-emit-llvm"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--instrument", "-g0", "--in-mem", "-S", "-emit-llvm"]),
            assert_stdout_contains("__ct_malloc"),
            # Clang leaves columns out of CodeView (Windows) debug info by default, so
            # sites are file:line there and file:line:column elsewhere.
            assert_stdout_contains("alloc_site.c:5" if platform.os == OS.WINDOWS
                                   else "alloc_site.c:5:"),
        ],
    )
    # The Windows C++ ABI mangles operator new/delete differently; cross-compiling to
    # IR checks on every host that the alloc pass still rewrites them.
    tc_instrument_cpp_microsoft_abi = TestCase(
        name="compile_instrument_cpp_microsoft_abi",
        plan=CompilePlan(
            name="compile_instrument_cpp_microsoft_abi",
            sources=[Path("new_delete.cpp")],
            out=None,
            extra_args=["--target=x86_64-pc-windows-msvc", "--instrument", "--ct-modules=alloc",
                        "--in-mem", "-S", "-emit-llvm"],
        ),
        assertions=[
            assert_exit_code(0),
            # Calls, not the declarations the pass adds to every module.
            assert_stdout_contains("call ptr @__ct_new("),
            assert_stdout_contains("call ptr @__ct_new_array("),
            assert_stdout_contains("call void @__ct_delete("),
            assert_stdout_contains("call void @__ct_delete_array("),
        ],
    )

    # Every Objective-C allocation form clang emits for an Apple target is followed by a
    # call recording the object, and no other message is. Cross-compiling to IR checks the
    # pass on every host.
    tc_instrument_objc_apple = TestCase(
        name="compile_instrument_objc_allocations_apple",
        plan=CompilePlan(
            name="compile_instrument_objc_allocations_apple",
            sources=[Path("objc_alloc_forms.m")],
            out=None,
            extra_args=["--target=arm64-apple-macosx15.0", "--instrument", "--ct-modules=alloc",
                        "--in-mem", "-S", "-emit-llvm"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_stdout_count("call void @__ct_objc_track(", 5),
        ],
    )
    # GNUstep 2.2 uses the same runtime functions, but the instrumentation runtime tracks
    # objects with the Apple runtime only: a GNUstep program must not depend on it.
    tc_instrument_objc_gnustep = TestCase(
        name="compile_instrument_objc_allocations_gnustep",
        plan=CompilePlan(
            name="compile_instrument_objc_allocations_gnustep",
            sources=[Path("objc_alloc_forms.m")],
            out=None,
            extra_args=["--target=x86_64-unknown-linux-gnu", "-fobjc-runtime=gnustep-2.2",
                        "--instrument", "--ct-modules=alloc", "--in-mem", "-S", "-emit-llvm"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_stdout_contains("call ptr @objc_alloc("),
            assert_stdout_count("call void @__ct_objc_track(", 0),
        ],
    )

    # A frame registers the stack objects its bounds checks or its callees may need, and
    # unregisters them on return; objects only accessed at constant offsets inside them
    # cost nothing.
    tc_instrument_bounds_stack_objects = TestCase(
        name="compile_instrument_bounds_stack_objects",
        plan=CompilePlan(
            name="compile_instrument_bounds_stack_objects",
            sources=[Path("stack_objects.c")],
            out=None,
            extra_args=["--instrument", "--ct-modules=bounds", "--in-mem", "-S", "-emit-llvm"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_stdout_count("call i64 @__ct_stack_push(", 2),
            assert_stdout_count("call void @__ct_stack_pop(", 1),
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
            native_artifact_assert_at("hello.o", platform.os),
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
            native_artifact_assert_at("hello.o", platform.os),
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
            native_artifact_assert_at("app", platform.os),
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
            native_artifact_assert_at("app_shadow", platform.os),
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
            native_artifact_assert_at("app_shadow_aggr", platform.os),
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
            native_artifact_assert_at("app_vtable", platform.os),
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

    tc_optnone_emit_llvm = TestCase(
        name="compile_optnone_emit_llvm",
        plan=CompilePlan(
            name="compile_optnone_emit_llvm",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--ct-optnone", "-O1", "-S", "-emit-llvm", "-o=hello_optnone.ll"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--ct-optnone", "-O1", "-S", "-emit-llvm"]),
            assert_output_exists_at("hello_optnone.ll"),
            assert_output_kind_at("hello_optnone.ll", ArtifactKind.LLVM_IR_TEXT),
            assert_output_nonempty_at("hello_optnone.ll"),
            assert_file_contains("hello_optnone.ll", "optnone"),
        ],
    )

    tc_optnone_disable_o0 = TestCase(
        name="compile_optnone_disable_o0",
        plan=CompilePlan(
            name="compile_optnone_disable_o0",
            sources=[Path("hello.c")],
            out=None,
            extra_args=[
                "--ct-optnone",
                "-O0",
                "-Xclang",
                "-disable-O0-optnone",
                "-S",
                "-emit-llvm",
                "-o",
                "-",
            ],
        ),
        assertions=[
            assert_exit_code(0),
            assert_argv_contains(["--ct-optnone", "-O0", "-Xclang", "-disable-O0-optnone"]),
            assert_stdout_contains("optnone"),
            assert_stderr_contains(
                "warning: ct: -disable-O0-optnone ignored because --ct-optnone is enabled"
            ),
        ],
    )

    # Runtime behaviour: an instrumented program that leaks must exit normally and
    # print the leak report; the report runs at process teardown and must not touch
    # logger state that may already be destroyed.
    tc_runtime_leak_report = TestCase(
        name="runtime_leak_report_at_exit",
        plan=CompilePlan(
            name="runtime_leak_report_at_exit",
            sources=[Path("leak.c")],
            out=None,
            extra_args=["--instrument", "--ct-modules=alloc", "-o", "leak_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("leak_app"),
            assert_run_artifact("leak_app", 0, "ct: leaks detected"),
        ],
    )
    tc_runtime_cpp_leak_report = TestCase(
        name="runtime_cpp_leak_report_at_exit",
        plan=CompilePlan(
            name="runtime_cpp_leak_report_at_exit",
            sources=[Path("new_delete.cpp")],
            out=None,
            extra_args=["--instrument", "--ct-modules=alloc", "-o", "new_delete_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("new_delete_app"),
            # The deleted object and array are gone, only the leaked int remains.
            assert_run_artifact("new_delete_app", 0, "ct: leaks detected count=1"),
        ],
    )

    # Runtime behaviour: bounds diagnostics must be reported even when the trace
    # module is not part of the build.
    tc_runtime_bounds_without_trace = TestCase(
        name="runtime_bounds_report_without_trace",
        plan=CompilePlan(
            name="runtime_bounds_report_without_trace",
            sources=[Path("overflow.c")],
            out=None,
            extra_args=["--instrument", "--ct-modules=alloc,bounds", "--ct-bounds-no-abort",
                        "-o", "overflow_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("overflow_app"),
            assert_run_artifact("overflow_app", 0, "heap-buffer-overflow"),
        ],
    )
    # A local array read past its end by the function it is passed to.
    tc_runtime_bounds_stack = TestCase(
        name="runtime_bounds_stack_overflow",
        plan=CompilePlan(
            name="runtime_bounds_stack_overflow",
            sources=[Path("stack_overflow.c")],
            out=None,
            extra_args=["--instrument", "--ct-modules=bounds", "--ct-bounds-no-abort",
                        "-o", "stack_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("stack_app"),
            assert_run_artifact("stack_app", 0, "stack-buffer-overflow"),
        ],
    )
    # A C function has no decorated name: the trace prints it once, not as "main, main".
    tc_runtime_trace_c_function = TestCase(
        name="runtime_trace_c_function_name",
        plan=CompilePlan(
            name="runtime_trace_c_function_name",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--instrument", "--ct-modules=trace", "-o", "trace_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("trace_app"),
            assert_run_artifact("trace_app", 0, "[ENTRY-FUNCTION]: -> main\n"),
        ],
    )
    # CT_BACKTRACE installs the runtime's fatal-error handler: a signal handler on POSIX,
    # an unhandled-exception filter on Windows. It reports the fault and, on Windows,
    # the symbolised frames, before the process dies.
    tc_runtime_backtrace = TestCase(
        name="runtime_backtrace_on_fatal_error",
        plan=CompilePlan(
            name="runtime_backtrace_on_fatal_error",
            sources=[Path("crash.c")],
            out=None,
            extra_args=["--instrument", "--ct-modules=trace", "-o", "crash_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("crash_app"),
            assert_run_artifact(
                "crash_app", None,
                ["ct: fatal exception code=", "  at "] if platform.os == OS.WINDOWS
                else ["ct: fatal signal 11"],
                env={"CT_BACKTRACE": "1"}),
        ],
    )
    # Traced C++ functions entered from several threads at once: their names are decoded
    # concurrently, which on Windows goes through the serialised DbgHelp.
    tc_runtime_trace_threads = TestCase(
        name="runtime_trace_cpp_function_from_threads",
        plan=CompilePlan(
            name="runtime_trace_cpp_function_from_threads",
            sources=[Path("trace_threads.cpp")],
            out=None,
            extra_args=["--instrument", "--ct-modules=trace", "-o", "trace_threads_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("trace_threads_app"),
            assert_run_artifact("trace_threads_app", 0, "work(int)"),
        ],
    )
    # clang names Objective-C methods with LLVM's asm-label marker, @"\01-[Greeter greet:]":
    # the trace prints the method name without it.
    tc_runtime_trace_objc_method = TestCase(
        name="runtime_trace_objc_method_name",
        plan=CompilePlan(
            name="runtime_trace_objc_method_name",
            sources=[Path("trace_objc.m")],
            out=None,
            extra_args=["--instrument", "--ct-modules=trace", "-framework", "Foundation",
                        "-o", "trace_objc_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("trace_objc_app"),
            assert_run_artifact("trace_objc_app", 0, "[ENTRY-FUNCTION]: -> -[Greeter greet:]\n"),
        ],
    )
    # Allocations made in Objective-C and Objective-C++ methods, built with ARC, are
    # tracked like any other: the freed ones are gone, only the leaked one is reported.
    tc_runtime_objc_leak_report = TestCase(
        name="runtime_objc_leak_report_at_exit",
        plan=CompilePlan(
            name="runtime_objc_leak_report_at_exit",
            sources=[Path("leak_objc.m")],
            out=None,
            extra_args=["--instrument", "--ct-modules=alloc", "-fobjc-arc",
                        "-framework", "Foundation", "-o", "leak_objc_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("leak_objc_app"),
            assert_run_artifact("leak_objc_app", 0, "ct: leaks detected count=1"),
        ],
    )
    tc_runtime_objcxx_leak_report = TestCase(
        name="runtime_objcxx_leak_report_at_exit",
        plan=CompilePlan(
            name="runtime_objcxx_leak_report_at_exit",
            sources=[Path("new_delete_objc.mm")],
            out=None,
            extra_args=["--instrument", "--ct-modules=alloc", "-fobjc-arc",
                        "-framework", "Foundation", "-o", "new_delete_objc_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("new_delete_objc_app"),
            assert_run_artifact("new_delete_objc_app", 0, "ct: leaks detected count=1"),
        ],
    )
    # Objective-C objects are tracked from their allocation to their deallocation by the
    # Objective-C runtime: objc_objects.m keeps four objects alive at exit and releases
    # the others.
    def objc_objects_case(name: str, source: str, extra_args: list[str]) -> TestCase:
        return TestCase(
            name=name,
            plan=CompilePlan(
                name=name,
                sources=[Path(source)],
                out=None,
                extra_args=["--instrument", *extra_args, "-framework", "Foundation", "-o", name],
            ),
            assertions=[
                assert_exit_code(0),
                assert_output_exists_at(name),
                assert_run_artifact(name, 0, "ct: leaks detected count=4"),
            ],
        )
    tc_runtime_objc_objects_arc = objc_objects_case(
        "runtime_objc_object_leaks_arc", "objc_objects.m", ["--ct-modules=alloc", "-fobjc-arc"])
    tc_runtime_objc_objects_mrc = objc_objects_case(
        "runtime_objc_object_leaks_mrc", "objc_objects.m", ["--ct-modules=alloc"])
    # In Objective-C++ the allocations are invokes. Bounds checks with shadow memory read
    # the tracked objects' instance variables, which must stay valid until deallocation.
    tc_runtime_objcxx_objects = objc_objects_case(
        "runtime_objcxx_object_leaks_shadow_bounds", "objc_objects.mm",
        ["--ct-modules=alloc,bounds", "--ct-shadow", "-fobjc-arc"])
    # The conservative scan releases allocations nothing refers to, never an Objective-C
    # object: the lost object is still reported at exit. The budget lets every scan finish.
    tc_runtime_objc_object_autofree_scan = TestCase(
        name="runtime_objc_object_survives_autofree_scan",
        plan=CompilePlan(
            name="runtime_objc_object_survives_autofree_scan",
            sources=[Path("objc_autofree_scan.m")],
            out=None,
            extra_args=["--instrument", "--ct-modules=alloc", "--ct-autofree",
                        "-framework", "Foundation", "-o", "objc_scan_app"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_output_exists_at("objc_scan_app"),
            assert_run_artifact("objc_scan_app", 0, "ct: leaks detected count=1",
                                env={"CT_AUTOFREE_SCAN": "1", "CT_AUTOFREE_SCAN_START": "1",
                                     "CT_AUTOFREE_SCAN_PERIOD_MS": "1",
                                     "CT_AUTOFREE_SCAN_BUDGET_MS": "2000",
                                     "CT_AUTOFREE_SCAN_GLOBALS": "0"}),
        ],
    )

    # Failures must be reported through the exit code even on the non-instrumented
    # path, which delegates to the clang driver.
    tc_fail_compile = TestCase(
        name="plain_compile_error_exits_nonzero",
        plan=CompilePlan(
            name="plain_compile_error_exits_nonzero",
            sources=[Path("broken.c")],
            out=None,
            extra_args=["-c"],
        ),
        assertions=[
            assert_exit_code(1),
            assert_stderr_contains("undeclared_symbol"),
        ],
    )
    # The instrumented path generates code in process: an error there must reach cc's
    # own report, with the frontend's warning, instead of LLVM ending the process.
    tc_instrument_fail_codegen = TestCase(
        name="instrument_codegen_error_exits_nonzero",
        plan=CompilePlan(
            name="instrument_codegen_error_exits_nonzero",
            sources=[Path("codegen_error.c")],
            out=None,
            extra_args=["--instrument", "-c"],
        ),
        assertions=[
            assert_exit_code(1),
            assert_stderr_contains("ct_not_an_instruction"),
            assert_stderr_contains("frontend warning before a code-generation error"),
        ],
    )
    tc_fail_missing_input = TestCase(
        name="plain_missing_input_exits_nonzero",
        plan=CompilePlan(
            name="plain_missing_input_exits_nonzero",
            sources=[Path("does_not_exist.c")],
            out=None,
            extra_args=["-o", "missing_app"],
        ),
        assertions=[
            assert_exit_code(1),
            assert_stderr_contains("does_not_exist.c"),
        ],
    )
    tc_fail_link = TestCase(
        name="plain_link_error_exits_nonzero",
        plan=CompilePlan(
            name="plain_link_error_exits_nonzero",
            sources=[Path("undefined_ref.c")],
            out=None,
            extra_args=["-o", "undefined_app"],
        ),
        assertions=[
            assert_exit_code(1),
            # link.exe reports unresolved symbols on stdout, which cc passes through
            # untouched; ld and ld64 report them on stderr.
            assert_stdout_contains("never_defined")
            if platform.os == OS.WINDOWS
            else assert_stderr_contains("never_defined"),
        ],
    )

    tc_version = TestCase(
        name="version_flag",
        plan=CompilePlan(
            name="version_flag",
            sources=[Path("hello.c")],
            out=None,
            extra_args=["--version"],
        ),
        assertions=[
            assert_exit_code(0),
            assert_stdout_contains("CoreTrace Compiler "),
            assert_stdout_matches(r"CoreTrace Compiler [0-9]+\.[0-9]+\.[0-9]+"),
        ],
    )

    common_cases = [tc_version, tc_o_eq, tc_d_space, tc_d_compact, tc_cpp, tc_x_cxx,
                    tc_fail_compile, tc_fail_missing_input, tc_fail_link]
    instrument_cases = [
        tc_instrument_c,
        tc_instrument_cpp,
        tc_instrument_x_cxx,
        tc_instrument_o_eq_trailing,
        tc_instrument_g0_inmem,
        tc_instrument_cpp_microsoft_abi,
        tc_instrument_objc_apple,
        tc_instrument_objc_gnustep,
        tc_instrument_bounds_stack_objects,
        tc_instrument_emit_llvm,
        tc_instrument_emit_bc,
        tc_instrument_fail_codegen,
    ]
    runtime_cases = [
        tc_runtime_leak_report,
        tc_runtime_cpp_leak_report,
        tc_runtime_bounds_without_trace,
        tc_runtime_bounds_stack,
        tc_runtime_trace_c_function,
        tc_runtime_backtrace,
        tc_runtime_trace_threads,
    ]
    # Objective-C programs need the Apple runtime and Foundation.
    objc_cases = [
        tc_runtime_trace_objc_method,
        tc_runtime_objc_leak_report,
        tc_runtime_objcxx_leak_report,
        tc_runtime_objc_objects_arc,
        tc_runtime_objc_objects_mrc,
        tc_runtime_objcxx_objects,
        tc_runtime_objc_object_autofree_scan,
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
        tc_optnone_emit_llvm,
        tc_optnone_disable_o0,
    ]
    if platform.os == OS.MACOS:
        cases = [tc_macho, *common_cases, *instrument_cases, *runtime_cases, *objc_cases,
                 *readme_cases]
    elif platform.os == OS.LINUX:
        cases = [tc_elf, *common_cases, *instrument_cases, *runtime_cases, *readme_cases]
    else:
        windows_readme_cases = [
            tc_readme_emit_llvm,
            tc_readme_c_obj,
            tc_readme_c_obj_o2,
            tc_readme_instrument,
            tc_readme_shadow,
            tc_readme_shadow_aggr,
            tc_readme_inmem,
            tc_optnone_emit_llvm,
            tc_optnone_disable_o0,
        ]
        cases = [tc_native, *common_cases, *instrument_cases, *runtime_cases,
                 *windows_readme_cases]

    suite = TestSuite(name="compiler_smoke", cases=cases)

    reports = []
    WORK.mkdir(parents=True, exist_ok=True)
    for case in suite.cases:
        import tempfile
        with tempfile.TemporaryDirectory(prefix=f"{case.name}_", dir=str(WORK)) as d:
            ws = Path(d)
            copy_fixtures(ws, [src, debug_src, cpp_src, cpp_as_c_src, vtable_src,
                               leak_src, overflow_src, broken_src, codegen_error_src,
                               undefined_ref_src,
                               alloc_site_src, new_delete_src, crash_src,
                               trace_threads_src, trace_objc_src, leak_objc_src,
                               new_delete_objc_src, objc_alloc_forms_src, objc_objects_src,
                               objcxx_objects_src, objc_autofree_scan_src,
                               stack_overflow_src, stack_objects_src])
            reports.append(case.run(runner, ws))

    rep = type("Tmp", (), {"name": suite.name, "reports": reports})()
    return ConsoleReporter().render(rep)

if __name__ == "__main__":
    raise SystemExit(main())
