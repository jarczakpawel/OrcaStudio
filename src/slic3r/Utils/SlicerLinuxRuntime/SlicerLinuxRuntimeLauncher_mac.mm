#include "SlicerLinuxRuntimeLauncher.hpp"
#include "SlicerLinuxRuntimeConfig.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace Slic3r::SlicerLinuxRuntime {

namespace {

std::filesystem::path module_dir()
{
    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&build_default_launch_spec), &info) || info.dli_fname == nullptr)
        return {};
    return std::filesystem::path(info.dli_fname).parent_path();
}

std::string trim_ascii(std::string value)
{
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string read_text_file_trimmed(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    std::string value((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (value.size() >= 3 &&
        static_cast<unsigned char>(value[0]) == 0xEFu &&
        static_cast<unsigned char>(value[1]) == 0xBBu &&
        static_cast<unsigned char>(value[2]) == 0xBFu)
        value.erase(0, 3);

    return trim_ascii(value);
}

std::string required_env(const char* name)
{
    const char* value = std::getenv(name);
    return (value && *value) ? trim_ascii(std::string(value)) : std::string();
}

std::string shell_quote(const std::string& value)
{
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('\'');
    for (char ch : value) {
        if (ch == '\'')
            out += "'\\''";
        else
            out.push_back(ch);
    }
    out.push_back('\'');
    return out;
}

int allocate_loopback_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }
    const int port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

std::filesystem::path runtime_dir()
{
    const auto env_value = required_env("SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR");
    if (!env_value.empty())
        return std::filesystem::path(env_value);

    const auto home = required_env("HOME");
    if (home.empty())
        return {};

    return std::filesystem::path(home) / "Library" / "Application Support" / "BambuStudio_OrcaSlicer" / "slicer-linux-runtime" / "runtime";
}

std::string configured_instance_name(const std::filesystem::path& component_dir)
{
    const auto env_value = required_env("SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE");
    if (!env_value.empty())
        return env_value;
    return read_text_file_trimmed(component_dir / mac_lima_instance_file_name());
}

std::string first_missing_runtime_file(const std::filesystem::path& component_dir,
                                       const std::filesystem::path& runtime_dir_path)
{
    const std::string plugin_required_files[] = {
        mac_host_wrapper_file_name(),
        mac_runtime_install_script_file_name(),
        mac_lima_instance_file_name()
    };

    for (const std::string& name : plugin_required_files) {
        if (!std::filesystem::exists(component_dir / std::filesystem::path(name)))
            return name;
    }

    if (runtime_dir_path.empty())
        return "macOS runtime directory is not resolvable";

    const std::string runtime_required_files[] = {
        host_executable_file_name(),
        std::string("slicer_linux_runtime_host_abi1"),
        std::string("slicer_linux_runtime_host_abi0"),
        std::string("liborcastudio_rosetta_splitlock_compat.so"),
        std::string("slicer_linux_auth_browser"),
        std::string("slicer_linux_auth_browser_x86_64"),
        std::string("slicer_linux_auth_browser_aarch64"),
        std::string("run_auth_browser.sh"),
        std::string("ca-certificates.crt"),
        std::string("slicer_base64.cer"),
        std::string("ld-linux-x86-64.so.2"),
        std::string("libc.so.6"),
        std::string("libm.so.6"),
        std::string("libresolv.so.2"),
        std::string("libnss_dns.so.2"),
        std::string("libnss_files.so.2"),
        std::string("libstdc++.so.6"),
        std::string("libgcc_s.so.1"),
        std::string("libz.so.1")
    };

    for (const std::string& name : runtime_required_files) {
        if (!std::filesystem::exists(runtime_dir_path / std::filesystem::path(name)))
            return name;
    }

    const bool allow_componentless = required_env("SLICER_LINUX_RUNTIME_ALLOW_COMPONENTLESS") == "1";
    if (!allow_componentless) {
        const std::string component_required_files[] = {
            linux_component_library_name(),
            linux_source_library_name()
        };

        for (const std::string& name : component_required_files) {
            if (!std::filesystem::exists(component_dir / std::filesystem::path(name)) &&
                !std::filesystem::exists(runtime_dir_path / std::filesystem::path(name)))
                return name;
        }
    }

    return {};
}

LaunchSpec error_launch_spec(const std::string& message)
{
    LaunchSpec spec;
    spec.description = "mac runtime preflight error";
    spec.argv = {
        "/bin/sh",
        "-lc",
        "printf '%s\\n' " + shell_quote(message) + " 1>&2; exit 127"
    };
    return spec;
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

    const std::filesystem::path runtime_dir_path = runtime_dir();
    const auto missing_file = first_missing_runtime_file(component_dir, runtime_dir_path);
    if (!missing_file.empty())
        return "required macOS runtime file missing: " + missing_file;

    const auto instance = configured_instance_name(component_dir);
    if (instance.empty())
        return "SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE is not set and slicer_linux_runtime_lima_instance.txt is missing or empty";

    return {};
}

LaunchSpec build_default_launch_spec()
{
    const std::filesystem::path component_dir = module_dir();
    if (component_dir.empty())
        return error_launch_spec("runtime launcher could not resolve plugin directory");

    const std::filesystem::path runtime_dir_path = runtime_dir();
    const auto missing_file = first_missing_runtime_file(component_dir, runtime_dir_path);
    if (!missing_file.empty())
        return error_launch_spec("required macOS runtime file missing: " + missing_file);

    const auto instance = configured_instance_name(component_dir);
    if (instance.empty())
        return error_launch_spec("SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE is not set and slicer_linux_runtime_lima_instance.txt is missing or empty");

    const std::filesystem::path wrapper_path = component_dir / mac_host_wrapper_file_name();
    const std::filesystem::path host_path = runtime_dir_path / host_executable_file_name();

    int novnc_port = allocate_loopback_port();
    int vnc_port = allocate_loopback_port();
    if (novnc_port <= 0 || vnc_port <= 0 || novnc_port == vnc_port)
        return error_launch_spec("failed to allocate private loopback ports for Linux browser transport");

    LaunchSpec spec;
    spec.description = "macOS via Lima linux guest";
    spec.argv = {wrapper_path.string(), host_path.string(), runtime_dir_path.string(), component_dir.string()};
    spec.env = {
        {"SLICER_LINUX_RUNTIME_COMPONENT_DIR", runtime_dir_path.string()},
        {"SLICER_LINUX_RUNTIME_COMPONENT_SO", (runtime_dir_path / linux_component_library_name()).string()},
        {"SLICER_LINUX_RUNTIME_SOURCE_SO", (runtime_dir_path / linux_source_library_name()).string()},
        {"SLICER_LINUX_RUNTIME_REQUIRE_LINUX_GUEST", "1"},
        {"SLICER_LINUX_RUNTIME_MAC_RUNTIME_DIR", runtime_dir_path.string()},
        {"SLICER_LINUX_RUNTIME_MAC_LIMA_INSTANCE", instance},
        {"SLICER_LINUX_RUNTIME_AUTH_HOST_NOVNC_PORT", std::to_string(novnc_port)},
        {"SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT", std::to_string(novnc_port)},
        {"SLICER_LINUX_RUNTIME_AUTH_VNC_PORT", std::to_string(vnc_port)}
    };
    return spec;
}

}
