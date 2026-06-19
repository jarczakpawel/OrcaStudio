#include "SlicerLinuxRuntimeLauncher.hpp"
#include "SlicerLinuxRuntimeConfig.hpp"

#include <dlfcn.h>
#include <filesystem>
#include <string>

namespace Slic3r::SlicerLinuxRuntime {

namespace {

std::filesystem::path module_dir()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&build_default_launch_spec), &info) || info.dli_fname == nullptr)
        return {};
    return std::filesystem::path(info.dli_fname).parent_path();
}

}

std::string host_executable_name()
{
    return host_executable_file_name();
}

std::string host_pipe_hint()
{
    return "stdio";
}

std::string launch_preflight_error()
{
    const std::filesystem::path component_dir = module_dir();
    if (component_dir.empty())
        return "runtime launcher could not resolve plugin directory";
    if (!std::filesystem::exists(component_dir / host_executable_file_name()))
        return "linux host executable missing in plugin directory";
    return {};
}

LaunchSpec build_default_launch_spec()
{
    const std::filesystem::path component_dir = module_dir();
    const std::filesystem::path host_path = component_dir / host_executable_file_name();

    LaunchSpec spec;
    spec.description = "linux native host";
    spec.argv = {host_path.string()};
    spec.env = {
        {"SLICER_LINUX_RUNTIME_COMPONENT_DIR", component_dir.string()},
        {"SLICER_LINUX_RUNTIME_COMPONENT_SO", (component_dir / linux_component_library_name()).string()},
        {"SLICER_LINUX_RUNTIME_SOURCE_SO", (component_dir / linux_source_library_name()).string()}
    };
    return spec;
}

}
