// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP
#define COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP

#include "compilerlib/frontend/consumer_action.hpp"
#include "compilerlib/frontend/optnone_consumer.hpp"

namespace compilerlib::frontend
{

    template <typename BaseAction>
    using OptNoneAction = PrependConsumerAction<BaseAction, makeOptNoneConsumer>;

} // namespace compilerlib::frontend

#endif // COMPILERLIB_FRONTEND_OPTNONE_ACTION_HPP
