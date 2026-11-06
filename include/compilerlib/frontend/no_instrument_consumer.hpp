// SPDX-License-Identifier: Apache-2.0
#ifndef COMPILERLIB_FRONTEND_NO_INSTRUMENT_CONSUMER_HPP
#define COMPILERLIB_FRONTEND_NO_INSTRUMENT_CONSUMER_HPP

#include <memory>

namespace clang
{
    class ASTConsumer;
    class ASTContext;
} // namespace clang

namespace compilerlib::frontend
{

    // Clang leaves no trace of no_instrument_function in the IR. This consumer gives every
    // function declared with it the disable_sanitizer_instrumentation attribute, which clang
    // does carry to the IR, where the instrumentation passes honour it.
    std::unique_ptr<clang::ASTConsumer> makeNoInstrumentConsumer(clang::ASTContext& ctx);

} // namespace compilerlib::frontend

#endif // COMPILERLIB_FRONTEND_NO_INSTRUMENT_CONSUMER_HPP
