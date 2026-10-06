// SPDX-License-Identifier: Apache-2.0
//
// Clang frontend and driver interfaces that changed between LLVM 16 and 23. compilerlib
// builds against all of them; the differences between versions stay here. Internal to
// compilerlib: not installed, not part of the public API.
#pragma once

#include <clang/Basic/CodeGenOptions.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticOptions.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/CodeGen/BackendUtil.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <llvm/Support/raw_ostream.h>

#if LLVM_VERSION_MAJOR >= 18
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#endif

#if LLVM_VERSION_MAJOR >= 22
#include <clang/Options/OptionUtils.h>
#include <clang/Options/Options.h>
#else
#include <clang/Driver/Driver.h>
#include <clang/Driver/Options.h>
#endif

#include <memory>
#include <string>
#include <utility>

namespace compilerlib::clang_compat
{
    // The driver's option identifiers (OPT_*), which LLVM 22 moved out of the driver.
#if LLVM_VERSION_MAJOR >= 22
    namespace options = clang::options;
#else
    namespace options = clang::driver::options;
#endif

    // Whether clang can run a pass of compilerlib at the start of its optimization pipeline,
    // before any optimization: CodeGenOptions::PassBuilderCallbacks, from LLVM 18.
    inline constexpr bool kHasPipelineStartPass = LLVM_VERSION_MAJOR >= 18;

    // A function on a module, as a pass of the new pass manager.
    struct ModuleFunctionPass : llvm::PassInfoMixin<ModuleFunctionPass>
    {
        void (*function)(llvm::Module&);

        llvm::PreservedAnalyses run(llvm::Module& module, llvm::ModuleAnalysisManager&)
        {
            function(module);
            return llvm::PreservedAnalyses::none();
        }
    };

    // Runs `function` on each module at the start of clang's optimization pipeline. Does
    // nothing without kHasPipelineStartPass.
    inline void runAtPipelineStart(clang::CodeGenOptions& options, void (*function)(llvm::Module&))
    {
#if LLVM_VERSION_MAJOR >= 18
        options.PassBuilderCallbacks.push_back(
            [function](llvm::PassBuilder& builder)
            {
                builder.registerPipelineStartEPCallback(
                    [function](llvm::ModulePassManager& passes, llvm::OptimizationLevel)
                    { passes.addPass(ModuleFunctionPass{{}, function}); });
            });
#else
        (void)options;
        (void)function;
#endif
    }

    // The file -fstack-usage writes, which LLVM 23 renamed.
    inline const std::string& stackUsageOutput(const clang::CodeGenOptions& options)
    {
#if LLVM_VERSION_MAJOR >= 23
        return options.StackUsageFile;
#else
        return options.StackUsageOutput;
#endif
    }

    // Runs Clang's backend on `module` as the instance's frontend action would: its
    // optimization pipeline unless `options` disable it, then the output `action` asks for,
    // with the instance's target and code generation options. Diagnostics go to the
    // instance's engine; code generation errors also go to the module's context.
    inline void emitBackendOutput(clang::CompilerInstance& ci, clang::CodeGenOptions& options,
                                  llvm::Module& module, clang::BackendAction action,
                                  std::unique_ptr<llvm::raw_pwrite_stream> stream)
    {
        const llvm::StringRef layout = ci.getTarget().getDataLayoutString();
#if LLVM_VERSION_MAJOR >= 20
        clang::emitBackendOutput(ci, options, layout, &module, action,
                                 ci.getFileManager().getVirtualFileSystemPtr(), std::move(stream));
#elif LLVM_VERSION_MAJOR >= 17
        clang::EmitBackendOutput(ci.getDiagnostics(), ci.getHeaderSearchOpts(), options,
                                 ci.getTargetOpts(), ci.getLangOpts(), layout, &module, action,
                                 ci.getFileManager().getVirtualFileSystemPtr(), std::move(stream));
#else
        clang::EmitBackendOutput(ci.getDiagnostics(), ci.getHeaderSearchOpts(), options,
                                 ci.getTargetOpts(), ci.getLangOpts(), layout, &module, action,
                                 std::move(stream));
#endif
    }

