#include "compilerlib/compiler.h"
#include "compilerlib/attributes.hpp"
#include "compilerlib/toolchain.hpp"

#include "compilerlib/instrumentation/alloc.hpp"
#include "compilerlib/instrumentation/bounds.hpp"
#include "compilerlib/instrumentation/config.hpp"
#include "compilerlib/instrumentation/trace.hpp"
#include "compilerlib/instrumentation/vtable.hpp"

#include <clang/Frontend/FrontendActions.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendOptions.h>
#include <clang/CodeGen/CodeGenAction.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>
#include <llvm-c/Target.h>

#include <cstring>
#include <iostream>
#include <memory>
#include <type_traits>

namespace compilerlib {

namespace {
constexpr llvm::StringRef kTargetTriple = LLVM_DEFAULT_TARGET_TRIPLE;

struct DiagsSaver : clang::DiagnosticConsumer {
    std::string message;
    llvm::raw_string_ostream os{message};

    void HandleDiagnostic(clang::DiagnosticsEngine::Level diagLevel,
                          const clang::Diagnostic &info) override {
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
        auto loc = info.getLocation();
        if (!info.hasSourceManager() || loc.isInvalid()) {
            os << level << ": " << msg << '\n';
            return;
        }

        auto &sm = info.getSourceManager();
        auto fileLoc = sm.getFileLoc(loc);
        if (fileLoc.isInvalid()) {
            os << level << ": " << msg << '\n';
            return;
        }

        os << sm.getFilename(fileLoc) << ':' << sm.getSpellingLineNumber(fileLoc)
           << ':' << sm.getSpellingColumnNumber(fileLoc) << ": " << level << ": "
           << msg << '\n';
        if (loc.isMacroID()) {
            auto spellingLoc = sm.getSpellingLoc(loc);
            if (spellingLoc.isValid()) {
                os << sm.getFilename(spellingLoc) << ':' << sm.getSpellingLineNumber(spellingLoc) << ':'
                   << sm.getSpellingColumnNumber(spellingLoc) << ": note: expanded from macro\n";
            }
        }
    }
};

llvm::CodeGenOptLevel toCodeGenOptLevel(unsigned level)
{
    switch (level) {
        case 0:
            return llvm::CodeGenOptLevel::None;
        case 1:
            return llvm::CodeGenOptLevel::Less;
        case 2:
            return llvm::CodeGenOptLevel::Default;
        case 3:
            return llvm::CodeGenOptLevel::Aggressive;
        default:
            return llvm::CodeGenOptLevel::Default;
    }
}

const char *findArgValue(const llvm::opt::ArgStringList &args, llvm::StringRef opt)
{
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (opt == args[i]) {
            return args[i + 1];
        }
        if (llvm::StringRef(args[i]).starts_with(opt) &&
            llvm::StringRef(args[i]).size() > opt.size() &&
            llvm::StringRef(args[i])[opt.size()] == '=') {
            return args[i] + opt.size() + 1;
        }
    }
    return nullptr;
}

bool hasArg(const std::vector<std::string> &args, llvm::StringRef opt)
{
    for (const auto &arg : args) {
        if (opt == arg) {
            return true;
        }
    }
    return false;
}

bool hasDebugFlag(const std::vector<std::string> &args)
{
    for (const auto &arg : args) {
        if (arg.rfind("-g", 0) == 0) {
            return true;
        }
    }
    return false;
}

void appendDiagnostics(std::string &out, const std::string &extra)
{
    if (extra.empty()) {
        return;
    }
    if (!out.empty() && out.back() != '\n') {
        out.push_back('\n');
    }
    out.append(extra);
}

std::string mergeDiagnostics(const std::string &driver, const std::string &cc1)
{
    std::string merged = driver;
    appendDiagnostics(merged, cc1);
    return merged;
}

bool isCc1Command(const llvm::opt::ArgStringList &args)
{
    for (const char *arg : args) {
        if (std::strcmp(arg, "-cc1") == 0) {
            return true;
        }
    }
    return false;
}

template <typename T>
std::unique_ptr<clang::driver::Compilation> takeCompilation(T &&comp)
{
    if constexpr (std::is_pointer_v<std::remove_reference_t<T>>) {
        return std::unique_ptr<clang::driver::Compilation>(comp);
    } else {
        return std::move(comp);
    }
}

struct CompileContext {
    OutputMode mode;
    bool instrument;
    RuntimeConfig runtimeConfig;
    std::vector<std::string> input_args;
    std::vector<std::string> filtered_args;
    std::vector<const char *> clang_args;
    std::string clang_path;
    std::string clang_resource_dir;
    std::string clang_sysroot;
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs;
    DiagsSaver dc;
    std::string driver_diagnostics;

