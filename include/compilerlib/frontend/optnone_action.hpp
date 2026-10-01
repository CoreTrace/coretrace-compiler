// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP
#define COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP

#include "compilerlib/frontend/consumer_action.hpp"

namespace compilerlib::frontend
{

    std::unique_ptr<clang::ASTConsumer> makeOptNoneConsumer(clang::ASTContext& ctx);

    template <typename BaseAction>
    using OptNoneAction = PrependConsumerAction<BaseAction, makeOptNoneConsumer>;

} // namespace compilerlib::frontend

#endif // COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP
