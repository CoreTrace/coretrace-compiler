#include "compilerlib/instrumentation/config.hpp"
#include "compilerlib/attributes.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

namespace compilerlib
{
    namespace
    {

        CT_NODISCARD bool startsWith(const std::string& value, const char* prefix)
        {
            return value.rfind(prefix, 0) == 0;
        }

        void setConfigGlobal(llvm::Module& module, const char* name, int value)
        {
            llvm::LLVMContext& context = module.getContext();
            llvm::Type* intTy = llvm::Type::getInt32Ty(context);
            auto* initializer = llvm::ConstantInt::get(intTy, value);

            llvm::GlobalVariable* global = module.getGlobalVariable(name);
            if (!global)
            {
                global = new llvm::GlobalVariable(
                    module, intTy, false, llvm::GlobalValue::WeakODRLinkage, initializer, name);
            }
            else
            {
                global->setInitializer(initializer);
                global->setLinkage(llvm::GlobalValue::WeakODRLinkage);
            }
            global->setVisibility(llvm::GlobalValue::DefaultVisibility);
        }

        void applyModuleList(RuntimeConfig& config, const std::string& value)
        {
            config.trace_enabled = false;
            config.alloc_enabled = false;
            config.bounds_enabled = false;
            config.vtable_enabled = false;

            size_t start = 0;
            while (start < value.size())
            {
                size_t end = value.find(',', start);
                if (end == std::string::npos)
                {
                    end = value.size();
                }
                std::string token = value.substr(start, end - start);
                size_t first = token.find_first_not_of(" \t");
                if (first == std::string::npos)
                {
                    start = end + 1;
                    continue;
                }
                size_t last = token.find_last_not_of(" \t");
                token = token.substr(first, last - first + 1);

                if (token == "all")
                {
                    config.trace_enabled = true;
                    config.alloc_enabled = true;
                    config.bounds_enabled = true;
                    config.vtable_enabled = true;
                }
                else if (token == "trace")
                {
                    config.trace_enabled = true;
                }
                else if (token == "alloc")
                {
                    config.alloc_enabled = true;
                }
                else if (token == "bounds")
                {
                    config.bounds_enabled = true;
                }
                else if (token == "vtable")
                {
                    config.vtable_enabled = true;
                }
                start = end + 1;
            }
        }

    } // namespace

    void extractRuntimeConfig(const std::vector<std::string>& input,
                              std::vector<std::string>& filtered, RuntimeConfig& config)
    {
        filtered.clear();
        config = RuntimeConfig{};

        for (const auto& arg : input)
        {
            if (arg == "--ct-shadow")
            {
                config.shadow_enabled = true;
                continue;
            }
            if (arg == "--ct-optnone")
            {
                config.optnone_enabled = true;
                continue;
            }
            if (arg == "--ct-no-optnone")
            {
                config.optnone_enabled = false;
                continue;
            }
            if (arg == "--ct-shadow-aggressive")
            {
                config.shadow_enabled = true;
                config.shadow_aggressive = true;
                continue;
            }
            if (startsWith(arg, "--ct-shadow="))
            {
                auto value = arg.substr(std::string("--ct-shadow=").size());
                if (value == "aggressive")
                {
                    config.shadow_enabled = true;
                    config.shadow_aggressive = true;
                }
                continue;
            }
            if (arg == "--ct-bounds-no-abort")
            {
                config.bounds_no_abort = true;
                continue;
            }
            if (startsWith(arg, "--ct-modules="))
            {
                applyModuleList(config, arg.substr(std::string("--ct-modules=").size()));
                continue;
            }
            if (arg == "--ct-no-trace")
            {
                config.trace_enabled = false;
                continue;
            }
            if (arg == "--ct-trace")
            {
                config.trace_enabled = true;
                continue;
            }
            if (arg == "--ct-no-alloc")
            {
                config.alloc_enabled = false;
                continue;
            }
            if (arg == "--ct-alloc")
            {
                config.alloc_enabled = true;
                continue;
            }
            if (arg == "--ct-no-bounds")
            {
                config.bounds_enabled = false;
                continue;
            }
            if (arg == "--ct-bounds")
            {
                config.bounds_enabled = true;
                continue;
            }
            if (arg == "--ct-no-autofree")
            {
                config.autofree_enabled = false;
                continue;
            }
            if (arg == "--ct-autofree")
            {
                config.autofree_enabled = true;
                continue;
            }
            if (arg == "--ct-no-alloc-trace")
            {
                config.alloc_trace_enabled = false;
                continue;
            }
            if (arg == "--ct-alloc-trace")
            {
                config.alloc_trace_enabled = true;
                continue;
            }
            if (arg == "--ct-vcall-trace")
            {
                config.vcall_trace_enabled = true;
                continue;
            }
            if (arg == "--ct-no-vcall-trace")
            {
                config.vcall_trace_enabled = false;
                continue;
            }
            if (arg == "--ct-vtable-diag")
            {
                config.vtable_diag_enabled = true;
                continue;
            }
            if (arg == "--ct-no-vtable-diag")
            {
                config.vtable_diag_enabled = false;
                continue;
            }
            filtered.push_back(arg);
        }

        config.bounds_without_alloc = config.bounds_enabled && !config.alloc_enabled;
    }

    void emitRuntimeConfigGlobals(llvm::Module& module, const RuntimeConfig& config)
    {
        setConfigGlobal(module, "__ct_config_shadow", config.shadow_enabled ? 1 : 0);
        setConfigGlobal(module, "__ct_config_shadow_aggressive", config.shadow_aggressive ? 1 : 0);
        setConfigGlobal(module, "__ct_config_bounds_no_abort", config.bounds_no_abort ? 1 : 0);
        setConfigGlobal(module, "__ct_config_disable_alloc", config.alloc_enabled ? 0 : 1);
        setConfigGlobal(module, "__ct_config_disable_autofree", config.autofree_enabled ? 0 : 1);
        setConfigGlobal(module, "__ct_config_disable_alloc_trace",
                        config.alloc_trace_enabled ? 0 : 1);
        setConfigGlobal(module, "__ct_config_vtable_diag", config.vtable_diag_enabled ? 1 : 0);
    }

} // namespace compilerlib
