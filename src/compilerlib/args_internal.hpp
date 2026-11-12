// SPDX-License-Identifier: Apache-2.0
//
// Argument helpers shared by the compiler driver and its unit tests. Internal to
// compilerlib: not installed, not part of the public API.
#ifndef COMPILERLIB_ARGS_INTERNAL_HPP
#define COMPILERLIB_ARGS_INTERNAL_HPP

#include "compilerlib/attributes.hpp"

#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Option/Option.h>
#include <llvm/TargetParser/Triple.h>

#include <string>
#include <vector>

namespace llvm::vfs
{
    class FileSystem;
} // namespace llvm::vfs

namespace compilerlib
{
    // Value of `opt` given either as a separate argument (`-o out`) or joined with
    // `=` (`-o=out`); nullptr when absent or when `opt` is the last argument.
    CT_NODISCARD const char* findArgValue(const llvm::opt::ArgStringList& args,
                                          llvm::StringRef opt);

    // Exact-match presence test.
    CT_NODISCARD bool hasArg(const std::vector<std::string>& args, llvm::StringRef opt);

    // Whether clang emits debug information for `args`: the last option of its -g group
    // (-g, -g1..3, -gline-tables-only, -gdwarf*, -ggdb*, ...) decides, and -g0 or -ggdb0
    // asks for none. Options outside the group, such as -gcolumn-info, -gsplit-dwarf or
    // -gno-column-info, only change the information another option asks for.
    CT_NODISCARD bool requestsDebugInfo(const std::vector<std::string>& args);

    // Target named by the last -target/--target[=] argument, else the host triple.
    CT_NODISCARD llvm::Triple effectiveTargetTriple(const std::vector<std::string>& args);

    // Rewrites the `-o=path` and `-x=lang` spellings into the two-argument forms.
    void normalizeEqualsArgs(std::vector<std::string>& args);

    // Rewrites each -working-directory of a driver command line, `-working-directory <dir>` or
    // `-working-directory=<dir>`, into the two-argument form, with <dir> made absolute by `fs`.
    // Returns the directory of the last one, which the driver uses; empty without one.
    CT_NODISCARD std::string normalizeWorkingDirectoryArgs(std::vector<std::string>& args,
                                                           llvm::vfs::FileSystem& fs);

    // The arguments of a -cc1 job, with the relative path of each file it writes made absolute
    // against `directory`: its output, dependency file, header list, optimization record, split
    // DWARF, stack usage, serialized diagnostics and time trace. A job resolves the files it
    // reads, and its output, against its -working-directory, but the other files it writes
    // against the process's working directory, which clang itself moves to that directory
    // first. `save` keeps a new argument alive as long as the job.
    CT_NODISCARD llvm::opt::ArgStringList
    resolveCc1OutputPaths(const llvm::opt::ArgStringList& args, llvm::StringRef directory,
                          llvm::function_ref<const char*(llvm::StringRef)> save);

    // Appends `extra` to `out` with exactly one newline between them; no-op when empty.
    void appendDiagnostics(std::string& out, const std::string& extra);

    // Concatenates driver and cc1 diagnostics with exactly one newline between them.
    CT_NODISCARD std::string mergeDiagnostics(const std::string& driver, const std::string& cc1);

    CT_NODISCARD bool isCc1Command(const llvm::opt::ArgStringList& args);

} // namespace compilerlib

#endif // COMPILERLIB_ARGS_INTERNAL_HPP
