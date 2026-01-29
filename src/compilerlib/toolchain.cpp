#include "compilerlib/toolchain.hpp"

#include <clang/Driver/Driver.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Object/Archive.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/MachOUniversal.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Config/llvm-config.h>

#include <cstdlib>
#include <system_error>

namespace compilerlib
{
namespace
{

CT_NODISCARD bool isCxxLang(llvm::StringRef lang)
{
    if (lang.empty())
        return false;
    if (lang.starts_with("="))
        lang = lang.substr(1);
    return lang == "c++" ||
           lang == "c++-header" ||
           lang == "c++-cpp-output" ||
           lang == "objective-c++" ||
           lang == "objective-c++-header";
}

CT_NODISCARD bool isCxxSourceExt(llvm::StringRef ext)
{
    if (ext == ".C")
        return true;
    std::string lower = ext.lower();
    return lower == ".cc" ||
           lower == ".cpp" ||
           lower == ".cxx" ||
           lower == ".c++" ||
           lower == ".cp" ||
           lower == ".mm";
}

CT_NODISCARD bool isSourceExt(llvm::StringRef ext)
{
    if (ext.empty())
        return false;
    std::string lower = ext.lower();
    return lower == ".c" ||
           lower == ".cc" ||
           lower == ".cpp" ||
           lower == ".cxx" ||
           lower == ".c++" ||
           lower == ".cp" ||
           lower == ".m" ||
           lower == ".mm";
}

CT_NODISCARD bool isObjectExt(llvm::StringRef ext)
{
    if (ext.empty())
        return false;
    std::string lower = ext.lower();
    return lower == ".o" || lower == ".obj";
}

CT_NODISCARD bool isArchiveExt(llvm::StringRef ext)
{
    if (ext.empty())
        return false;
    std::string lower = ext.lower();
    return lower == ".a" || lower == ".lib";
}

CT_NODISCARD bool looksLikeCxxSymbol(llvm::StringRef name)
{
    if (name.starts_with("_Z") || name.starts_with("__Z"))
        return true;
    if (name.starts_with("__cxa") || name.starts_with("___cxa"))
        return true;
    if (name.starts_with("__gxx_personality_v0") || name.starts_with("___gxx_personality_v0"))
        return true;
    return false;
}

CT_NODISCARD bool objectHasCxxSymbols(llvm::object::ObjectFile &obj)
{
    for (const auto &sym : obj.symbols())
    {
        llvm::Expected<llvm::StringRef> nameOrErr = sym.getName();
        if (!nameOrErr)
        {
            llvm::consumeError(nameOrErr.takeError());
            continue;
        }
        if (looksLikeCxxSymbol(*nameOrErr))
            return true;
    }
    return false;
}

CT_NODISCARD bool binaryHasCxxSymbols(llvm::object::Binary &binary)
{
    if (auto *obj = llvm::dyn_cast<llvm::object::ObjectFile>(&binary))
        return objectHasCxxSymbols(*obj);

    if (auto *arch = llvm::dyn_cast<llvm::object::Archive>(&binary))
    {
        llvm::Error err = llvm::Error::success();
        for (const auto &child : arch->children(err))
        {
            llvm::Expected<std::unique_ptr<llvm::object::Binary>> childBin = child.getAsBinary();
            if (!childBin)
            {
                llvm::consumeError(childBin.takeError());
                continue;
            }
            if (binaryHasCxxSymbols(**childBin))
                return true;
        }
        if (err)
            llvm::consumeError(std::move(err));
        return false;
    }

    if (auto *fat = llvm::dyn_cast<llvm::object::MachOUniversalBinary>(&binary))
    {
        for (const auto &objForArch : fat->objects())
        {
            llvm::Expected<std::unique_ptr<llvm::object::ObjectFile>> objOrErr = objForArch.getAsObjectFile();
            if (!objOrErr)
            {
                llvm::consumeError(objOrErr.takeError());
                continue;
            }
            if (objectHasCxxSymbols(**objOrErr))
                return true;
        }
        return false;
    }

    return false;
}

CT_NODISCARD llvm::Expected<bool> pathLooksLikeCxxObject(const std::string &path)
{
    llvm::Expected<llvm::object::OwningBinary<llvm::object::Binary>> binOrErr =
        llvm::object::createBinary(path);
    if (!binOrErr)
    {
        return binOrErr.takeError();
    }
    llvm::object::Binary *bin = binOrErr->getBinary();
    if (!bin)
        return llvm::createStringError(std::errc::invalid_argument, "invalid binary");
    return binaryHasCxxSymbols(*bin);
}

CT_NODISCARD bool takesValue(llvm::StringRef arg)
{
    return arg == "-o" ||
           arg == "-x" ||
           arg == "-target" ||
           arg == "--target" ||
           arg == "-gcc-toolchain" ||
           arg == "-isysroot" ||
           arg == "-I" ||
           arg == "-isystem" ||
           arg == "-iquote" ||
           arg == "-idirafter" ||
           arg == "-iprefix" ||
           arg == "-iwithprefix" ||
           arg == "-iwithprefixbefore" ||
           arg == "-include" ||
           arg == "-imacros" ||
           arg == "-D" ||
           arg == "-U" ||
           arg == "-L" ||
           arg == "-F" ||
           arg == "-MF" ||
           arg == "-MT" ||
           arg == "-MQ" ||
           arg == "-Xclang" ||
           arg == "-Xlinker" ||
           arg == "-Xassembler" ||
           arg == "-Xpreprocessor" ||
           arg == "-Wl,";
}

struct ArgScan
{
    bool has_driver_mode = false;
    bool has_resource_dir = false;
    bool has_sysroot = false;
    bool needs_cxx_driver = false;
    bool has_source_inputs = false;
    bool has_object_inputs = false;
    std::vector<std::string> inputs;
};

CT_NODISCARD ArgScan scanArgs(const std::vector<std::string> &args)
{
    ArgScan scan;
    bool end_of_opts = false;

    for (size_t i = 0; i < args.size(); ++i)
    {
        llvm::StringRef arg = args[i];
        if (!end_of_opts && arg == "--")
        {
            end_of_opts = true;
            continue;
        }

        if (!end_of_opts && arg.starts_with("-"))
        {
            if (arg == "--driver-mode" || arg.starts_with("--driver-mode="))
            {
                scan.has_driver_mode = true;
                if (arg == "--driver-mode" && i + 1 < args.size())
                    ++i;
                continue;
            }
            if (arg == "-resource-dir" || arg.starts_with("-resource-dir="))
            {
                scan.has_resource_dir = true;
                if (arg == "-resource-dir" && i + 1 < args.size())
                    ++i;
                continue;
            }
            if (arg == "-isysroot")
            {
                scan.has_sysroot = true;
                if (i + 1 < args.size())
                    ++i;
                continue;
            }
            if (arg.starts_with("-isysroot=") || arg.starts_with("--sysroot="))
            {
                scan.has_sysroot = true;
                continue;
            }
            if (arg == "-x")
            {
                if (i + 1 < args.size() && isCxxLang(args[i + 1]))
                    scan.needs_cxx_driver = true;
                if (i + 1 < args.size())
                    ++i;
                continue;
            }
            if (arg.starts_with("-x="))
            {
                llvm::StringRef lang = arg.substr(3);
                if (isCxxLang(lang))
                    scan.needs_cxx_driver = true;
                continue;
            }
            if (arg.starts_with("-x"))
            {
                llvm::StringRef lang = arg.substr(2);
                if (isCxxLang(lang))
                    scan.needs_cxx_driver = true;
                continue;
            }
            if (arg.starts_with("-o="))
            {
                continue;
            }
            if (arg.starts_with("-stdlib="))
            {
                scan.needs_cxx_driver = true;
                continue;
            }
            if (arg == "-lstdc++" || arg == "-lc++")
            {
                scan.needs_cxx_driver = true;
                continue;
            }
            if (takesValue(arg))
            {
                if (i + 1 < args.size())
                    ++i;
                continue;
            }
            continue;
        }

        scan.inputs.push_back(args[i]);
        llvm::StringRef ext = llvm::sys::path::extension(arg);
        if (isCxxSourceExt(ext))
            scan.needs_cxx_driver = true;
        if (isSourceExt(ext))
            scan.has_source_inputs = true;
        if (isObjectExt(ext) || isArchiveExt(ext))
            scan.has_object_inputs = true;
    }

    return scan;
}

CT_NODISCARD std::string findClangPath(void)
{
    if (const char *env = std::getenv("CT_CLANG"))
    {
        if (llvm::sys::fs::exists(env))
            return env;
    }
#ifdef CT_LLVM_BIN_DIR
    {
        std::string versioned = std::string("clang-") + std::to_string(LLVM_VERSION_MAJOR);
        llvm::SmallString<256> candidate;
        candidate = CT_LLVM_BIN_DIR;
        llvm::sys::path::append(candidate, versioned);
        if (llvm::sys::fs::exists(candidate))
            return candidate.str().str();

        candidate = CT_LLVM_BIN_DIR;
        llvm::sys::path::append(candidate, "clang");
        if (llvm::sys::fs::exists(candidate))
            return candidate.str().str();

        candidate = CT_LLVM_BIN_DIR;
        llvm::sys::path::append(candidate, "clang++");
        if (llvm::sys::fs::exists(candidate))
            return candidate.str().str();
    }
#endif
#ifdef CT_CLANG_EXECUTABLE
    if (llvm::sys::fs::exists(CT_CLANG_EXECUTABLE))
        return CT_CLANG_EXECUTABLE;
#endif
    {
        std::string versioned = std::string("clang-") + std::to_string(LLVM_VERSION_MAJOR);
        if (auto found = llvm::sys::findProgramByName(versioned))
            return *found;
    }
    if (auto found = llvm::sys::findProgramByName("clang"))
        return *found;
    if (auto found = llvm::sys::findProgramByName("clang++"))
        return *found;
    return {};
}

CT_NODISCARD std::string detectResourceDir(const std::string &clang_path)
{
    if (!clang_path.empty())
    {
        std::string resource = clang::driver::Driver::GetResourcesPath(clang_path);
        if (!resource.empty() && llvm::sys::fs::exists(resource))
            return resource;
    }
#ifdef CLANG_RESOURCE_DIR
    if (llvm::sys::fs::exists(CLANG_RESOURCE_DIR))
        return CLANG_RESOURCE_DIR;
#endif
    return {};
}

CT_NODISCARD std::string detectMacSysroot()
{
#ifdef __APPLE__
    if (auto found = llvm::sys::findProgramByName("xcrun"))
    {
        llvm::SmallString<256> outPath;
        if (std::error_code ec = llvm::sys::fs::createTemporaryFile("ct_sysroot", "txt", outPath))
            return {};

        llvm::SmallString<256> errPath;
        if (std::error_code ec = llvm::sys::fs::createTemporaryFile("ct_sysroot_err", "txt", errPath))
            return {};

        llvm::SmallVector<llvm::StringRef, 4> args;
        args.push_back(*found);
        args.push_back("--show-sdk-path");

        std::array<std::optional<llvm::StringRef>, 3> redirects{
            std::nullopt,
            llvm::StringRef(outPath),
            llvm::StringRef(errPath)
        };

        int rc = llvm::sys::ExecuteAndWait(*found, args, std::nullopt, redirects);
        if (rc == 0)
        {
            llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buf = llvm::MemoryBuffer::getFile(outPath);
            if (buf && *buf)
            {
                llvm::StringRef content = (*buf)->getBuffer().trim();
                if (!content.empty() && llvm::sys::fs::exists(content))
                    return content.str();
            }
        }
    }
#endif
    return {};
}

} // namespace

CT_NODISCARD bool resolveDriverConfig(const std::vector<std::string> &args,
                                      DriverConfig &out,
                                      std::string &error)
{
    out = {};

    ArgScan scan = scanArgs(args);
    out.force_cxx_driver = scan.needs_cxx_driver;

    if (!scan.has_source_inputs && scan.has_object_inputs && !out.force_cxx_driver)
    {
        for (const auto &path : scan.inputs)
        {
            llvm::StringRef ext = llvm::sys::path::extension(path);
            if (!isObjectExt(ext) && !isArchiveExt(ext))
                continue;
            llvm::Expected<bool> looksCxx = pathLooksLikeCxxObject(path);
            if (!looksCxx)
            {
                error = "failed to inspect object: " + path + ": " + llvm::toString(looksCxx.takeError());
                return false;
            }
            if (*looksCxx)
            {
                out.force_cxx_driver = true;
                break;
            }
        }
    }

    if (scan.has_driver_mode)
        out.force_cxx_driver = false;

    out.clang_path = findClangPath();
    if (out.clang_path.empty())
    {
        error = "unable to find clang executable in PATH";
        return false;
    }

    if (!scan.has_resource_dir)
    {
        out.resource_dir = detectResourceDir(out.clang_path);
        if (!out.resource_dir.empty())
            out.add_resource_dir = true;
    }

    if (!scan.has_sysroot)
    {
        out.sysroot = detectMacSysroot();
        if (!out.sysroot.empty())
            out.add_sysroot = true;
    }

    return true;
}

} // namespace compilerlib