    CompileContext(const std::vector<std::string> &args, OutputMode mode, bool instrument)
        : mode(mode), instrument(instrument), input_args(args), fs(llvm::vfs::getRealFileSystem()) {}
};

class ArgBuilder {
public:
    explicit ArgBuilder(CompileContext &ctx) : ctx_(ctx) {}

    CT_NODISCARD bool build(std::string &error)
    {
        extractRuntimeConfig(ctx_.input_args, ctx_.filtered_args, ctx_.runtimeConfig);
        normalizeEqualsArgs(ctx_.filtered_args);
        if (ctx_.runtimeConfig.bounds_without_alloc) {
            ctx_.dc.os << "warning: ct: bounds instrumentation requires alloc tracking; use --ct-alloc or disable bounds\n";
            ctx_.dc.os.flush();
        }
        DriverConfig driverCfg;
        if (!resolveDriverConfig(ctx_.filtered_args, driverCfg, error)) {
            return false;
        }
        ctx_.clang_path = driverCfg.clang_path;
        ctx_.clang_resource_dir = driverCfg.resource_dir;
        ctx_.clang_sysroot = driverCfg.sysroot;

        ctx_.clang_args.clear();
        ctx_.clang_args.push_back(ctx_.clang_path.c_str());
        if (driverCfg.force_cxx_driver) {
            ctx_.clang_args.push_back("--driver-mode=g++");
        }
        if (driverCfg.add_resource_dir) {
            ctx_.clang_args.push_back("-resource-dir");
            ctx_.clang_args.push_back(ctx_.clang_resource_dir.c_str());
        }
        if (driverCfg.add_sysroot) {
            ctx_.clang_args.push_back("-isysroot");
            ctx_.clang_args.push_back(ctx_.clang_sysroot.c_str());
        }
        for (const auto &arg : ctx_.filtered_args) {
            ctx_.clang_args.push_back(arg.c_str());
        }

        if (ctx_.instrument) {
            if (!hasDebugFlag(ctx_.filtered_args)) {
                ctx_.clang_args.push_back("-gline-tables-only");
            }
            ctx_.clang_args.push_back("-fno-builtin");
            ctx_.clang_args.push_back("-fno-builtin-malloc");
            ctx_.clang_args.push_back("-fno-builtin-free");
            // Ensure position-independent code for Linux targets
            // to avoid relocation errors with PIE-enabled distributions
            if (kTargetTriple.contains("linux")) {
                if (!hasArg(ctx_.filtered_args, "-fPIE") && !hasArg(ctx_.filtered_args, "-fPIC")) {
                    ctx_.clang_args.push_back("-fPIE");
                }
            }
        }

        if (ctx_.instrument && ctx_.mode == OutputMode::ToFile && linkRequested()) {
#ifdef CT_RUNTIME_LIB_PATH
            // Ensure position-independent executable linking on Linux
            if (kTargetTriple.contains("linux")) {
                if (!hasArg(ctx_.filtered_args, "-pie")) {
                    ctx_.clang_args.push_back("-pie");
                }
            }
            ctx_.clang_args.push_back("-x");
            ctx_.clang_args.push_back("none");
            ctx_.clang_args.push_back(CT_RUNTIME_LIB_PATH);
#ifdef __APPLE__
            ctx_.clang_args.push_back("-lc++");
#else
            ctx_.clang_args.push_back("-lstdc++");
#if defined(__linux__)
            if (ctx_.runtimeConfig.vtable_enabled ||
                ctx_.runtimeConfig.vcall_trace_enabled ||
                ctx_.runtimeConfig.vtable_diag_enabled) {
                ctx_.clang_args.push_back("-ldl");
            }
#endif
#endif
#else
            error = "instrumentation runtime path not configured";
            return false;
#endif
        }

        return true;
    }

private:
    static void normalizeEqualsArgs(std::vector<std::string> &args)
    {
        std::vector<std::string> out;
        out.reserve(args.size() + 2);
        for (auto &arg : args) {
            if (llvm::StringRef(arg).starts_with("-o=")) {
                out.push_back("-o");
                out.push_back(arg.substr(3));
                continue;
            }
            if (llvm::StringRef(arg).starts_with("-x=")) {
                out.push_back("-x");
                out.push_back(arg.substr(3));
                continue;
            }
            out.push_back(std::move(arg));
        }
        args.swap(out);
    }

