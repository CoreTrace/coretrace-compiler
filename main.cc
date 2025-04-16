
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendOptions.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm-c/Target.h>

#include <iostream>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

constexpr llvm::StringRef kTargetTriple = LLVM_DEFAULT_TARGET_TRIPLE;

namespace
{
    struct DiagsSaver : clang::DiagnosticConsumer
    {
        std::string message;
        llvm::raw_string_ostream os{message};

        void HandleDiagnostic(clang::DiagnosticsEngine::Level diagLevel, const clang::Diagnostic &info) override
        {
        DiagnosticConsumer::HandleDiagnostic(diagLevel, info);
        const char *level;
        switch (diagLevel)
        {
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
        if (loc.isMacroID())
        {
            loc = sm.getSpellingLoc(loc);
            os << sm.getFilename(loc) << ':' << sm.getSpellingLineNumber(loc) << ':'
            << sm.getSpellingColumnNumber(loc) << ": note: expanded from macro\n";
        }
        }
    };
}

static std::pair<bool, std::string> compile(int argc, char *argv[])
{
    auto fs = llvm::vfs::getRealFileSystem();
    DiagsSaver dc;

    // std::vector<const char *> args{"clang"};
    // args.insert(args.end(), argv + 1, argv + argc);
    std::vector<const char *> args = {"clang"};

    #ifdef __APPLE__
    args.insert(args.end(), {"-isysroot", "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
                            "-I", "/Library/Developer/CommandLineTools/usr/include",
                            "-I", "/Library/Developer/CommandLineTools/usr/include/c++/v1"});
    #else
    args.insert(args.end(), {"-I", "/usr/include",
                            "-I", "/usr/include/c++/11", // ou c++/9, c++/10 selon distro/gcc
                            "-I", "/usr/local/include"});
    #endif
    args.insert(args.end(), argv + 1, argv + argc);
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
        return {false, "only support one job"};

    const llvm::opt::ArgStringList &ccArgs = jobs.begin()->getArguments();

    auto invoc = std::make_unique<clang::CompilerInvocation>();
    clang::CompilerInvocation::CreateFromArgs(*invoc, ccArgs, *diags);

    auto ci = std::make_unique<clang::CompilerInstance>();
    ci->setInvocation(std::move(invoc));
    ci->createDiagnostics(*fs, &dc, false);
    ci->getDiagnostics().getDiagnosticOptions().ShowCarets = false;
    ci->createFileManager(fs);
    ci->createSourceManager(ci->getFileManager());
    ci->getCodeGenOpts().DisableFree = false;
    ci->getFrontendOpts().DisableFree = false;

    // LLVM target initialization for macOS (x86_64 or ARM64)
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
        case clang::frontend::EmitAssembly: /* -S */
        {
            clang::EmitAssemblyAction action;
            ci->ExecuteAction(action);
            break;
        }
        case clang::frontend::EmitBC: /* -emit-llvm -c */
        {
            clang::EmitBCAction action;
            ci->ExecuteAction(action);
            break;
        }
        case clang::frontend::EmitLLVM: /* -emit-llvm */
        {
            clang::EmitLLVMAction action;
            ci->ExecuteAction(action);
            break;
        }
        default:
            return {false, "Unhandled action"};
      }
  // case frontend::EmitLLVMOnly: {
  //   EmitLLVMOnlyAction action;
  //   ci->ExecuteAction(action);
  //   auto mod = action.takeModule();
  //   if (mod) {
  //     std::error_code EC;
  //     llvm::raw_fd_ostream out("output.ll", EC, llvm::sys::fs::OF_None);
  //     if (EC) {
  //       return {false, "Error open output.ll : " + EC.message()};
  //     }
  //     mod->print(out, nullptr);
  //   }
  //   break;
  // }
  // EmitLLVMOnlyAction action;
  // ci->ExecuteAction(action);

  // // Sauvegarde dans un fichier .ll
  // if (auto mod = action.takeModule()) {
  //   std::cout << "LLVM IR genered in output.ll\n";
  //   std::error_code EC;
  //   llvm::raw_fd_ostream out("output.ll", EC, llvm::sys::fs::OF_None);
  //   if (EC) {
  //     return {false, "Error open output.ll : " + EC.message()};
  //   }
  //   mod->print(out, nullptr);
  // }

    return {true, std::move(dc.message)};
}

int main(int argc, char *argv[])
{
    auto [ok, err] = compile(argc, argv);
    llvm::errs() << err;
}
