// SPDX-License-Identifier: Apache-2.0
//
// Clang's backend consumer (clang/lib/CodeGen/CodeGenAction.cpp) turns LLVM's diagnostics
// into Clang's, but is not part of Clang's installed headers. This handler follows it, for
// the diagnostics a module's optimization and code generation report; keep it in step with
// Clang's when that changes. Not covered: what LLVM 23 added, the inlining chain notes of
// -fdiagnostics-show-inlining-chain and unsupported target intrinsics, which are reported
// as other backend diagnostics are.
#include "backend_diagnostics.hpp"

#include <clang/Basic/CodeGenOptions.h>
#include <clang/Basic/Diagnostic.h>
#include <clang/Basic/DiagnosticFrontend.h>
#include <clang/Basic/FileManager.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Basic/SourceManager.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/DiagnosticHandler.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/DiagnosticPrinter.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <optional>
#include <string>
#include <utility>

namespace compilerlib::emit
{
    namespace
    {
        namespace diag = clang::diag;

        // Clang's diagnostics for one kind of backend diagnostic, by LLVM severity.
        struct DiagnosticKind
        {
            unsigned error;
            unsigned warning;
            unsigned remark;
            unsigned note;

            unsigned forSeverity(llvm::DiagnosticSeverity severity) const
            {
                switch (severity)
                {
                case llvm::DS_Error:
                    return error;
                case llvm::DS_Warning:
                    return warning;
                case llvm::DS_Remark:
                    return remark;
                case llvm::DS_Note:
                    return note;
                }
                return error;
            }
        };

        // Clang expects no remark of the kinds that have none; one is reported as a plugin's.
        constexpr DiagnosticKind kInlineAsm{diag::err_fe_inline_asm, diag::warn_fe_inline_asm,
                                            diag::remark_fe_backend_plugin,
                                            diag::note_fe_inline_asm};
        constexpr DiagnosticKind kSourceMgr{diag::err_fe_source_mgr, diag::warn_fe_source_mgr,
                                            diag::remark_fe_backend_plugin,
                                            diag::note_fe_source_mgr};
        constexpr DiagnosticKind kFrameLargerThan{
            diag::err_fe_backend_frame_larger_than, diag::warn_fe_backend_frame_larger_than,
            diag::remark_fe_backend_plugin, diag::note_fe_backend_frame_larger_than};
        constexpr DiagnosticKind kResourceLimit{
            diag::err_fe_backend_resource_limit, diag::warn_fe_backend_resource_limit,
            diag::remark_fe_backend_plugin, diag::note_fe_backend_resource_limit};
        constexpr DiagnosticKind kPlugin{diag::err_fe_backend_plugin, diag::warn_fe_backend_plugin,
                                         diag::remark_fe_backend_plugin,
                                         diag::note_fe_backend_plugin};

        // A location in the temporary buffer of an inline assembly diagnostic, as a location
        // in a copy of that buffer that the source manager owns.
        clang::FullSourceLoc convertBackendLocation(const llvm::SMDiagnostic& diagnostic,
                                                    clang::SourceManager& sources)
        {
            const llvm::SourceMgr& backendSources = *diagnostic.getSourceMgr();
            const llvm::MemoryBuffer* buffer = backendSources.getMemoryBuffer(
                backendSources.FindBufferContainingLoc(diagnostic.getLoc()));
            clang::FileID file = sources.createFileID(llvm::MemoryBuffer::getMemBufferCopy(
                buffer->getBuffer(), buffer->getBufferIdentifier()));
            const unsigned offset =
                static_cast<unsigned>(diagnostic.getLoc().getPointer() - buffer->getBufferStart());
            return clang::FullSourceLoc(sources.getLocForStartOfFile(file).getLocWithOffset(offset),
                                        sources);
        }

        class ClangBackendHandler final : public llvm::DiagnosticHandler
        {
          public:
            ClangBackendHandler(clang::DiagnosticsEngine& diagnostics,
                                clang::SourceManager& sources, const clang::CodeGenOptions& options,
                                FunctionLocations functions)
                : diags_(diagnostics), sources_(sources), options_(options),
                  functions_(std::move(functions))
            {
            }

            bool isAnalysisRemarkEnabled(llvm::StringRef pass) const override
            {
                return options_.OptimizationRemarkAnalysis.patternMatches(pass);
            }

