// SPDX-License-Identifier: Apache-2.0
//
// Capture tracking, which LLVM 21 redescribed by captured components (address,
// provenance) instead of a capture kind. compilerlib builds against LLVM 16 to 23; the
// differences between versions stay here.
#ifndef COMPILERLIB_INSTRUMENTATION_CAPTURE_COMPAT_HPP
#define COMPILERLIB_INSTRUMENTATION_CAPTURE_COMPAT_HPP

#include <llvm/Analysis/CaptureTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Use.h>
#include <llvm/IR/Value.h>

namespace compilerlib::capture_compat
{
    enum class UseCapture
    {
        // The use does not capture the pointer.
        None,
        // The use does not capture it, but its user's result may be the pointer.
        Passthrough,
        // The use may capture it.
        May,
    };

    // How the use of a pointer captures it. A comparison with null counts as a capture,
    // as no pointer is assumed dereferenceable.
    inline UseCapture useCapture(const llvm::Use& use)
    {
#if LLVM_VERSION_MAJOR >= 21
        const llvm::UseCaptureInfo info = llvm::DetermineUseCaptureKind(use, use.get());
        if (llvm::capturesNothing(info))
            return UseCapture::None;
        return info.isPassthrough() ? UseCapture::Passthrough : UseCapture::May;
#else
        switch (llvm::DetermineUseCaptureKind(use, [](llvm::Value*, const llvm::DataLayout&)
                                              { return false; }))
        {
        case llvm::UseCaptureKind::NO_CAPTURE:
            return UseCapture::None;
        case llvm::UseCaptureKind::PASSTHROUGH:
            return UseCapture::Passthrough;
        case llvm::UseCaptureKind::MAY_CAPTURE:
            break;
        }
        return UseCapture::May;
#endif
    }

    // Whether the pointer may be captured, stores and returns included. LLVM 21 dropped
    // the StoreCaptures parameter, stores always capturing: passing it there would set
    // the number of uses to explore instead.
    inline bool pointerMayBeCaptured(const llvm::Value* pointer)
    {
#if LLVM_VERSION_MAJOR >= 21
        return llvm::PointerMayBeCaptured(pointer, /*ReturnCaptures=*/true);
#else
        return llvm::PointerMayBeCaptured(pointer, /*ReturnCaptures=*/true,
                                          /*StoreCaptures=*/true);
#endif
    }
} // namespace compilerlib::capture_compat

#endif // COMPILERLIB_INSTRUMENTATION_CAPTURE_COMPAT_HPP
