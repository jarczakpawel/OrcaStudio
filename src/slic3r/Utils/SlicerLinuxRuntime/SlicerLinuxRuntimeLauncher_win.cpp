#include "SlicerLinuxRuntimeLauncher.hpp"
#include "SlicerLinuxRuntimeConfig.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace Slic3r::SlicerLinuxRuntime {

namespace {

int allocate_loopback_port()
{
    WSADATA data{};
    if (::WSAStartup(MAKEWORD(2, 2), &data) != 0)
        return 0;

    const SOCKET sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        ::WSACleanup();
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(sock);
        ::WSACleanup();
        return 0;
    }

    int len = sizeof(addr);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        ::closesocket(sock);
        ::WSACleanup();
        return 0;
    }

    const int port = ntohs(addr.sin_port);
    ::closesocket(sock);
    ::WSACleanup();
    return port;
}

std::filesystem::path module_dir()
{
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&build_default_launch_spec), &module))
        return {};

    std::wstring path(32768, L'\0');
    const DWORD size = ::GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0)
        return {};
    path.resize(size);
    return std::filesystem::path(path).parent_path();
}

std::string narrow(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int size = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), size);
    return out;
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

std::string normalize_native_text(std::string value)
{
    value.erase(std::remove(value.begin(), value.end(), '\0'), value.end());

    std::string out;
    out.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        const char ch = value[i];
        if (ch == '\r') {
            if (i + 1 < value.size() && value[i + 1] == '\n')
                continue;
            out.push_back('\n');
        } else {
            out.push_back(ch);
        }
    }
    return trim_ascii(out);
}

std::string decode_text_auto(const std::vector<char>& bytes)
{
    if (bytes.empty())
        return {};

    auto from_utf16le = [&](size_t offset) {
        const size_t wchar_count = (bytes.size() - offset) / 2;
        std::wstring ws(wchar_count, L'\0');
        if (wchar_count > 0)
            std::memcpy(ws.data(), bytes.data() + offset, wchar_count * sizeof(wchar_t));
        return normalize_native_text(narrow(ws));
    };

    if (bytes.size() >= 2) {
        const unsigned char b0 = static_cast<unsigned char>(bytes[0]);
        const unsigned char b1 = static_cast<unsigned char>(bytes[1]);
        if (b0 == 0xFFu && b1 == 0xFEu)
            return from_utf16le(2);
        if (b0 == 0xFEu && b1 == 0xFFu) {
            std::wstring ws;
            ws.reserve((bytes.size() - 2) / 2);
            for (size_t i = 2; i + 1 < bytes.size(); i += 2) {
                wchar_t ch = static_cast<wchar_t>((static_cast<unsigned char>(bytes[i]) << 8) | static_cast<unsigned char>(bytes[i + 1]));
                ws.push_back(ch);
            }
            return normalize_native_text(narrow(ws));
        }
    }

    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEFu &&
        static_cast<unsigned char>(bytes[1]) == 0xBBu &&
        static_cast<unsigned char>(bytes[2]) == 0xBFu)
        return normalize_native_text(std::string(bytes.begin() + 3, bytes.end()));

    for (size_t i = 1; i < std::min<size_t>(bytes.size(), 64); i += 2) {
        if (bytes[i] == '\0')
            return from_utf16le(0);
    }

    return normalize_native_text(std::string(bytes.begin(), bytes.end()));
}

std::wstring quote_windows_arg(const std::wstring& value)
{
    if (value.empty())
        return L"\"\"";

    const bool needs_quotes = value.find_first_of(L" \t\n\v\"") != std::wstring::npos;
    if (!needs_quotes)
        return value;

    std::wstring out;
    out.push_back(L'"');
    size_t backslashes = 0;
    for (wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        if (backslashes != 0) {
            out.append(backslashes, L'\\');
            backslashes = 0;
        }
        out.push_back(ch);
    }
    if (backslashes != 0)
        out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::string run_and_capture(const std::wstring& exe_path, const std::vector<std::wstring>& args, DWORD* exit_code = nullptr)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
        if (exit_code)
            *exit_code = static_cast<DWORD>(-1);
        return {};
    }

    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi{};

    std::wstring command_line = quote_windows_arg(exe_path);
    for (const std::wstring& arg : args) {
        command_line.push_back(L' ');
        command_line += quote_windows_arg(arg);
    }

    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');

    const BOOL ok = ::CreateProcessW(exe_path.c_str(),
                                     mutable_command.data(),
                                     nullptr,
                                     nullptr,
                                     TRUE,
                                     CREATE_NO_WINDOW,
                                     nullptr,
                                     nullptr,
                                     &si,
                                     &pi);

    ::CloseHandle(write_pipe);
    write_pipe = nullptr;

    if (!ok) {
        ::CloseHandle(read_pipe);
        if (exit_code)
            *exit_code = static_cast<DWORD>(-1);
        return {};
    }

    std::vector<char> bytes;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || read == 0)
            break;
        bytes.insert(bytes.end(), buffer.data(), buffer.data() + read);
    }

    ::WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD process_exit = static_cast<DWORD>(-1);
    ::GetExitCodeProcess(pi.hProcess, &process_exit);

    ::CloseHandle(read_pipe);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);

    if (exit_code)
        *exit_code = process_exit;
    return decode_text_auto(bytes);
}