    // The resource directory of the clang at clangPath, as that clang computes it.
    inline std::string resourcesPath(llvm::StringRef clangPath)
    {
#if LLVM_VERSION_MAJOR >= 22
        return clang::GetResourcesPath(clangPath);
#else
        return clang::driver::Driver::GetResourcesPath(clangPath);
#endif
    }

    // Options of a DiagnosticsEngine that compilerlib creates itself. LLVM 21 passes them
    // by reference, so they must outlive the engine; earlier versions share them through
    // a reference count. get() returns what DiagnosticsEngine and
    // CompilerInstance::createDiagnostics take.
    class DiagnosticOptionsStorage
    {
      public:
#if LLVM_VERSION_MAJOR >= 21
        clang::DiagnosticOptions& get()
        {
            return options_;
        }

      private:
        clang::DiagnosticOptions options_;
#else
        clang::DiagnosticOptions* get()
        {
            return options_.get();
        }

      private:
        llvm::IntrusiveRefCntPtr<clang::DiagnosticOptions> options_{new clang::DiagnosticOptions};
#endif
    };

    // The instance's own diagnostic options, as TextDiagnosticPrinter takes them.
#if LLVM_VERSION_MAJOR >= 21
    inline clang::DiagnosticOptions& diagnosticOptions(clang::CompilerInstance& ci)
    {
        return ci.getDiagnosticOpts();
    }
#else
    inline clang::DiagnosticOptions* diagnosticOptions(clang::CompilerInstance& ci)
    {
        return &ci.getDiagnosticOpts();
    }
#endif

    // An instance for the given invocation. LLVM 21 takes it at construction only.
    inline std::unique_ptr<clang::CompilerInstance>
    makeCompilerInstance(std::shared_ptr<clang::CompilerInvocation> invocation)
    {
#if LLVM_VERSION_MAJOR >= 21
        return std::make_unique<clang::CompilerInstance>(std::move(invocation));
#else
        auto ci = std::make_unique<clang::CompilerInstance>();
        ci->setInvocation(std::move(invocation));
        return ci;
#endif
    }

    // Gives the instance a diagnostics engine that reports to client. From LLVM 20 the
    // engine reads files through a file system; LLVM 22 takes the instance's own, which
    // is set to fs when the instance has none yet.
    inline void createDiagnostics(clang::CompilerInstance& ci,
                                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs,
                                  clang::DiagnosticConsumer* client, bool shouldOwnClient)
    {
#if LLVM_VERSION_MAJOR >= 22
        if (!ci.hasVirtualFileSystem())
            ci.setVirtualFileSystem(std::move(fs));
        ci.createDiagnostics(client, shouldOwnClient);
#elif LLVM_VERSION_MAJOR >= 20
        ci.createDiagnostics(*fs, client, shouldOwnClient);
#else
        (void)fs;
        ci.createDiagnostics(client, shouldOwnClient);
#endif
    }

    // A standalone diagnostics engine, with the given options, reporting to client.
    inline llvm::IntrusiveRefCntPtr<clang::DiagnosticsEngine>
    createDiagnostics(llvm::vfs::FileSystem& fs, DiagnosticOptionsStorage& options,
                      clang::DiagnosticConsumer* client, bool shouldOwnClient)
    {
#if LLVM_VERSION_MAJOR >= 20
        return clang::CompilerInstance::createDiagnostics(fs, options.get(), client,
                                                          shouldOwnClient);
#else
        (void)fs;
        return clang::CompilerInstance::createDiagnostics(options.get(), client, shouldOwnClient);
#endif
    }

    // Creates the instance's file and source managers, reading files through fs. LLVM 22
    // reads them through the instance's file system, which createDiagnostics set.
    inline void createFileAndSourceManagers(clang::CompilerInstance& ci,
                                            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs)
    {
#if LLVM_VERSION_MAJOR >= 22
        if (!ci.hasVirtualFileSystem())
            ci.setVirtualFileSystem(std::move(fs));
        ci.createFileManager();
        ci.createSourceManager();
#else
        ci.createFileManager(std::move(fs));
        ci.createSourceManager(ci.getFileManager());
#endif
    }
} // namespace compilerlib::clang_compat
