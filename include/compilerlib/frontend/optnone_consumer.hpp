// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_FRONTEND_OPTNONE_CONSUMER_HPP
#define COMPILERLIB_FRONTEND_OPTNONE_CONSUMER_HPP

#include <memory>

namespace clang
{
    class ASTConsumer;
    class ASTContext;
} // namespace clang

namespace compilerlib::frontend
{

    std::unique_ptr<clang::ASTConsumer> makeOptNoneConsumer(clang::ASTContext& ctx);

} // namespace compilerlib::frontend

#endif // COMPILERLIB_FRONTEND_OPTNONE_CONSUMER_HPP