std::string to_wsl_path(const std::filesystem::path& p)
{
    const std::wstring ws = p.wstring();
    if (ws.size() >= 2 && ws[1] == L':') {
        std::string tail = narrow(ws.substr(2));
        std::replace(tail.begin(), tail.end(), '\\', '/');
        if (!tail.empty() && tail.front() == '/')
            tail.erase(tail.begin());
        std::string out = "/mnt/";
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ws[0]))));
        out.push_back('/');
        out += tail;
        return out;
    }

    std::string out = narrow(ws);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

std::string required_env(const char* name)
{
    const char* value = std::getenv(name);
    return (value && *value) ? trim_ascii(std::string(value)) : std::string();
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

std::string configured_distro_name(const std::filesystem::path& component_dir)
{
    const auto env_value = required_env("SLICER_LINUX_RUNTIME_WSL_DISTRO");
    if (!env_value.empty())
        return env_value;
    return read_text_file_trimmed(component_dir / windows_wsl_distro_file_name());
}

std::filesystem::path configured_component_cache_dir(const std::filesystem::path& component_dir)
{
    const auto env_value = required_env("SLICER_LINUX_RUNTIME_WINDOWS_COMPONENT_CACHE_DIR");
    if (!env_value.empty())
        return std::filesystem::path(env_value);

    const auto appdata = required_env("APPDATA");
    if (appdata.empty())
        return {};

    const auto subdir_file = component_dir / windows_component_cache_subdir_file_name();
    const auto configured_subdir = read_text_file_trimmed(subdir_file);
    if (!configured_subdir.empty())
        return std::filesystem::path(appdata) / std::filesystem::path(configured_subdir);

    return std::filesystem::path(appdata) / "OrcaSlicer" / "ota" / "plugins";
}

std::string wsl_exe_path()
{
    std::vector<std::filesystem::path> candidates;

    std::wstring system_dir(32768, L'\0');
    const UINT system_size = ::GetSystemDirectoryW(system_dir.data(), static_cast<UINT>(system_dir.size()));
    if (system_size > 0 && system_size < system_dir.size()) {
        system_dir.resize(system_size);
        candidates.emplace_back(std::filesystem::path(system_dir) / L"wsl.exe");
    }

    std::wstring windows_dir(32768, L'\0');
    const UINT windows_size = ::GetWindowsDirectoryW(windows_dir.data(), static_cast<UINT>(windows_dir.size()));
    if (windows_size > 0 && windows_size < windows_dir.size()) {
        windows_dir.resize(windows_size);
        candidates.emplace_back(std::filesystem::path(windows_dir) / L"System32" / L"wsl.exe");
        candidates.emplace_back(std::filesystem::path(windows_dir) / L"Sysnative" / L"wsl.exe");
    }

    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec) && !ec)
            return narrow(candidate.wstring());
    }

    std::wstring found(32768, L'\0');
    const DWORD found_size = ::SearchPathW(nullptr, L"wsl.exe", nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr);
    if (found_size > 0 && found_size < found.size()) {
        found.resize(found_size);
        return narrow(found);
    }

    return {};
}

std::string legacy_windows_wsl_bootstrap_script_file_name()
{
    return "slicer_linux_runtime_wsl_run_host.sh";
}

std::filesystem::path resolve_bootstrap_script_path(const std::filesystem::path& component_dir)
{
    const auto primary = component_dir / windows_wsl_bootstrap_script_file_name();
    if (std::filesystem::exists(primary))
        return primary;

    const auto legacy = component_dir / legacy_windows_wsl_bootstrap_script_file_name();
    if (std::filesystem::exists(legacy))
        return legacy;

    return {};
}

std::string first_missing_runtime_file(const std::filesystem::path& component_dir)
{
    const std::array<std::string, 11> required_files = {{
        host_executable_file_name(),
        std::string("slicer_linux_runtime_host_abi1"),
        std::string("slicer_linux_runtime_host_abi0"),
        std::string("slicer_linux_auth_browser"),
        std::string("run_auth_browser.sh"),
        windows_wsl_import_script_file_name(),
        windows_wsl_distro_file_name(),
        windows_wsl_rootfs_file_name(),
        windows_component_cache_subdir_file_name(),
        std::string("ca-certificates.crt"),
        std::string("slicer_base64.cer")
    }};

    for (const std::string& name : required_files) {
        if (!std::filesystem::exists(component_dir / std::filesystem::path(name)))
            return name;
    }

    if (resolve_bootstrap_script_path(component_dir).empty())
        return windows_wsl_bootstrap_script_file_name();

    return {};
}

