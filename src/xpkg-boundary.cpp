module mcpplibs.xpkg.executor;

import std;

namespace mcpplibs::xpkg {

std::string_view hook_action_name(HookAction action) {
    switch (action) {
    case HookAction::Load: return "load";
    case HookAction::RunHook: return "run_hook";
    case HookAction::RunScript: return "run_script";
    case HookAction::Elfpatch: return "elfpatch";
    case HookAction::SetLogLevel: return "set_log_level";
    }
    return "";
}

std::optional<HookAction> hook_action_from_string(std::string_view name) {
    for (auto action : {HookAction::Load, HookAction::RunHook, HookAction::RunScript,
                        HookAction::Elfpatch, HookAction::SetLogLevel})
        if (hook_action_name(action) == name) return action;
    return std::nullopt;
}

PackageExecutor::PackageExecutor(fs::path pkg, ExecutionBoundary boundary, HookResponse loaded)
    : pkg_(std::move(pkg)), boundary_(std::move(boundary)), boundaryResponse_(std::move(loaded)) {}

HookResult PackageExecutor::execute_external_(HookAction action, const ExecutionContext& ctx,
                                              HookType hook, std::string_view logLevel) {
    if (!boundaryError_.empty()) return {.success = false, .error = boundaryError_};
    boundaryContext_ = ctx;
    try {
        auto response = boundary_({.action = action, .package = pkg_, .hook = hook,
                                    .context = ctx, .log_level = std::string(logLevel)});
        if (!response) return {.success = false, .error = "hook boundary: " + response.error()};
        // Load is the only source of capabilities: a hook cannot change what
        // the host believes the package supplies by returning different flags.
        response->hooks = boundaryResponse_.hooks;
        boundaryResponse_ = std::move(*response);
        return boundaryResponse_.result;
    } catch (const std::exception& error) {
        return {.success = false, .error = "hook boundary: " + std::string(error.what())};
    } catch (...) {
        return {.success = false, .error = "hook boundary failed"};
    }
}

std::expected<PackageExecutor, std::string>
create_executor(const fs::path& pkg_path, ExecutionBoundary boundary) {
    if (!boundary) return std::unexpected("hook execution boundary is empty");
    try {
        auto loaded = boundary({.action = HookAction::Load, .package = pkg_path});
        if (!loaded) return std::unexpected("hook boundary: " + loaded.error());
        if (!loaded->result.success)
            return std::unexpected("hook boundary load failed: " + loaded->result.error);
        return PackageExecutor(pkg_path, std::move(boundary), std::move(*loaded));
    } catch (const std::exception& error) {
        return std::unexpected("hook boundary: " + std::string(error.what()));
    } catch (...) {
        return std::unexpected("hook boundary load failed");
    }
}

std::expected<HookResponse, std::string> HookWorker::dispatch(const HookInvocation& invocation) {
    if (invocation.action == HookAction::Load) {
        if (executor_) return std::unexpected("hook worker already has a package; start a new worker");
        auto loaded = create_executor(invocation.package);
        if (!loaded) return std::unexpected(loaded.error());
        package_ = invocation.package;
        executor_.emplace(std::move(*loaded));
    } else if (!executor_) {
        return std::unexpected("hook worker must load a package before executing it");
    } else if (invocation.package != package_) {
        return std::unexpected("hook worker refuses a different package");
    }
    HookResponse response;
    switch (invocation.action) {
    case HookAction::Load:
        response.result.success = true;
        for (auto hook : {HookType::Installed, HookType::Build, HookType::Install, HookType::Config, HookType::Uninstall})
            response.hooks[static_cast<std::size_t>(hook)] = executor_->has_hook(hook);
        break;
    case HookAction::RunHook:
        if (static_cast<std::size_t>(invocation.hook) >= response.hooks.size())
            return std::unexpected("hook worker refuses an unknown hook");
        response.result = executor_->run_hook(invocation.hook, invocation.context);
        break;
    case HookAction::RunScript:
        response.result = executor_->run_script(invocation.context);
        break;
    case HookAction::Elfpatch:
        response.result = executor_->apply_elfpatch_auto();
        break;
    case HookAction::SetLogLevel:
        if (invocation.log_level != "debug" && invocation.log_level != "info" && invocation.log_level != "warn"
            && invocation.log_level != "error" && invocation.log_level != "silent")
            return std::unexpected("hook worker refuses an unknown log level");
        executor_->set_log_level(invocation.log_level);
        response.result.success = true;
        break;
    default:
        return std::unexpected("hook worker refuses an unknown action");
    }
    response.xvm_ops = executor_->xvm_operations();
    response.install_requests = executor_->install_requests();
    return response;
}

}  // namespace mcpplibs::xpkg