    CT_NODISCARD bool linkRequested(void) const
    {
        return !(hasArg(ctx_.filtered_args, "-c") ||
                 hasArg(ctx_.filtered_args, "-S") ||
                 hasArg(ctx_.filtered_args, "-E") ||
                 hasArg(ctx_.filtered_args, "-emit-llvm"));
    }

    CompileContext &ctx_;
};

class DriverSession
{
public:
    DriverSession(CompileContext &ctx, clang::DiagnosticsEngine &diags)
        : ctx_(ctx),
          diags_(diags),
          driver_(std::make_unique<clang::driver::Driver>(
              ctx_.clang_path,
              kTargetTriple,
              diags_,
              "cc",
              ctx_.fs))
    {
        driver_->setCheckInputsExist(false);
    }

    CT_NODISCARD std::unique_ptr<clang::driver::Compilation> buildCompilation(std::string &error)
    {
        auto comp = driver_->BuildCompilation(ctx_.clang_args);
        auto ownedComp = takeCompilation(std::move(comp));
        if (!ownedComp) {
            error = ctx_.dc.message.empty() ? "failed to build compilation" : std::move(ctx_.dc.message);
        }
        return ownedComp;
    }

    clang::driver::Driver &driver()
    {
        return *driver_;
    }

private:
    CompileContext &ctx_;
    clang::DiagnosticsEngine &diags_;
    std::unique_ptr<clang::driver::Driver> driver_;
};

struct JobPlan
{
    llvm::SmallVector<const clang::driver::Command *, 4> cc1Jobs;
    llvm::SmallVector<const clang::driver::Command *, 4> otherJobs;
};

CT_NODISCARD JobPlan buildJobPlan(const clang::driver::Compilation &comp)
{
    JobPlan plan;

    for (const auto &job : comp.getJobs())
    {
        const auto &jobArgs = job.getArguments();

        if (isCc1Command(jobArgs))
            plan.cc1Jobs.push_back(&job);
        else
            plan.otherJobs.push_back(&job);
    }
    return plan;
}

CT_NODISCARD bool emitObjectFile(llvm::Module &module,
                    const clang::CompilerInstance &ci,
                    llvm::StringRef outputPath,
                    std::string &error)
{
    std::string targetTriple = module.getTargetTriple();

    if (targetTriple.empty())
        targetTriple = llvm::sys::getDefaultTargetTriple();
    module.setTargetTriple(targetTriple);

    std::string targetError;
    const llvm::Target *target = llvm::TargetRegistry::lookupTarget(targetTriple, targetError);
    if (!target)
    {
        error = targetError;
        return false;
    }

    const auto &targetOpts = ci.getTargetOpts();
    std::string features;
    for (const auto &feature : targetOpts.FeaturesAsWritten)
    {
        if (!features.empty())
            features += ",";
        features += feature;
    }

    llvm::TargetOptions options;
    auto codegenLevel = toCodeGenOptLevel(ci.getCodeGenOpts().OptimizationLevel);
    std::unique_ptr<llvm::TargetMachine> targetMachine(
        target->createTargetMachine(targetTriple,
                                    targetOpts.CPU,
                                    features,
                                    options,
                                    std::nullopt,
                                    std::nullopt,
                                    codegenLevel));
    if (!targetMachine)
    {
        error = "failed to create target machine";
        return false;
    }

    module.setDataLayout(targetMachine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec)
    {
        error = ec.message();
        return false;
    }

    llvm::legacy::PassManager pass;
    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, llvm::CodeGenFileType::ObjectFile))
    {
        error = "target does not support object emission";
        return false;
    }

    pass.run(module);
    dest.flush();
    return true;
}

