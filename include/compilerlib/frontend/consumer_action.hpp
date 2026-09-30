// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_FRONTEND_CONSUMER_ACTION_HPP
#define COMPILERLIB_FRONTEND_CONSUMER_ACTION_HPP

#include <clang/AST/ASTConsumer.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/MultiplexConsumer.h>
#include <llvm/ADT/StringRef.h>

#include <memory>
#include <vector>

namespace compilerlib::frontend
{

    using ConsumerFactory = std::unique_ptr<clang::ASTConsumer> (*)(clang::ASTContext&);

    // BaseAction, with the consumer MakeConsumer returns seeing every declaration before
    // BaseAction's own consumer, typically code generation, does.
    template <typename BaseAction, ConsumerFactory MakeConsumer>
    class PrependConsumerAction : public BaseAction
    {
      public:
        using BaseAction::BaseAction;

      protected:
        std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& CI,
                                                              llvm::StringRef InFile) override
        {
            auto base = BaseAction::CreateASTConsumer(CI, InFile);
            if (!base)
            {
                return base;
            }
            std::vector<std::unique_ptr<clang::ASTConsumer>> consumers;
            consumers.push_back(MakeConsumer(CI.getASTContext()));
            consumers.push_back(std::move(base));
            return std::make_unique<clang::MultiplexConsumer>(std::move(consumers));
        }
    };

} // namespace compilerlib::frontend

#endif // COMPILERLIB_FRONTEND_CONSUMER_ACTION_HPP
