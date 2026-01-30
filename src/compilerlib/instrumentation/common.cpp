#include "compilerlib/instrumentation/common.hpp"
#include "compilerlib/attributes.hpp"

#include <llvm/ADT/SmallString.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
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

    } // namespace

    CT_NODISCARD std::string formatSiteString(const llvm::Instruction& inst)
    {
        llvm::DebugLoc loc = inst.getDebugLoc();
        if (!loc)
            return "<unknown>";

        const llvm::DILocation* di = loc.get();
        if (!di)
            return "<unknown>";

        llvm::StringRef filename = di->getFilename();
        llvm::StringRef base = llvm::sys::path::filename(filename);
        std::string site = base.str();
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

    bool shouldInstrument(const llvm::Function& func)
    {
        if (func.isDeclaration())
            return false;
        if (func.getName().starts_with("__ct_"))
            return false;
        if (func.hasFnAttribute("no_instrument_function") ||
            func.hasFnAttribute(llvm::Attribute::Naked))
        {
            return false;
        }
        if (func.hasAvailableExternallyLinkage() || func.hasLinkOnceODRLinkage() ||
            func.hasLinkOnceAnyLinkage() || func.hasWeakAnyLinkage() || func.hasWeakODRLinkage())
        {
            return false;
        }

        if (auto* subprogram = func.getSubprogram())
        {
            llvm::StringRef dir = subprogram->getDirectory();
            llvm::StringRef file = subprogram->getFilename();
            if (!dir.empty() && !file.empty())
            {
                llvm::SmallString<256> fullPath(dir);
                llvm::sys::path::append(fullPath, file);
                if (isSystemPath(fullPath))
                    return false;
            }
            else if (!file.empty() && isSystemPath(file))
            {
                return false;
            }
        }

        return true;
    }

} // namespace compilerlib