class Cc1Runner {
public:
    Cc1Runner(CompileContext &ctx, clang::DiagnosticsEngine &driverDiags)
        : ctx_(ctx), driverDiags_(driverDiags)
    {
        initTargetsOnce();
    }

    CT_NODISCARD bool runInstrumented(const clang::driver::Command &job, std::string &error)
    {
        const llvm::opt::ArgStringList &ccArgs = job.getArguments();
        auto ci = makeCompilerInstance(ccArgs);
        if (!ci)
        {
            error = "failed to create compiler instance";
            return false;
        }

        if (ci->getFrontendOpts().ProgramAction != clang::frontend::EmitObj)
        {
            error = "instrumentation only supports object/binary output";
            return false;
        }

        clang::EmitLLVMOnlyAction action;
        resetDiagnostics();
        if (!ci->ExecuteAction(action))
        {
            error = std::move(ctx_.dc.message);
            return false;
        }

        std::unique_ptr<llvm::Module> module = action.takeModule();
        if (!module)
        {
            error = "failed to generate LLVM module";
            return false;
        }

        if (ctx_.runtimeConfig.trace_enabled) {
            instrumentModule(*module);
        }
        if (ctx_.runtimeConfig.alloc_enabled) {
            wrapAllocCalls(*module);
        }
        if (ctx_.runtimeConfig.bounds_enabled) {
            instrumentMemoryAccesses(*module);
        }
        if (ctx_.runtimeConfig.vtable_enabled || ctx_.runtimeConfig.vcall_trace_enabled) {
            instrumentVirtualCalls(*module,
                                   ctx_.runtimeConfig.vcall_trace_enabled,
                                   ctx_.runtimeConfig.vtable_enabled);
        }
        emitRuntimeConfigGlobals(*module, ctx_.runtimeConfig);

        const char *outputObj = findArgValue(ccArgs, "-o");
        if (!outputObj) {
            error = "unable to determine output object file";
            return false;
        }

        if (!emitObjectFile(*module, *ci, outputObj, error)) {
            return false;
        }

        return true;
    }