bool probe_wsl_ready(const std::string& distro, std::string* reason)
{
    const std::string wsl = wsl_exe_path();
    if (wsl.empty() || !std::filesystem::exists(std::filesystem::u8path(wsl))) {
        if (reason)
            *reason = "Windows Subsystem for Linux (wsl.exe) was not found. Run wsl --install as Administrator, restart Windows, then verify with wsl --status and wsl -l -v";
        return false;
    }

    const std::wstring wsl_w = widen(wsl);

    if (distro.empty()) {
        if (reason)
            reason->clear();
        return true;
    }

    DWORD probe_code = 0;
    const std::string probe_out = run_and_capture(wsl_w, {L"-d", widen(distro), L"--user", L"root", L"--", L"sh", L"-lc", L"true"}, &probe_code);
    if (probe_code == 0) {
        if (reason)
            reason->clear();
        return true;
    }

    std::string message = trim_ascii(probe_out);
    std::string lowered = message;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (lowered.find("there is no distribution with the supplied name") != std::string::npos ||
        lowered.find("wsl_e_distribution_not_found") != std::string::npos ||
        (lowered.find("distribution") != std::string::npos &&
         lowered.find("not") != std::string::npos &&
         lowered.find("found") != std::string::npos)) {
        if (reason)
            *reason = "WSL distro '" + distro + "' is not installed";
        return false;
    }

    if (reason)
        *reason = message.empty() ? ("Failed to start WSL distro '" + distro + "'") : message;
    return false;
}

LaunchSpec error_launch_spec(const std::string& message)
{
    LaunchSpec spec;
    spec.description = "windows runtime preflight error";
    spec.argv = {"cmd.exe", "/C", "echo " + message + " 1>&2 && exit /b 127"};
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

    const std::string wsl = wsl_exe_path();
    if (wsl.empty() || !std::filesystem::exists(std::filesystem::u8path(wsl)))
        return "Windows Subsystem for Linux (wsl.exe) was not found. Run wsl --install as Administrator, restart Windows, then verify with wsl --status and wsl -l -v";

    const auto missing_file = first_missing_runtime_file(component_dir);
    if (!missing_file.empty())
        return "required Windows WSL runtime file missing: " + missing_file;

    const auto distro = configured_distro_name(component_dir);
    if (distro.empty())
        return "SLICER_LINUX_RUNTIME_WSL_DISTRO is not set and slicer_linux_runtime_wsl_distro.txt is missing or empty";

    const auto component_cache_dir = configured_component_cache_dir(component_dir);
    if (component_cache_dir.empty())
        return "Windows plugin cache dir is not configured";

    return {};
}

LaunchSpec build_default_launch_spec()
{
    const std::filesystem::path component_dir = module_dir();
    if (component_dir.empty())
        return error_launch_spec("runtime launcher could not resolve plugin directory");

    const auto missing_file = first_missing_runtime_file(component_dir);
    if (!missing_file.empty())
        return error_launch_spec("required Windows WSL runtime file missing: " + missing_file);

    const std::string distro = configured_distro_name(component_dir);
    if (distro.empty())
        return error_launch_spec("SLICER_LINUX_RUNTIME_WSL_DISTRO is not set and slicer_linux_runtime_wsl_distro.txt is missing or empty");

    const auto component_cache_dir = configured_component_cache_dir(component_dir);
    if (component_cache_dir.empty())
        return error_launch_spec("Windows plugin cache dir is not configured");

    std::string reason;
    if (!probe_wsl_ready(distro, &reason))
        return error_launch_spec(reason.empty() ? "WSL2 runtime is not ready" : reason);

    const auto bootstrap_path = resolve_bootstrap_script_path(component_dir);
    const std::string component_dir_wsl = to_wsl_path(component_dir);
    const std::string plugin_cache_wsl = component_cache_dir.empty() ? std::string() : to_wsl_path(component_cache_dir);
    const std::string bootstrap_wsl = bootstrap_path.empty() ? std::string() : to_wsl_path(bootstrap_path);

    int novnc_port = allocate_loopback_port();
    int vnc_port = allocate_loopback_port();
    if (novnc_port <= 0 || vnc_port <= 0 || novnc_port == vnc_port)
        return error_launch_spec("failed to allocate private loopback ports for Linux browser transport");

    const std::string wsl = wsl_exe_path();
    if (wsl.empty())
        return error_launch_spec("Windows Subsystem for Linux (wsl.exe) was not found");

    LaunchSpec spec;
    spec.description = "windows via explicit WSL2 distro with linux-local runtime bootstrap";
    spec.argv = {
        wsl,
        "-d", distro,
        "--user", "root",
        "--cd", "/",
        "sh", bootstrap_wsl, component_dir_wsl, plugin_cache_wsl,
        std::to_string(novnc_port), std::to_string(novnc_port), std::to_string(vnc_port)
    };
    return spec;
}

}