            bool isMissedOptRemarkEnabled(llvm::StringRef pass) const override
            {
                return options_.OptimizationRemarkMissed.patternMatches(pass);
            }

            bool isPassedOptRemarkEnabled(llvm::StringRef pass) const override
            {
                return options_.OptimizationRemark.patternMatches(pass);
            }

            bool isAnyRemarkEnabled() const override
            {
                return options_.OptimizationRemarkAnalysis.hasValidPattern() ||
                       options_.OptimizationRemarkMissed.hasValidPattern() ||
                       options_.OptimizationRemark.hasValidPattern();
            }

            bool handleDiagnostics(const llvm::DiagnosticInfo& info) override
            {
                report(info);
                return true;
            }

          private:
            void report(const llvm::DiagnosticInfo& info)
            {
                const llvm::DiagnosticSeverity severity = info.getSeverity();
                unsigned id = kPlugin.forSeverity(severity);
                switch (info.getKind())
                {
                case llvm::DK_InlineAsm:
                    reportInlineAsm(llvm::cast<llvm::DiagnosticInfoInlineAsm>(info));
                    return;
                case llvm::DK_SrcMgr:
                    reportSourceMgr(llvm::cast<llvm::DiagnosticInfoSrcMgr>(info));
                    return;
                case llvm::DK_StackSize:
                    if (reportStackSize(llvm::cast<llvm::DiagnosticInfoStackSize>(info)))
                        return;
                    id = kFrameLargerThan.forSeverity(severity);
                    break;
                case llvm::DK_ResourceLimit:
                    if (reportResourceLimit(llvm::cast<llvm::DiagnosticInfoResourceLimit>(info)))
                        return;
                    id = kResourceLimit.forSeverity(severity);
                    break;
                case llvm::DK_OptimizationRemark:
                case llvm::DK_OptimizationRemarkMissed:
                case llvm::DK_OptimizationRemarkAnalysis:
                case llvm::DK_MachineOptimizationRemark:
                case llvm::DK_MachineOptimizationRemarkMissed:
                case llvm::DK_MachineOptimizationRemarkAnalysis:
                    reportRemark(llvm::cast<llvm::DiagnosticInfoOptimizationBase>(info));
                    return;
                case llvm::DK_OptimizationRemarkAnalysisFPCommute:
                case llvm::DK_OptimizationRemarkAnalysisAliasing:
                    reportAnalysisRemark(llvm::cast<llvm::OptimizationRemarkAnalysis>(info));
                    return;
                case llvm::DK_OptimizationFailure:
                    reportOptimizationMessage(
                        llvm::cast<llvm::DiagnosticInfoOptimizationBase>(info),
                        diag::warn_fe_backend_optimization_failure);
                    return;
                case llvm::DK_Unsupported:
                    reportUnsupported(llvm::cast<llvm::DiagnosticInfoUnsupported>(info));
                    return;
                case llvm::DK_DontCall:
                    reportDontCall(llvm::cast<llvm::DiagnosticInfoDontCall>(info));
                    return;
                case llvm::DK_MisExpect:
                    reportMisExpect(llvm::cast<llvm::DiagnosticInfoMisExpect>(info));
                    return;
                default:
                    break;
                }
                std::string message;
                llvm::raw_string_ostream stream(message);
                llvm::DiagnosticPrinterRawOStream printer(stream);
                info.print(printer);
                stream.flush();
                diags_.Report(clang::SourceLocation(), id).AddString(message);
            }

            void reportInlineAsm(const llvm::DiagnosticInfoInlineAsm& info)
            {
                // Located at the asm statement when the frontend gave it a location cookie,
                // otherwise in the generated assembly, which has no source location.
                diags_
                    .Report(locationOfCookie(info.getLocCookie()),
                            kInlineAsm.forSeverity(info.getSeverity()))
                    .AddString(info.getMsgStr().str());
            }