    CompileResult runSingle(const clang::driver::Command &job, bool includeDriverDiags = true)
    {
        CompileResult result;
        result.success = false;
        result.diagnostics = {};
        result.llvmIR = {};

        auto fail = [&](const char *fallback) -> CompileResult
        {
            result.success = false;
            std::string diag = ctx_.dc.message.empty() ? fallback : std::move(ctx_.dc.message);
            result.diagnostics = includeDriverDiags ? mergeDiagnostics(ctx_.driver_diagnostics, diag) : std::move(diag);
            return result;
        };

        const llvm::opt::ArgStringList &ccArgs = job.getArguments();
        auto ci = makeCompilerInstance(ccArgs);
        if (!ci) {
            result.diagnostics = "failed to create compiler instance";
            return result;
        }

        switch (ci->getFrontendOpts().ProgramAction)
        {
            case clang::frontend::EmitObj:
            {
                clang::EmitObjAction action;

                resetDiagnostics();
                if (!ci->ExecuteAction(action))
                    return fail("compilation failed");
                break;
            }
            case clang::frontend::EmitAssembly:
            {
                clang::EmitAssemblyAction action;

                resetDiagnostics();
                if (!ci->ExecuteAction(action))
                    return fail("compilation failed");
                break;
            }
            case clang::frontend::EmitBC:
            {
                clang::EmitBCAction action;

                resetDiagnostics();
                if (!ci->ExecuteAction(action))
                    return fail("compilation failed");
                break;
            }
            case clang::frontend::EmitLLVM:
            {
                if (ctx_.mode == OutputMode::ToFile)
                {
                    clang::EmitLLVMAction action;

                    resetDiagnostics();
                    if (!ci->ExecuteAction(action))
                        return fail("compilation failed");
                }
                if (ctx_.mode == OutputMode::ToMemory)
                {
                    clang::EmitLLVMOnlyAction action;

                    resetDiagnostics();
                    if (!ci->ExecuteAction(action))
                    {
                        result.success     = false;
                        if (includeDriverDiags) {
                            result.diagnostics = mergeDiagnostics(ctx_.driver_diagnostics,
                                                                  std::move(ctx_.dc.message));
                        } else {
                            result.diagnostics = std::move(ctx_.dc.message);
                        }
                        return result;
                    }
                    std::unique_ptr<llvm::Module> module = action.takeModule();
                    if (module)
                    {
                        if (ctx_.instrument)
                        {
                            if (ctx_.runtimeConfig.trace_enabled)
                                instrumentModule(*module);
                            if (ctx_.runtimeConfig.alloc_enabled)
                                wrapAllocCalls(*module);
                            if (ctx_.runtimeConfig.bounds_enabled)
                                instrumentMemoryAccesses(*module);
                            if (ctx_.runtimeConfig.vtable_enabled || ctx_.runtimeConfig.vcall_trace_enabled)
                                instrumentVirtualCalls(*module,
                                                       ctx_.runtimeConfig.vcall_trace_enabled,
                                                       ctx_.runtimeConfig.vtable_enabled);
                            emitRuntimeConfigGlobals(*module, ctx_.runtimeConfig);
                        }
                        std::string llvmIR;
                        llvm::raw_string_ostream rso(llvmIR);
                        module->print(rso, nullptr);
                        rso.flush();
                        result.llvmIR = std::move(llvmIR);
                    }
                }
                break;
            }
            case clang::frontend::PrintPreprocessedInput:
            {
                clang::PrintPreprocessedAction action;

                resetDiagnostics();
                if (!ci->ExecuteAction(action))
                    return fail("preprocessing failed");
                break;
            }
            case clang::frontend::RunPreprocessorOnly:
            {
                clang::PreprocessOnlyAction action;

                resetDiagnostics();
                if (!ci->ExecuteAction(action))
                    return fail("preprocessing failed");
                break;
            }
            case clang::frontend::ParseSyntaxOnly:
            {
                clang::SyntaxOnlyAction action;

                resetDiagnostics();
                if (!ci->ExecuteAction(action))
                    return fail("parsing failed");
                break;
            }
            default:
                result.success = false;
                result.diagnostics = "Unhandled action";
                return result;
        }

        result.success     = true;
        if (includeDriverDiags) {
            result.diagnostics = mergeDiagnostics(ctx_.driver_diagnostics, ctx_.dc.message);
        } else {
            result.diagnostics = std::move(ctx_.dc.message);
        }
        return result;
    }

private:
    static void initTargetsOnce(void)
    {
        static bool initialized = false;
        if (initialized)
            return;
        initialized = true;

        LLVMInitializeAllTargetInfos();
        LLVMInitializeAllTargets();
        LLVMInitializeAllTargetMCs();
        LLVMInitializeAllAsmParsers();
        LLVMInitializeAllAsmPrinters();
    }

    void resetDiagnostics(void)
    {
        ctx_.dc.message.clear();
        ctx_.dc.os.flush();
    }

