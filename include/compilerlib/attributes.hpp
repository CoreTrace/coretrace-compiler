#ifndef COMPILERLIB_ATTRIBUTES_HPP
#define COMPILERLIB_ATTRIBUTES_HPP

#if defined(__cplusplus)
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(nodiscard)
#define CT_NODISCARD [[nodiscard]]
#endif
#endif
#endif

#ifndef CT_NODISCARD
#if defined(__GNUC__) || defined(__clang__)
#define CT_NODISCARD __attribute__((warn_unused_result))
#else
#define CT_NODISCARD
#endif
#endif

#endif // COMPILERLIB_ATTRIBUTES_HPP
