// SPDX-License-Identifier: Apache-2.0
//
// Argument helpers shared by the compiler driver and its unit tests. Internal to
// compilerlib: not installed, not part of the public API.
#ifndef COMPILERLIB_ARGS_INTERNAL_HPP
#define COMPILERLIB_ARGS_INTERNAL_HPP

#include "compilerlib/attributes.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/Option/Option.h>
#include <llvm/TargetParser/Triple.h>

#include <string>
#include <vector>

namespace compilerlib
{
    // Value of `opt` given either as a separate argument (`-o out`) or joined with
    // `=` (`-o=out`); nullptr when absent or when `opt` is the last argument.
    CT_NODISCARD const char* findArgValue(const llvm::opt::ArgStringList& args,
                                          llvm::StringRef opt);

    // Exact-match presence test.
    CT_NODISCARD bool hasArg(const std::vector<std::string>& args, llvm::StringRef opt);

    // Mirrors clang's "last debug level wins" rule: -g0 turns debug info off again
    // and -gno-* only adjusts how debug info is emitted. Every other -g* spelling
    // (-g, -g1..3, -gline-tables-only, -gdwarf*, -ggdb*, ...) requests debug info.
    CT_NODISCARD bool requestsDebugInfo(const std::vector<std::string>& args);

    // Target named by the last -target/--target[=] argument, else the host triple.
    CT_NODISCARD llvm::Triple effectiveTargetTriple(const std::vector<std::string>& args);

    // Rewrites the `-o=path` and `-x=lang` spellings into the two-argument forms.
    void normalizeEqualsArgs(std::vector<std::string>& args);

    // Appends `extra` to `out` with exactly one newline between them; no-op when empty.
    void appendDiagnostics(std::string& out, const std::string& extra);

    // Concatenates driver and cc1 diagnostics with exactly one newline between them.
    CT_NODISCARD std::string mergeDiagnostics(const std::string& driver, const std::string& cc1);

    CT_NODISCARD bool isCc1Command(const llvm::opt::ArgStringList& args);

} // namespace compilerlib

#endif // COMPILERLIB_ARGS_INTERNAL_HPP