    CT_NODISCARD std::unique_ptr<clang::CompilerInstance> makeCompilerInstance(const llvm::opt::ArgStringList &ccArgs)
    {
        auto invoc = std::make_unique<clang::CompilerInvocation>();
        clang::CompilerInvocation::CreateFromArgs(*invoc, ccArgs, driverDiags_);

        auto ci = std::make_unique<clang::CompilerInstance>();
        ci->setInvocation(std::move(invoc));

        // Adaptation to different LLVM/Clang versions
        // - LLVM < 16: createDiagnostics(VFS, Consumer, ShouldOwnClient)
        // - LLVM 16–18: createDiagnostics(Consumer, ShouldOwnClient)
        // - LLVM 19–20+: the overloads without a VFS have been removed, you must pass the VFS.
        #if LLVM_VERSION_MAJOR >= 19
            ci->createDiagnostics(*ctx_.fs, &ctx_.dc, false);
        #else
            ci->createDiagnostics(&ctx_.dc, false);
        #endif

        ci->getDiagnostics().getDiagnosticOptions().ShowCarets = false;
        ci->createFileManager(ctx_.fs);
        ci->createSourceManager(ci->getFileManager());
        ci->getCodeGenOpts().DisableFree = false;
        ci->getFrontendOpts().DisableFree = false;

        return ci;
    }

    CompileContext &ctx_;
    clang::DiagnosticsEngine &driverDiags_;
};

class Linker
{
public:
    CT_NODISCARD bool run(const llvm::SmallVector<const clang::driver::Command *, 4> &jobs,
             std::string &error) const
    {
        for (const auto *job : jobs)
        {
            std::string errMsg;
            bool execFailed = false;
            int rc = job->Execute({}, &errMsg, &execFailed);

            if (execFailed || rc != 0)
            {
                if (errMsg.empty())
                    errMsg = "link job failed";
                error = std::move(errMsg);
                return false;
            }
        }
        return true;
    }
};

CT_NODISCARD llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine> createDriverDiagnostics(CompileContext &ctx)
{
    return clang::CompilerInstance::createDiagnostics(
#if LLVM_VERSION_MAJOR >= 20
        *ctx.fs,
#endif
        new clang::DiagnosticOptions,
        &ctx.dc,
        false);
}

CT_NODISCARD CompileResult runNonInstrumentedCompilation(CompileContext &ctx,
                                            DriverSession &driver,
                                            clang::driver::Compilation &comp)
{
    llvm::SmallVector<std::pair<int, const clang::driver::Command *>, 4> failingCommands;
    int rc = driver.driver().ExecuteCompilation(comp, failingCommands);

    CompileResult result;
    result.success = (rc == 0);
    result.diagnostics = std::move(ctx.dc.message);
    result.llvmIR = {};
    if (!result.success && result.diagnostics.empty()) {
        result.diagnostics = "compilation failed";
    }
    return result;
}

CT_NODISCARD bool validateJobPlan(const JobPlan &plan, OutputMode mode, std::string &error)
{
    if (mode == OutputMode::ToMemory)
    {
        if (plan.cc1Jobs.size() != 1 || !plan.otherJobs.empty())
        {
            error = "in-memory output only supports a single compilation job";
            return false;
        }
        return true;
    }
    if (plan.cc1Jobs.empty())
    {
        if (mode == OutputMode::ToFile && !plan.otherJobs.empty())
            return true;
        error = "no cc1 job found";
        return false;
    }
    return true;
}

CT_NODISCARD CompileResult runInstrumentedToFile(CompileContext &ctx,
                                    Cc1Runner &cc1,
                                    const JobPlan &plan,
                                    std::string &error)
{
    Linker linker;
    std::string cc1_diags;

    for (const auto *job : plan.cc1Jobs)
    {
        if (!cc1.runInstrumented(*job, error))
            return {false, mergeDiagnostics(ctx.driver_diagnostics, mergeDiagnostics(cc1_diags, error)), {}};
        appendDiagnostics(cc1_diags, ctx.dc.message);
        ctx.dc.message.clear();
        ctx.dc.os.flush();
    }

    if (!linker.run(plan.otherJobs, error))
        return {false, mergeDiagnostics(ctx.driver_diagnostics, mergeDiagnostics(cc1_diags, error)), {}};

    return {true, mergeDiagnostics(ctx.driver_diagnostics, cc1_diags), {}};
}

