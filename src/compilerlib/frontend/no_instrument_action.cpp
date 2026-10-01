// SPDX-License-Identifier: Apache-2.0
#include "compilerlib/frontend/no_instrument_action.hpp"

#include <clang/AST/Attr.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclCXX.h>

namespace compilerlib::frontend
{
    namespace
    {
        class NoInstrumentConsumer : public clang::ASTConsumer
        {
          public:
            explicit NoInstrumentConsumer(clang::ASTContext& ctx) : ctx_(ctx) {}

            bool HandleTopLevelDecl(clang::DeclGroupRef decls) override
            {
                for (clang::Decl* decl : decls)
                {
                    visit(decl);
                }
                return true;
            }

            void HandleInlineFunctionDefinition(clang::FunctionDecl* decl) override
            {
                mark(decl);
            }

          private:
            // A namespace or a linkage specification reaches the consumer whole, with the
            // functions it declares.
            void visit(clang::Decl* decl)
            {
                if (auto* function = llvm::dyn_cast_or_null<clang::FunctionDecl>(decl))
                {
                    mark(function);
                    return;
                }
                if (llvm::isa_and_nonnull<clang::NamespaceDecl>(decl) ||
                    llvm::isa_and_nonnull<clang::LinkageSpecDecl>(decl))
                {
                    for (clang::Decl* inner : llvm::cast<clang::DeclContext>(decl)->decls())
                    {
                        visit(inner);
                    }
                }
            }

            void mark(clang::FunctionDecl* decl)
            {
                if (decl && decl->hasAttr<clang::NoInstrumentFunctionAttr>() &&
                    !decl->hasAttr<clang::DisableSanitizerInstrumentationAttr>())
                {
                    decl->addAttr(clang::DisableSanitizerInstrumentationAttr::CreateImplicit(ctx_));
                }
            }

            clang::ASTContext& ctx_;
        };
    } // namespace

    std::unique_ptr<clang::ASTConsumer> makeNoInstrumentConsumer(clang::ASTContext& ctx)
    {
        return std::make_unique<NoInstrumentConsumer>(ctx);
    }

} // namespace compilerlib::frontend
