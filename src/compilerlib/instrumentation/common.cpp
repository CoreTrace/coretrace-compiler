// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/instrumentation/common.hpp"
#include "compilerlib/attributes.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/Support/Path.h>

namespace compilerlib
{
    namespace
    {

        CT_NODISCARD bool isSystemPath(llvm::StringRef path)
        {
            if (path.empty())
                return false;

            if (path.contains("c++/v1") || path.contains("/lib/clang/"))
                return true;

            return path.starts_with("/Library/Developer/CommandLineTools") ||
                   path.starts_with("/Applications/Xcode.app") ||
                   path.starts_with("/usr/include") || path.starts_with("/usr/local/include");
        }

        // A definition the passes may modify: not runtime-internal, not opted out, and
        // not an available_externally copy, which is discarded for the real body. A function
        // opts out with disable_sanitizer_instrumentation, which the frontend also gives the
        // functions declared no_instrument_function.
        CT_NODISCARD bool isModifiableDefinition(const llvm::Function& func)
        {
            if (func.isDeclaration())
                return false;
            if (func.getName().starts_with("__ct_"))
                return false;
            if (func.hasFnAttribute(llvm::Attribute::DisableSanitizerInstrumentation) ||
                func.hasFnAttribute(llvm::Attribute::Naked))
            {
                return false;
            }
            return !func.hasAvailableExternallyLinkage();
        }

        CT_NODISCARD bool isSystemFile(llvm::StringRef dir, llvm::StringRef file)
        {
            if (file.empty())
                return false;
            if (dir.empty())
                return isSystemPath(file);
            llvm::SmallString<256> fullPath(dir);
            llvm::sys::path::append(fullPath, file);
            return isSystemPath(fullPath);
        }

    } // namespace

    CT_NODISCARD std::string formatSiteString(const llvm::Instruction& inst)
    {
        llvm::DebugLoc loc = inst.getDebugLoc();
        if (!loc)
            return "<unknown>";

        const llvm::DILocation* di = loc.get();
        if (!di)
            return "<unknown>";

        // The path the compiler was given, often relative, so that files with the same
        // name stay apart. Not joined with the compilation directory: binaries built in
        // different directories stay identical.
        std::string site = di->getFilename().str();
        if (site.empty())
            site = "<unknown>";

        unsigned line = di->getLine();
        unsigned col = di->getColumn();
        if (line > 0)
            site += ":" + std::to_string(line);
        if (col > 0)
            site += ":" + std::to_string(col);

        return site;
    }

    bool isUserDefinedFunction(const llvm::Function& func)
    {
        if (!isModifiableDefinition(func))
            return false;
        const llvm::DISubprogram* subprogram = func.getSubprogram();
        return !subprogram || !isSystemFile(subprogram->getDirectory(), subprogram->getFilename());
    }

    bool shouldInstrument(const llvm::Function& func)
    {
        if (!isUserDefinedFunction(func))
            return false;
        return !(func.hasLinkOnceODRLinkage() || func.hasLinkOnceAnyLinkage() ||
                 func.hasWeakAnyLinkage() || func.hasWeakODRLinkage());
    }

    bool shouldRewriteAllocations(const llvm::Function& func)
    {
        return isModifiableDefinition(func);
    }

    bool isUserCall(const llvm::Instruction& call)
    {
        if (const llvm::DILocation* location = call.getDebugLoc().get())
            return !isSystemFile(location->getDirectory(), location->getFilename());
        return isUserDefinedFunction(*call.getFunction());
    }

} // namespace compilerlib