            void reportSourceMgr(const llvm::DiagnosticInfoSrcMgr& info)
            {
                const llvm::SMDiagnostic& diagnostic = info.getSMDiag();
                const unsigned id = info.isInlineAsmDiag()
                                        ? kInlineAsm.forSeverity(info.getSeverity())
                                        : kSourceMgr.forSeverity(info.getSeverity());
                llvm::StringRef message = diagnostic.getMessage();
                (void)message.consume_front("error: ");

                clang::FullSourceLoc location;
                if (diagnostic.getLoc() != llvm::SMLoc())
                    location = convertBackendLocation(diagnostic, sources_);

                // In inline assembly with a location cookie: reported at the asm statement,
                // with a note showing the instantiated assembly.
                if (info.isInlineAsmDiag())
                {
                    const clang::SourceLocation cookie = locationOfCookie(info.getLocCookie());
                    if (cookie.isValid())
                    {
                        diags_.Report(cookie, id).AddString(message);
                        if (diagnostic.getLoc().isValid())
                        {
                            clang::DiagnosticBuilder note =
                                diags_.Report(location, diag::note_fe_inline_asm_here);
                            for (const std::pair<unsigned, unsigned>& range :
                                 diagnostic.getRanges())
                            {
                                const int column = diagnostic.getColumnNo();
                                note << clang::SourceRange(
                                    location.getLocWithOffset(static_cast<int>(range.first) -
                                                              column),
                                    location.getLocWithOffset(static_cast<int>(range.second) -
                                                              column));
                            }
                        }
                        return;
                    }
                }
                diags_.Report(location, id).AddString(message);
            }

            bool reportStackSize(const llvm::DiagnosticInfoStackSize& info)
            {
                // The only severity Clang formats for this diagnostic.
                if (info.getSeverity() != llvm::DS_Warning)
                    return false;
                const std::optional<clang::FullSourceLoc> location =
                    functionLocation(info.getFunction());
                if (!location)
                    return false;
                diags_.Report(*location, diag::warn_fe_frame_larger_than)
                    << info.getStackSize() << info.getStackLimit()
                    << llvm::demangle(info.getFunction().getName().str());
                return true;
            }

            bool reportResourceLimit(const llvm::DiagnosticInfoResourceLimit& info)
            {
                const std::optional<clang::FullSourceLoc> location =
                    functionLocation(info.getFunction());
                if (!location)
                    return false;
                diags_.Report(*location, kResourceLimit.forSeverity(info.getSeverity()))
                    << info.getResourceName() << info.getResourceSize() << info.getResourceLimit()
                    << llvm::demangle(info.getFunction().getName().str());
                return true;
            }

            void reportRemark(const llvm::DiagnosticInfoOptimizationBase& info)
            {
                // Without hotness information, verbose remarks are noise.
                if (info.isVerbose() && !info.getHotness())
                    return;
                if (info.isPassed())
                {
                    if (options_.OptimizationRemark.patternMatches(info.getPassName()))
                        reportOptimizationMessage(info,
                                                  diag::remark_fe_backend_optimization_remark);
                }
                else if (info.isMissed())
                {
                    if (options_.OptimizationRemarkMissed.patternMatches(info.getPassName()))
                        reportOptimizationMessage(
                            info, diag::remark_fe_backend_optimization_remark_missed);
                }
                else
                {
                    const auto* analysis = llvm::dyn_cast<llvm::OptimizationRemarkAnalysis>(&info);
                    if ((analysis && analysis->shouldAlwaysPrint()) ||
                        options_.OptimizationRemarkAnalysis.patternMatches(info.getPassName()))
                        reportOptimizationMessage(
                            info, diag::remark_fe_backend_optimization_remark_analysis);
                }
            }

            // The analysis remarks about floating-point commutation and aliasing, which have
            // diagnostics of their own.
            void reportAnalysisRemark(const llvm::OptimizationRemarkAnalysis& info)
            {
                if (!info.shouldAlwaysPrint() &&
                    !options_.OptimizationRemarkAnalysis.patternMatches(info.getPassName()))
                    return;
                reportOptimizationMessage(
                    info, info.getKind() == llvm::DK_OptimizationRemarkAnalysisFPCommute
                              ? diag::remark_fe_backend_optimization_remark_analysis_fpcommute
                              : diag::remark_fe_backend_optimization_remark_analysis_aliasing);
            }

            void reportOptimizationMessage(const llvm::DiagnosticInfoOptimizationBase& info,
                                           unsigned id)
            {
                LocatedDiagnostic located = locate(info);
                std::string message = info.getMsg();
                if (info.getHotness())
                    message += " (hotness: " + std::to_string(*info.getHotness()) + ")";
                diags_.Report(located.location, id)
                    << clang::AddFlagValue(info.getPassName()) << message;
                reportInvalidLocation(located);
            }

