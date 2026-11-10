// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_FRONTEND_NO_INSTRUMENT_ACTION_HPP
#define COMPILERLIB_FRONTEND_NO_INSTRUMENT_ACTION_HPP

#include "compilerlib/frontend/consumer_action.hpp"
#include "compilerlib/frontend/no_instrument_consumer.hpp"

namespace compilerlib::frontend
{

    template <typename BaseAction>
    using NoInstrumentAction = PrependConsumerAction<BaseAction, makeNoInstrumentConsumer>;

} // namespace compilerlib::frontend

#endif // COMPILERLIB_FRONTEND_NO_INSTRUMENT_ACTION_HPP
