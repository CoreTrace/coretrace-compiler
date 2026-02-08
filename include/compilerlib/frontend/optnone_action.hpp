#ifndef COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP
#define COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP

#include <clang/AST/ASTConsumer.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/MultiplexConsumer.h>
#include <llvm/ADT/StringRef.h>

#include <memory>
#include <vector>

namespace compilerlib::frontend
{

    std::unique_ptr<clang::ASTConsumer> makeOptNoneConsumer(clang::ASTContext& ctx);

    template <typename BaseAction> class OptNoneAction : public BaseAction
    {
      public:
        using BaseAction::BaseAction;

      protected:
        std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& CI,
                                                              llvm::StringRef InFile) override
        {
            auto codegen = BaseAction::CreateASTConsumer(CI, InFile);
            if (!codegen)
            {
                return codegen;
            }
            std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
            consumers.push_back(makeOptNoneConsumer(CI.getASTContext()));
            consumers.push_back(std::move(codegen));
            return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
        }
    };

} // namespace compilerlib::frontend

#endif // COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP
