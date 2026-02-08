#include "compilerlib/frontend/optnone_action.hpp"

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Basic/SourceManager.h>

namespace compilerlib::frontend
{
    namespace
    {
        bool shouldAnnotate(const clang::FunctionDecl& decl, clang::ASTContext& ctx)
        {
            clang::SourceManager& sm = ctx.getSourceManager();
            clang::SourceLocation loc = sm.getSpellingLoc(decl.getLocation());
            if (loc.isInvalid())
                return false;
            if (sm.isInSystemHeader(loc) || sm.isInSystemMacro(loc))
                return false;
            return true;
        }

        class OptNoneConsumer : public clang::ASTConsumer
        {
          public:
            explicit OptNoneConsumer(clang::ASTContext& ctx) : ctx_(ctx) {}

            bool HandleTopLevelDecl(clang::DeclGroupRef decls) override
            {
                for (clang::Decl* decl : decls)
                {
                    annotateDecl(decl);
                }
                return true;
            }

            void HandleInlineFunctionDefinition(clang::FunctionDecl* decl) override
            {
                annotateFunction(decl);
            }

          private:
            void annotateDecl(clang::Decl* decl)
            {
                if (!decl)
                    return;
                if (auto* fn = clang::dyn_cast<clang::FunctionDecl>(decl))
                {
                    annotateFunction(fn);
                }
            }

            void annotateFunction(clang::FunctionDecl* decl)
            {
                if (!decl || !decl->hasBody() || decl->isImplicit())
                    return;
                if (!shouldAnnotate(*decl, ctx_))
                    return;
                if (!decl->hasAttr<clang::OptimizeNoneAttr>())
                {
                    decl->addAttr(clang::OptimizeNoneAttr::CreateImplicit(ctx_));
                }
                if (!decl->hasAttr<clang::NoInlineAttr>())
                {
                    decl->addAttr(clang::NoInlineAttr::CreateImplicit(ctx_));
                }
            }

            clang::ASTContext& ctx_;
        };
    } // namespace

    std::unique_ptr<clang::ASTConsumer> makeOptNoneConsumer(clang::ASTContext& ctx)
    {
        return std::make_unique<OptNoneConsumer>(ctx);
    }

} // namespace compilerlib::frontend