            void reportUnsupported(const llvm::DiagnosticInfoUnsupported& info)
            {
                LocatedDiagnostic located = locate(info);
                diags_.Report(located.location, info.getSeverity() == llvm::DS_Error
                                                    ? diag::err_fe_backend_unsupported
                                                    : diag::warn_fe_backend_unsupported)
                    << info.getMessage().str();
                reportInvalidLocation(located);
            }

            void reportDontCall(const llvm::DiagnosticInfoDontCall& info)
            {
                // Indirect calls have no location cookie, and are not diagnosed.
                const clang::SourceLocation cookie = locationOfCookie(info.getLocCookie());
                if (!cookie.isValid())
                    return;
                diags_.Report(cookie, info.getSeverity() == llvm::DS_Error
                                          ? diag::err_fe_backend_error_attr
                                          : diag::warn_fe_backend_warning_attr)
                    << llvm::demangle(info.getFunctionName().str()) << info.getNote();
            }

            void reportMisExpect(const llvm::DiagnosticInfoMisExpect& info)
            {
                LocatedDiagnostic located = locate(info);
                diags_.Report(located.location, diag::warn_profile_data_misexpect)
                    << info.getMsg().str();
                reportInvalidLocation(located);
            }

            // A diagnostic's source location, and the debug location it came from when that
            // could not be translated, as with #line directives.
            struct LocatedDiagnostic
            {
                clang::FullSourceLoc location;
                bool badDebugInfo = false;
                llvm::StringRef file;
                unsigned line = 0;
                unsigned column = 0;
            };

            // The source location of a diagnostic's debug location; without one, the
            // location of its function's declaration.
            LocatedDiagnostic locate(const llvm::DiagnosticInfoWithLocationBase& info)
            {
                LocatedDiagnostic located;
                clang::SourceLocation debugLocation;
                if (info.isLocationAvailable())
                {
                    info.getLocation(located.file, located.line, located.column);
                    if (located.line > 0)
                    {
                        clang::FileManager& files = sources_.getFileManager();
                        auto entry = files.getOptionalFileRef(located.file);
                        if (!entry)
                            entry = files.getOptionalFileRef(info.getAbsolutePath());
                        // Without -gcolumn-info, the column is 0, which the source manager
                        // does not accept.
                        if (entry)
                            debugLocation =
                                sources_.translateFileLineCol(&entry->getFileEntry(), located.line,
                                                              located.column ? located.column : 1);
                    }
                    located.badDebugInfo = debugLocation.isInvalid();
                }
                located.location = clang::FullSourceLoc(debugLocation, sources_);
                if (located.location.isInvalid())
                {
                    if (std::optional<clang::FullSourceLoc> function =
                            functionLocation(info.getFunction()))
                        located.location = *function;
                }
                return located;
            }

            void reportInvalidLocation(const LocatedDiagnostic& located)
            {
                if (located.badDebugInfo)
                    diags_.Report(located.location, diag::note_fe_backend_invalid_loc)
                        << located.file << located.line << located.column;
            }

            std::optional<clang::FullSourceLoc> functionLocation(const llvm::Function& function)
            {
                auto found = functions_.find(function.getName());
                if (found == functions_.end())
                    return std::nullopt;
                return clang::FullSourceLoc(found->second, sources_);
            }

            static clang::SourceLocation locationOfCookie(uint64_t cookie)
            {
                return clang::SourceLocation::getFromRawEncoding(
                    static_cast<clang::SourceLocation::UIntTy>(cookie));
            }

            clang::DiagnosticsEngine& diags_;
            clang::SourceManager& sources_;
            const clang::CodeGenOptions& options_;
            FunctionLocations functions_;
        };
    } // namespace

    BackendDiagnostics::BackendDiagnostics(llvm::LLVMContext& context,
                                           clang::DiagnosticsEngine& diagnostics,
                                           clang::SourceManager& sources,
                                           const clang::LangOptions& language,
                                           const clang::CodeGenOptions& options,
                                           FunctionLocations functions)
        : context_(context), diagnostics_(diagnostics), previous_(context.getDiagnosticHandler())
    {
        diagnostics_.getClient()->BeginSourceFile(language);
        context_.setDiagnosticHandler(std::make_unique<ClangBackendHandler>(
            diagnostics, sources, options, std::move(functions)));
    }

    BackendDiagnostics::~BackendDiagnostics()
    {
        context_.setDiagnosticHandler(std::move(previous_));
        diagnostics_.getClient()->EndSourceFile();
    }
} // namespace compilerlib::emit