CT_NODISCARD CompileResult runPlainToFile(CompileContext &ctx,
                                Cc1Runner &cc1,
                                const JobPlan &plan,
                                std::string &error)
{
    Linker linker;
    std::string cc1_diags;

    for (const auto *job : plan.cc1Jobs)
    {
        CompileResult res = cc1.runSingle(*job, false);
        if (!res.success)
            return {false, mergeDiagnostics(ctx.driver_diagnostics, mergeDiagnostics(cc1_diags, res.diagnostics)), {}};
        appendDiagnostics(cc1_diags, res.diagnostics);
    }

    if (!linker.run(plan.otherJobs, error))
        return {false, mergeDiagnostics(ctx.driver_diagnostics, mergeDiagnostics(cc1_diags, error)), {}};

    return {true, mergeDiagnostics(ctx.driver_diagnostics, cc1_diags), {}};
}

} // namespace

CT_NODISCARD CompileResult compile(
    const std::vector<std::string>& input_args,
    OutputMode mode,
    bool instrument
)
{
    CompileContext ctx(input_args, mode, instrument);
    ArgBuilder argBuilder(ctx);
    std::string error;

    if (!argBuilder.build(error))
        return {false, std::move(error), {}};

    auto diags = createDriverDiagnostics(ctx);

    DriverSession driver(ctx, *diags);
    std::unique_ptr<clang::driver::Compilation> comp = driver.buildCompilation(error);
    if (!comp)
    {
        if (error.empty())
            error = ctx.dc.message.empty() ? "failed to build compilation" : std::move(ctx.dc.message);
        return {false, std::move(error), {}};
    }

    if (comp->getJobs().empty())
        return {false, ctx.dc.message.empty() ? "no jobs to run" : std::move(ctx.dc.message), {}};

    if (!instrument && mode == OutputMode::ToFile)
        return runNonInstrumentedCompilation(ctx, driver, *comp);

    JobPlan plan = buildJobPlan(*comp);
    if (!validateJobPlan(plan, mode, error))
        return {false, std::move(error), {}};

    if (!ctx.dc.message.empty())
    {
        ctx.driver_diagnostics = std::move(ctx.dc.message);
        ctx.dc.message.clear();
        ctx.dc.os.flush();
    }

    Cc1Runner cc1(ctx, *diags);
    if (mode == OutputMode::ToMemory)
        return cc1.runSingle(*plan.cc1Jobs.front());

    if (plan.cc1Jobs.empty())
    {
        Linker linker;
        if (!linker.run(plan.otherJobs, error))
            return {false, mergeDiagnostics(ctx.driver_diagnostics, error), {}};
        return {true, mergeDiagnostics(ctx.driver_diagnostics, {}), {}};
    }

    if (instrument)
        return runInstrumentedToFile(ctx, cc1, plan, error);
    return runPlainToFile(ctx, cc1, plan, error);
}

extern "C" int compile_c(int argc, const char** argv, char* output_buffer, int buffer_size)
{
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    OutputMode mode = OutputMode::ToFile;
    bool instrument = false;

    for (int i = 0; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--in-mem" || arg == "--in-memory")
        {
            mode = OutputMode::ToMemory;
        }
        else if (arg == "--instrument")
        {
            instrument = true;
        }
        else
        {
            args.emplace_back(std::move(arg));
        }
    }

    CompileResult result = compilerlib::compile(args, mode, instrument);

    std::strncpy(output_buffer, result.diagnostics.c_str(), buffer_size - 1);
    output_buffer[buffer_size - 1] = '\0';

    return result.success ? 1 : 0;
}

} // namespace compilerlib
