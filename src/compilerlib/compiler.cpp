#include "compilerlib/compiler.h"

#include <clang/Frontend/FrontendActions.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/CodeGen/CodeGenAction.h>


#include <llvm/Config/llvm-config.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm-c/Target.h>

#include <cstring>

namespace compilerlib {

namespace {
constexpr llvm::StringRef kTargetTriple = LLVM_DEFAULT_TARGET_TRIPLE;

struct DiagsSaver : clang::DiagnosticConsumer {
    std::string message;
    llvm::raw_string_ostream os{message};

    void HandleDiagnostic(clang::DiagnosticsEngine::Level diagLevel, const clang::Diagnostic &info) override {
        DiagnosticConsumer::HandleDiagnostic(diagLevel, info);
        const char *level;
        switch (diagLevel) {
            case clang::DiagnosticsEngine::Note:
                level = "note";
                break;
            case clang::DiagnosticsEngine::Warning:
                level = "warning";
                break;
            case clang::DiagnosticsEngine::Error:
            case clang::DiagnosticsEngine::Fatal:
                level = "error";
                break;
            default:
                return;
        }

        llvm::SmallString<256> msg;
        info.FormatDiagnostic(msg);
        auto &sm = info.getSourceManager();
        auto loc = info.getLocation();
        auto fileLoc = sm.getFileLoc(loc);
        os << sm.getFilename(fileLoc) << ':' << sm.getSpellingLineNumber(fileLoc)
           << ':' << sm.getSpellingColumnNumber(fileLoc) << ": " << level << ": "
           << msg << '\n';
        if (loc.isMacroID()) {
            loc = sm.getSpellingLoc(loc);
            os << sm.getFilename(loc) << ':' << sm.getSpellingLineNumber(loc) << ':'
               << sm.getSpellingColumnNumber(loc) << ": note: expanded from macro\n";
        }
    }
};
} // anonymous namespace

std::pair<bool, std::string> compile(const std::vector<std::string>& input_args) {
    auto fs = llvm::vfs::getRealFileSystem();
    DiagsSaver dc;

    std::vector<const char *> args = {"clang"};
#ifdef __APPLE__
    args.insert(args.end(), {"-isysroot", "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                             "-I", "/Library/Developer/CommandLineTools/usr/include",
                             "-I", "/Library/Developer/CommandLineTools/usr/include/c++/v1"});
#else
    args.insert(args.end(), {"-I", "/usr/include",
                             "-I", "/usr/include/c++/11",
                             "-I", "/usr/local/include"});
#endif
    for (const auto& arg : input_args) {
        args.push_back(arg.c_str());
    }

    auto diags = clang::CompilerInstance::createDiagnostics(
#if LLVM_VERSION_MAJOR >= 20
        *fs,
#endif
        new clang::DiagnosticOptions, &dc, false);

    clang::driver::Driver d(args[0], kTargetTriple, *diags, "cc", fs);
    d.setCheckInputsExist(false);
    std::unique_ptr<clang::driver::Compilation> comp(d.BuildCompilation(args));
    const auto &jobs = comp->getJobs();
    if (jobs.size() != 1)
    {
        return {false, "only support one job"};
    }

    const llvm::opt::ArgStringList &ccArgs = jobs.begin()->getArguments();
    auto invoc = std::make_unique<clang::CompilerInvocation>();
    clang::CompilerInvocation::CreateFromArgs(*invoc, ccArgs, *diags);

    auto ci = std::make_unique<clang::CompilerInstance>();
    ci->setInvocation(std::move(invoc));

    // Adaptation to different LLVM/Clang versions
    // - LLVM < 16: createDiagnostics(VFS, Consumer, ShouldOwnClient)
    // - LLVM 16–18: createDiagnostics(Consumer, ShouldOwnClient)
    // - LLVM 19–20+: the overloads without a VFS have been removed, you must pass the VFS.
    #if LLVM_VERSION_MAJOR >= 19
        ci->createDiagnostics(*fs, &dc, false);
    #else
        ci->createDiagnostics(&dc, false);
    #endif

    ci->getDiagnostics().getDiagnosticOptions().ShowCarets = false;
    ci->createFileManager(fs);
    ci->createSourceManager(ci->getFileManager());
    ci->getCodeGenOpts().DisableFree = false;
    ci->getFrontendOpts().DisableFree = false;

    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeAllAsmPrinters();

    switch (ci->getFrontendOpts().ProgramAction)
    {
        case clang::frontend::EmitObj:
        {
            clang::EmitObjAction action;
            ci->ExecuteAction(action);
            break;
        }
        case clang::frontend::EmitAssembly:
        {
            clang::EmitAssemblyAction action;
            ci->ExecuteAction(action);
            break;
        }
        case clang::frontend::EmitBC:
        {
            clang::EmitBCAction action;
            ci->ExecuteAction(action);
            break;
        }
        case clang::frontend::EmitLLVM:
        {
            clang::EmitLLVMAction action;
            ci->ExecuteAction(action);
            break;
        }
        default:
            return {false, "Unhandled action"};
    }
    return {true, std::move(dc.message)};
}

// TODO : improve this code to use a better interface
extern "C" int compile_c(int argc, const char** argv, char* output_buffer, int buffer_size)
{
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i)
        args.emplace_back(argv[i]);

    auto [success, message] = compilerlib::compile(args);

    std::strncpy(output_buffer, message.c_str(), buffer_size - 1);
    output_buffer[buffer_size - 1] = '\0';

    return success ? 1 : 0;
}

} // namespace compilerlib
