#include "LinuxRuntimeHost.hpp"

#include "../../shared/slicer_linux_runtime_core/RuntimeCoreJson.hpp"
#include "../../shared/slicer_linux_runtime_core/RuntimeAuthPayload.hpp"
#include "../../src/slic3r/Utils/bambu_networking.hpp"
#include "../../src/slic3r/GUI/Printer/BambuTunnel.h"
#include "../../src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeCompat.hpp"
#include "../../src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.hpp"

#include <dlfcn.h>
#include <curl/curl.h>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <utility>
#include <cstdio>
#include <cstdint>
#include <sstream>
#include <iostream>
#include <limits>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>

using namespace std::chrono_literals;

namespace Slic3r::SlicerLinuxRuntime {

namespace {

std::atomic<Slic3r::SlicerLinuxRuntime::LinuxRuntimeHost*> g_active_host{nullptr};

extern "C" void host_refresh_agora_url(char const* device, char const* dev_ver, char const* channel, void* context, void (*callback)(void* context, char const* url))
{
    auto* host = g_active_host.load();
    std::string url;
    if (host)
        url = host->refresh_camera_url_for_ft(device ? device : "", dev_ver ? dev_ver : "", channel ? channel : "");
    if (callback)
        callback(context, url.c_str());
}

extern "C" {
struct ft_job_result {
    int ec;
    int resp_ec;
    const char* json;
    const void* bin;
    uint32_t bin_size;
};
struct ft_job_msg {
    int kind;
    const char* json;
};
struct FT_TunnelHandle;
struct FT_JobHandle;
typedef int ft_err;
}

using fn_ft_abi_version = int (*)();
using fn_ft_free = void (*)(void *);
using fn_ft_job_result_destroy = void (*)(ft_job_result *);
using fn_ft_job_msg_destroy = void (*)(ft_job_msg *);
using fn_ft_tunnel_create = ft_err (*)(const char *url, FT_TunnelHandle **out);
using fn_ft_tunnel_release = void (*)(FT_TunnelHandle *);
using fn_ft_tunnel_sync_connect = ft_err (*)(FT_TunnelHandle *);
using fn_ft_tunnel_shutdown = ft_err (*)(FT_TunnelHandle *);
using fn_ft_job_create = ft_err (*)(const char *params_json, FT_JobHandle **out);
using fn_ft_job_release = void (*)(FT_JobHandle *);
using fn_ft_job_set_result_cb = ft_err (*)(FT_JobHandle *, void (*)(void *user, ft_job_result result), void *user);
using fn_ft_job_get_result = ft_err (*)(FT_JobHandle *, uint32_t timeout_ms, ft_job_result *out_result);
using fn_ft_tunnel_start_job = ft_err (*)(FT_TunnelHandle *, FT_JobHandle *);
using fn_ft_job_cancel = ft_err (*)(FT_JobHandle *);
using fn_ft_job_set_msg_cb = ft_err (*)(FT_JobHandle *, void (*)(void *user, ft_job_msg msg), void *user);
using fn_ft_job_try_get_msg = ft_err (*)(FT_JobHandle *, ft_job_msg *out_msg);
using fn_ft_job_get_msg = ft_err (*)(FT_JobHandle *, uint32_t timeout_ms, ft_job_msg *out_msg);

bool path_exists(const std::filesystem::path& path)
{
    FILE* f = std::fopen(path.string().c_str(), "rb");
    if (!f)
        return false;
    std::fclose(f);
    return true;
}

bool path_exists_any(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::string env_or(const char* name, const char* fallback)
{
    if (const char* v = std::getenv(name))
        return v;
    return fallback;
}

std::filesystem::path component_path(const char* env_name,
                                     const char* fallback_name,
                                     const std::filesystem::path& component_folder)
{
    const char* configured = std::getenv(env_name);
    std::filesystem::path path = configured != nullptr && *configured != '\0'
        ? std::filesystem::path(configured)
        : std::filesystem::path(fallback_name);
    if (path.is_relative())
        path = component_folder / path;
    return path.lexically_normal();
}

int env_port(const char* name, int fallback)
{
    const std::string value = env_or(name, "");
    if (value.empty())
        return fallback;
    try {
        const int port = std::stoi(value);
        return port > 0 && port <= 65535 ? port : fallback;
    } catch (...) {
        return fallback;
    }
}

std::string auth_metadata_token(const std::string& value,
                                const std::string& fallback,
                                std::size_t max_size,
                                bool allow_dot)
{
    if (value.empty() || value.size() > max_size)
        return fallback;
    for (const unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || (allow_dot && ch == '.'))
            continue;
        return fallback;
    }
    return value;
}

bool fill_secure_random(unsigned char* data, std::size_t count)
{
    std::size_t offset = 0;
    while (offset < count) {
        const ssize_t n = ::getrandom(data + offset, count - offset, 0);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    if (offset == count)
        return true;

    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    while (offset < count) {
        const ssize_t n = ::read(fd, data + offset, count - offset);
        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

std::string random_alnum(std::size_t count)
{
    static constexpr char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
    static constexpr std::size_t alphabet_size = sizeof(alphabet) - 1;
    static constexpr unsigned int accepted_limit = 256U - (256U % alphabet_size);
    std::string out;
    out.reserve(count);
    while (out.size() < count) {
        unsigned char bytes[64];
        if (!fill_secure_random(bytes, sizeof(bytes)))
            throw std::runtime_error("secure random source unavailable");
        for (unsigned char byte : bytes) {
            if (byte >= accepted_limit)
                continue;
            out.push_back(alphabet[byte % alphabet_size]);
            if (out.size() == count)
                break;
        }
    }
    return out;
}

bool command_exists(const char* name)
{
    const char* path = std::getenv("PATH");
    if (!path || !*path)
        return false;
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) dir = ".";
        const auto candidate = std::filesystem::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0)
            return true;
    }
    return false;
}

void write_private_file(const std::filesystem::path& path, const std::string& value)
{
    std::filesystem::create_directories(path.parent_path());
    const auto tmp = path.string() + ".tmp." + std::to_string(::getpid());
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0)
        throw std::runtime_error("open failed: " + tmp + ": " + std::strerror(errno));
    std::size_t off = 0;
    while (off < value.size()) {
        const ssize_t n = ::write(fd, value.data() + off, value.size() - off);
        if (n < 0) {
            const int e = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            throw std::runtime_error("write failed: " + std::string(std::strerror(e)));
        }
        off += static_cast<std::size_t>(n);
    }
    ::fsync(fd);
    ::close(fd);
    std::filesystem::rename(tmp, path);
}

std::string read_text_file(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::string browser_diagnostic(const std::filesystem::path& state_dir)
{
    std::string diagnostic = read_text_file(state_dir / "session.log");
    if (diagnostic.size() > 4096)
        diagnostic.erase(0, diagnostic.size() - 4096);
    diagnostic.erase(std::remove(diagnostic.begin(), diagnostic.end(), '\0'), diagnostic.end());
    return diagnostic;
}

std::string linux_release()
{
    struct utsname u{};
    return ::uname(&u) == 0 ? std::string(u.release) : std::string("unknown");
}

std::string linux_os_version()
{
    const std::string release = linux_release();
    std::vector<unsigned long> parts;
    std::size_t pos = 0;
    while (pos < release.size() && parts.size() < 3) {
        while (pos < release.size() && !std::isdigit(static_cast<unsigned char>(release[pos])))
            ++pos;
        if (pos >= release.size())
            break;
        std::size_t end = pos;
        while (end < release.size() && std::isdigit(static_cast<unsigned char>(release[end])))
            ++end;
        try {
            parts.push_back(std::stoul(release.substr(pos, end - pos)));
        } catch (...) {
            break;
        }
        pos = end;
    }
    while (parts.size() < 3)
        parts.push_back(0);
    return std::to_string(parts[0]) + "." + std::to_string(parts[1]) + "." + std::to_string(parts[2]);
}

std::mutex g_host_log_mutex;

std::string trim_for_log(const std::string& value, std::size_t limit = 8192)
{
    if (value.size() <= limit)
        return value;
    return value.substr(0, limit) + "...<truncated>";
}

std::string lower_ascii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

bool contains_ascii_ci(const std::string& value, const std::string& needle)
{
    return lower_ascii(value).find(lower_ascii(needle)) != std::string::npos;
}

bool sensitive_log_key(const std::string& key)
{
    const std::string k = lower_ascii(key);
    return k.find("token") != std::string::npos ||
           k.find("passwd") != std::string::npos ||
           k.find("password") != std::string::npos ||
           k.find("authkey") != std::string::npos ||
           k == "authorization" ||
           k == "cookie" ||
           k == "set-cookie" ||
           k == "ticket" ||
           k == "license" ||
           k == "access_code" ||
           k == "user_access_code" ||
           k == "sec_link" ||
           k == "http_body" ||
           k == "login_cmd" ||
           k == "logout_cmd" ||
           k == "login_info" ||
           k == "user_info" ||
           k == "user_info_original" ||
           k == "user_info_normalized" ||
           k == "headers" ||
           k == "viewer_url";
}

bool private_identifier_log_key(const std::string& key)
{
    const std::string k = lower_ascii(key);
    return k == "dev_id" ||
           k == "sdev_id" ||
           k == "device" ||
           k == "serial" ||
           k == "sn" ||
           k == "uid" ||
           k == "user_id" ||
           k == "username" ||
           k == "user_name" ||
           k == "user_nickname" ||
           k == "user_avatar" ||
           k == "email";
}

std::string redact_len(const std::string& value)
{
    return "<redacted len=" + std::to_string(value.size()) + ">";
}

std::string mask_identifier(const std::string& value)
{
    if (value.empty())
        return {};
    if (value.size() <= 8)
        return "<id len=" + std::to_string(value.size()) + ">";
    return value.substr(0, 4) + "..." + value.substr(value.size() - 4) + " (len=" + std::to_string(value.size()) + ")";
}

std::string mask_private_ip(const std::string& value)
{
    const auto last_dot = value.rfind('.');
    if (last_dot == std::string::npos)
        return mask_identifier(value);
    return value.substr(0, last_dot + 1) + "x";
}

bool has_url_param_ci(const std::string& value, const std::string& key)
{
    const std::string lower = lower_ascii(value);
    const std::string k = lower_ascii(key);
    return lower.find("?" + k + "=") != std::string::npos ||
           lower.find("&" + k + "=") != std::string::npos;
}

std::string bambu_url_kind_for_log(const std::string& value)
{
    constexpr const char* prefix = "bambu:///";
    if (value.rfind(prefix, 0) != 0)
        return "unknown";

    const std::size_t start = std::strlen(prefix);
    if (start >= value.size())
        return "root";

    if (value.compare(start, 4, "tutk") == 0)
        return "tutk";
    if (value.compare(start, 7, "rtsp___") == 0)
        return "rtsp";
    if (value.compare(start, 8, "local___") == 0)
        return "local";
    return "other";
}

std::string bambu_url_summary(const std::string& value)
{
    const std::string kind = bambu_url_kind_for_log(value);

    return "bambu_url{len=" + std::to_string(value.size()) +
           ",kind=" + kind +
           ",has_uid=" + (has_url_param_ci(value, "uid") ? "1" : "0") +
           ",has_authkey=" + (has_url_param_ci(value, "authkey") ? "1" : "0") +
           ",has_passwd=" + (has_url_param_ci(value, "passwd") ? "1" : "0") +
           ",has_license=" + (has_url_param_ci(value, "license") ? "1" : "0") +
           ",has_token=" + (has_url_param_ci(value, "token") ? "1" : "0") +
           ",has_refresh_url=" + (has_url_param_ci(value, "refresh_url") ? "1" : "0") +
           ",has_device=" + (has_url_param_ci(value, "device") ? "1" : "0") +
           "}";
}

std::string sanitize_log_string(const std::string& key, const std::string& value)
{
    if (sensitive_log_key(key))
        return redact_len(value);
    if (private_identifier_log_key(key))
        return mask_identifier(value);
    if (key == "dev_ip" || key == "ip" || key == "lan_ip")
        return mask_private_ip(value);
    if (value.rfind("bambu:///", 0) == 0)
        return bambu_url_summary(value);
    if (contains_ascii_ci(value, "authkey=") || contains_ascii_ci(value, "passwd=") || contains_ascii_ci(value, "token=") || contains_ascii_ci(value, "access_code"))
        return redact_len(value);
    return value;
}

nlohmann::json sanitize_log_json(const nlohmann::json& value, const std::string& key = {})
{
    if (value.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = value.begin(); it != value.end(); ++it)
            out[it.key()] = sanitize_log_json(it.value(), it.key());
        return out;
    }
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& item : value)
            out.push_back(sanitize_log_json(item, key));
        return out;
    }
    if (value.is_string())
        return sanitize_log_string(key, value.get<std::string>());
    return value;
}


std::string replace_url_param_value(std::string value, const std::string& key, const std::string& replacement)
{
    if (replacement.empty())
        return value;

    const std::string prefix = key + "=";
    std::size_t search = 0;
    for (;;) {
        std::size_t pos = value.find(prefix, search);
        if (pos == std::string::npos)
            return value;

        if (pos == 0 || value[pos - 1] == '?' || value[pos - 1] == '&') {
            pos += prefix.size();
            const std::size_t end = value.find('&', pos);
            value.replace(pos, end == std::string::npos ? std::string::npos : end - pos, replacement);
            return value;
        }

        search = pos + prefix.size();
    }
}


void host_log_json(const std::string& kind, const nlohmann::json& payload)
{
    try {
        nlohmann::json line;
        line["kind"] = kind;
        line["payload"] = sanitize_log_json(payload);
        std::string text = trim_for_log(line.dump());
        std::lock_guard<std::mutex> lock(g_host_log_mutex);
        std::cerr << "[SLRUNTIME] " << text << std::endl;
        if (const char* file = std::getenv("SLICER_LINUX_RUNTIME_HOST_LOG_FILE")) {
            std::ofstream out(file, std::ios::app);
            if (out)
                out << "[SLRUNTIME] " << text << '\n';
        }
    } catch (...) {
    }
}


std::string windows_path_to_wsl(std::string path)
{
    if (path.empty())
        return path;

    if (path.rfind("\\\\?\\", 0) == 0)
        path.erase(0, 4);

    if (path.size() < 3)
        return path;

    const unsigned char drive = static_cast<unsigned char>(path[0]);
    const bool drive_ok =
        (drive >= 'A' && drive <= 'Z') ||
        (drive >= 'a' && drive <= 'z');

    if (!drive_ok || path[1] != ':' || (path[2] != '\\' && path[2] != '/'))
        return path;

    std::string out = "/mnt/";
    out.push_back(static_cast<char>(std::tolower(drive)));
    out.push_back('/');

    for (std::size_t i = 3; i < path.size(); ++i)
        out.push_back(path[i] == '\\' ? '/' : path[i]);

    return out;
}

std::vector<std::string> windows_paths_to_wsl(std::vector<std::string> values)
{
    for (std::string& value : values)
        value = windows_path_to_wsl(std::move(value));
    return values;
}

void translate_print_params_paths(BBL::PrintParams& p)
{
    p.filename = windows_path_to_wsl(std::move(p.filename));
    p.config_filename = windows_path_to_wsl(std::move(p.config_filename));
    p.dst_file = windows_path_to_wsl(std::move(p.dst_file));
}

void translate_publish_params_paths(BBL::PublishParams& p)
{
    p.project_3mf_file = windows_path_to_wsl(std::move(p.project_3mf_file));
    p.config_filename = windows_path_to_wsl(std::move(p.config_filename));
}



struct HttpBuffer {
    std::vector<unsigned char> data;
    std::size_t max_bytes{128U * 1024U * 1024U};
    bool overflow{false};
};

std::size_t curl_write_to_buffer(char* ptr, std::size_t size, std::size_t nmemb, void* userdata)
{
    auto* buffer = static_cast<HttpBuffer*>(userdata);
    if (!buffer || !ptr)
        return 0;
    if (size != 0 && nmemb > (std::numeric_limits<std::size_t>::max() / size))
        return 0;
    const std::size_t bytes = size * nmemb;
    if (bytes > buffer->max_bytes || buffer->data.size() > buffer->max_bytes - bytes) {
        buffer->overflow = true;
        return 0;
    }
    const auto* first = reinterpret_cast<const unsigned char*>(ptr);
    buffer->data.insert(buffer->data.end(), first, first + bytes);
    return bytes;
}

bool valid_http_url(const std::string& url)
{
    const bool https = url.size() >= 9 && url.compare(0, 8, "https://") == 0;
    const bool http = url.size() >= 8 && url.compare(0, 7, "http://") == 0;
    return (https || http) && url.find_first_of("\r\n\0", 0) == std::string::npos;
}

bool valid_https_url(const std::string& url)
{
    return url.size() >= 9 && url.compare(0, 8, "https://") == 0 &&
           url.find_first_of("\r\n\0", 0) == std::string::npos;
}

void ensure_curl_initialized();

bool allowed_bambu_browser_url(const std::string& url)
{
    if (!valid_https_url(url))
        return false;
    ensure_curl_initialized();
    CURLU* parsed = curl_url();
    if (!parsed)
        return false;
    const auto cleanup = [&] { curl_url_cleanup(parsed); };
    if (curl_url_set(parsed, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
        cleanup();
        return false;
    }
    char* scheme_raw = nullptr;
    char* host_raw = nullptr;
    const bool parsed_ok = curl_url_get(parsed, CURLUPART_SCHEME, &scheme_raw, 0) == CURLUE_OK &&
                           curl_url_get(parsed, CURLUPART_HOST, &host_raw, 0) == CURLUE_OK;
    std::string scheme = scheme_raw ? scheme_raw : "";
    std::string host = host_raw ? host_raw : "";
    curl_free(scheme_raw);
    curl_free(host_raw);
    cleanup();
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!parsed_ok || scheme != "https")
        return false;
    while (!host.empty() && host.back() == '.')
        host.pop_back();
    const auto matches = [&host](const std::string& domain) {
        return host == domain || (host.size() > domain.size() &&
            host.compare(host.size() - domain.size(), domain.size(), domain) == 0 &&
            host[host.size() - domain.size() - 1] == '.');
    };
    return matches("bambulab.com") || matches("bambulab.cn") ||
           matches("bambu-lab.com") || matches("makerworld.com");
}

std::string build_bind_ticket_url(const std::string& target, const std::string& ticket)
{
    if (!allowed_bambu_browser_url(target) || ticket.empty())
        return {};
    ensure_curl_initialized();
    CURLU* parsed = curl_url();
    if (!parsed)
        return {};
    if (curl_url_set(parsed, CURLUPART_URL, target.c_str(), 0) != CURLUE_OK) {
        curl_url_cleanup(parsed);
        return {};
    }
    char* scheme_raw = nullptr;
    char* host_raw = nullptr;
    char* port_raw = nullptr;
    const bool ok = curl_url_get(parsed, CURLUPART_SCHEME, &scheme_raw, 0) == CURLUE_OK &&
                    curl_url_get(parsed, CURLUPART_HOST, &host_raw, 0) == CURLUE_OK;
    (void) curl_url_get(parsed, CURLUPART_PORT, &port_raw, 0);
    std::string origin;
    if (ok) {
        origin = std::string(scheme_raw ? scheme_raw : "https") + "://" + (host_raw ? host_raw : "");
        if (port_raw && *port_raw)
            origin += ":" + std::string(port_raw);
        origin += "/";
    }
    curl_free(scheme_raw);
    curl_free(host_raw);
    curl_free(port_raw);
    curl_url_cleanup(parsed);
    if (origin.empty())
        return {};

    CURL* curl = curl_easy_init();
    if (!curl)
        return {};
    char* escaped_target = curl_easy_escape(curl, target.c_str(), static_cast<int>(target.size()));
    char* escaped_ticket = curl_easy_escape(curl, ticket.c_str(), static_cast<int>(ticket.size()));
    std::string out;
    if (escaped_target && escaped_ticket)
        out = origin + "api/sign-in/ticket?to=" + escaped_target + "&ticket=" + escaped_ticket;
    curl_free(escaped_target);
    curl_free(escaped_ticket);
    curl_easy_cleanup(curl);
    return out;
}

bool valid_http_header(const std::string& name, const std::string& value)
{
    if (name.empty() || name.size() > 128 || value.size() > 8192)
        return false;
    if (name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
        return false;
    return std::all_of(name.begin(), name.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '-' || ch == '_';
    });
}

void ensure_curl_initialized()
{
    static std::once_flag once;
    static CURLcode init_result = CURLE_FAILED_INIT;
    std::call_once(once, [] { init_result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (init_result != CURLE_OK)
        throw std::runtime_error("curl_global_init failed");
}

std::string runtime_ca_bundle_path()
{
    const auto valid_file = [](const std::filesystem::path& path) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec) || ec)
            return false;
        const auto size = std::filesystem::file_size(path, ec);
        return !ec && size > 0;
    };

    if (const char* configured = std::getenv("SLICER_LINUX_RUNTIME_CA_BUNDLE"); configured && *configured) {
        const std::filesystem::path path(configured);
        if (valid_file(path))
            return path.string();
    }
    if (const char* component_dir = std::getenv("SLICER_LINUX_RUNTIME_COMPONENT_DIR"); component_dir && *component_dir) {
        const auto path = std::filesystem::path(component_dir) / "ca-certificates.crt";
        if (valid_file(path))
            return path.string();
    }
    if (const char* configured = std::getenv("SSL_CERT_FILE"); configured && *configured) {
        const std::filesystem::path path(configured);
        if (valid_file(path))
            return path.string();
    }
    if (const char* configured = std::getenv("CURL_CA_BUNDLE"); configured && *configured) {
        const std::filesystem::path path(configured);
        if (valid_file(path))
            return path.string();
    }
    for (const auto* candidate : {
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem"}) {
        if (valid_file(candidate))
            return candidate;
    }
    return {};
}

thread_local std::vector<unsigned char> g_thread_request_binary;
thread_local std::vector<unsigned char> g_thread_reply_binary;
constexpr std::size_t kMaxFtPayloadBytes = 1024ULL * 1024ULL * 1024ULL;

struct LoggerCallbackContext {
    LinuxRuntimeHost* host{nullptr};
    std::int64_t tunnel_id{0};
    void (*free_fn)(tchar const*){nullptr};
};

struct StreamInfoCallbackContext {
    LinuxRuntimeHost* host{nullptr};
    std::int64_t tunnel_id{0};
};

struct TrackReporterCallbackContext {
    LinuxRuntimeHost* host{nullptr};
    std::int64_t tunnel_id{0};
};

template <typename T>
void destroy_callback_context(void* pointer)
{
    delete static_cast<T*>(pointer);
}

nlohmann::json stream_info_to_json(const Bambu_StreamInfo* info)
{
    if (!info)
        return nlohmann::json::object();

    nlohmann::json payload{
        {"type", info->type},
        {"sub_type", info->sub_type},
        {"format_type", info->format_type},
        {"format_size", info->format_size},
        {"max_frame_size", info->max_frame_size}
    };
    if (info->type == VIDE) {
        payload.update({
            {"width", info->format.video.width},
            {"height", info->format.video.height},
            {"frame_rate", info->format.video.frame_rate}
        });
    } else {
        payload.update({
            {"sample_rate", info->format.audio.sample_rate},
            {"channel_count", info->format.audio.channel_count},
            {"sample_size", info->format.audio.sample_size}
        });
    }
    if (info->format_buffer && info->format_size > 0 && info->format_size <= 16 * 1024 * 1024) {
        const auto* first = info->format_buffer;
        payload["format_buffer"] = std::vector<unsigned char>(first, first + static_cast<std::size_t>(info->format_size));
    }
    return payload;
}

void logger_callback_forwarder(void* ctx, int level, tchar const* msg) noexcept
{
    auto* context = static_cast<LoggerCallbackContext*>(ctx);
    if (!context)
        return;
    try {
        if (context->host)
            context->host->dispatch_logger_event(context->tunnel_id, level, std::string(msg ? msg : ""));
    } catch (...) {
    }
    if (context->free_fn && msg)
        context->free_fn(msg);
}

void stream_info_callback_forwarder(void* ctx, Bambu_StreamInfo* info) noexcept
{
    auto* context = static_cast<StreamInfoCallbackContext*>(ctx);
    if (!context || !context->host || !info)
        return;
    try {
        context->host->dispatch_stream_info_event(context->tunnel_id, stream_info_to_json(info));
    } catch (...) {
    }
}

void track_reporter_callback_forwarder(void* ctx, const PlayerEventC* event) noexcept
{
    auto* context = static_cast<TrackReporterCallbackContext*>(ctx);
    if (!context || !context->host || !event)
        return;
    try {
        context->host->dispatch_track_event(context->tunnel_id, {
            {"event_name", std::string(event->event_name ? event->event_name : "")},
            {"module", std::string(event->module ? event->module : "")},
            {"phase", std::string(event->phase ? event->phase : "")},
            {"result", std::string(event->result ? event->result : "")},
            {"error_code", std::string(event->error_code ? event->error_code : "")},
            {"error_message", std::string(event->error_message ? event->error_message : "")},
            {"event_data_body", std::string(event->event_data_body ? event->event_data_body : "")}
        });
    } catch (...) {
    }
}


std::string host_arch_string()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "unknown";
#endif
}

void clear_thread_reply_binary()
{
    g_thread_reply_binary.clear();
}

bool copy_ft_job_result_payload(HostFtJobState& state, const ft_job_result& result)
{
    state.result_ec = result.ec;
    state.result_resp_ec = result.resp_ec;
    state.result_json.assign(result.json ? result.json : "");
    state.result_bin.clear();
    if (result.bin_size > kMaxFtPayloadBytes || (result.bin_size != 0 && !result.bin)) {
        state.result_ec = -1;
        state.result_resp_ec = -1;
        state.result_json = "invalid file-transfer payload";
        return false;
    }
    if (result.bin_size) {
        const auto* first = static_cast<const unsigned char*>(result.bin);
        state.result_bin.assign(first, first + result.bin_size);
    }
    return true;
}

void finish_ft_callback(HostFtJobState* state) noexcept
{
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->active_callbacks > 0)
            --state->active_callbacks;
    }
    state->cv.notify_all();
}

std::shared_ptr<HostFtJobState> lookup_ft_job_state(std::map<std::int64_t, std::shared_ptr<HostFtJobState>>& states, std::mutex& mutex, std::int64_t id)
{
    std::lock_guard<std::mutex> lock(mutex);
    auto it = states.find(id);
    if (it == states.end())
        return {};
    return it->second;
}

template <typename T>
HostResourceLease<T> acquire_resource(
    std::map<std::int64_t, std::shared_ptr<HostResource>>& resources,
    std::mutex& map_mutex,
    std::int64_t id)
{
    std::shared_ptr<HostResource> resource;
    {
        std::lock_guard<std::mutex> lock(map_mutex);
        auto it = resources.find(id);
        if (it == resources.end())
            return {};
        resource = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(resource->mutex);
        if (resource->closing || !resource->handle)
            return {};
        ++resource->active_calls;
    }
    return HostResourceLease<T>(std::move(resource));
}

std::shared_ptr<HostResource> detach_resource(
    std::map<std::int64_t, std::shared_ptr<HostResource>>& resources,
    std::mutex& map_mutex,
    std::int64_t id)
{
    std::shared_ptr<HostResource> resource;
    {
        std::lock_guard<std::mutex> lock(map_mutex);
        auto it = resources.find(id);
        if (it == resources.end())
            return {};
        resource = it->second;
        resources.erase(it);
    }
    {
        std::lock_guard<std::mutex> lock(resource->mutex);
        resource->closing = true;
    }
    return resource;
}

void* wait_and_take_resource(const std::shared_ptr<HostResource>& resource)
{
    if (!resource)
        return nullptr;
    std::unique_lock<std::mutex> lock(resource->mutex);
    resource->cv.wait(lock, [&] { return resource->active_calls == 0; });
    void* handle = resource->handle;
    resource->handle = nullptr;
    return handle;
}

std::shared_ptr<HostResource> make_host_resource(void* handle)
{
    auto resource = std::make_shared<HostResource>();
    resource->handle = handle;
    return resource;
}

std::map<std::string, std::string> json_to_string_map(const nlohmann::json& j)
{
    std::map<std::string, std::string> out;
    if (!j.is_object())
        return out;
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_string())
            out[it.key()] = it.value().get<std::string>();
        else
            out[it.key()] = it.value().dump();
    }
    return out;
}

std::map<std::string, std::map<std::string, std::string>> json_to_nested_string_map(const nlohmann::json& j)
{
    std::map<std::string, std::map<std::string, std::string>> out;
    if (!j.is_object())
        return out;
    for (auto it = j.begin(); it != j.end(); ++it)
        out[it.key()] = json_to_string_map(it.value());
    return out;
}

nlohmann::json nested_string_map_to_json(const std::map<std::string, std::map<std::string, std::string>>& value)
{
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [k, inner] : value)
        out[k] = inner;
    return out;
}

BBL::TaskQueryParams task_query_from_json(const nlohmann::json& j)
{
    BBL::TaskQueryParams p{};
    p.dev_id = j.value("dev_id", std::string());
    p.status = j.value("status", 0);
    p.offset = j.value("offset", 0);
    p.limit = j.value("limit", 20);
    return p;
}

BBL::PrintParams print_params_from_json(const nlohmann::json& j)
{
    BBL::PrintParams p{};
    p.dev_id = j.value("dev_id", std::string());
    p.task_name = j.value("task_name", std::string());
    p.project_name = j.value("project_name", std::string());
    p.preset_name = j.value("preset_name", std::string());
    p.filename = j.value("filename", std::string());
    p.config_filename = j.value("config_filename", std::string());
    p.plate_index = j.value("plate_index", 0);
    p.ftp_folder = j.value("ftp_folder", std::string());
    p.ftp_file = j.value("ftp_file", std::string());
    p.ftp_file_md5 = j.value("ftp_file_md5", std::string());
    p.nozzle_mapping = j.value("nozzle_mapping", std::string());
    p.ams_mapping = j.value("ams_mapping", std::string());
    p.ams_mapping2 = j.value("ams_mapping2", std::string());
    p.ams_mapping_info = j.value("ams_mapping_info", std::string());
    p.nozzles_info = j.value("nozzles_info", std::string());
    p.connection_type = j.value("connection_type", std::string());
    p.comments = j.value("comments", std::string());
    p.origin_profile_id = j.value("origin_profile_id", 0);
    p.stl_design_id = j.value("stl_design_id", 0);
    p.origin_model_id = j.value("origin_model_id", std::string());
    p.print_type = j.value("print_type", std::string());
    p.dst_file = j.value("dst_file", std::string());
    p.dev_name = j.value("dev_name", std::string());
    p.dev_ip = j.value("dev_ip", std::string());
    p.use_ssl_for_ftp = j.value("use_ssl_for_ftp", false);
    p.use_ssl_for_mqtt = j.value("use_ssl_for_mqtt", false);
    p.username = j.value("username", std::string());
    p.password = j.value("password", std::string());
    p.task_bed_leveling = j.value("task_bed_leveling", false);
    p.task_flow_cali = j.value("task_flow_cali", false);
    p.task_vibration_cali = j.value("task_vibration_cali", false);
    p.task_layer_inspect = j.value("task_layer_inspect", false);
    p.task_record_timelapse = j.value("task_record_timelapse", false);
    p.task_timelapse_use_internal = j.value("task_timelapse_use_internal", false);
    p.task_use_ams = j.value("task_use_ams", false);
    p.task_bed_type = j.value("task_bed_type", std::string());
    p.extra_options = j.value("extra_options", std::string());
    p.auto_bed_leveling = j.value("auto_bed_leveling", 0);
    p.auto_flow_cali = j.value("auto_flow_cali", 0);
    p.auto_offset_cali = j.value("auto_offset_cali", 0);
    p.extruder_cali_manual_mode = j.value("extruder_cali_manual_mode", -1);
    p.task_ext_change_assist = j.value("task_ext_change_assist", false);
    p.try_emmc_print = j.value("try_emmc_print", false);
    p.svc_context = j.value("svc_context", std::string());
    translate_print_params_paths(p);
    return p;
}

template <typename Value>
struct AsyncCallbackState {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready{false};
    Value value{};
};

template <typename Invoke>
nlohmann::json wait_string_callback(Invoke&& invoke)
{
    auto state = std::make_shared<AsyncCallbackState<std::string>>();
    const int ret = invoke([state](std::string value) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->value = std::move(value);
            state->ready = true;
        }
        state->cv.notify_one();
    });
    if (ret != 0)
        return {{"ok", true}, {"value", ret}};
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, 120s, [state] { return state->ready; }))
        return {{"ok", true}, {"value", BAMBU_NETWORK_ERR_TIMEOUT}};
    return {{"ok", true}, {"value", 0}, {"result", state->value}};
}

struct StringIntCallbackValue {
    std::string result;
    int status{0};
};

template <typename Invoke>
nlohmann::json wait_string_int_callback(Invoke&& invoke)
{
    auto state = std::make_shared<AsyncCallbackState<StringIntCallbackValue>>();
    const int ret = invoke([state](std::string value, int status) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->value.result = std::move(value);
            state->value.status = status;
            state->ready = true;
        }
        state->cv.notify_one();
    });
    if (ret != 0)
        return {{"ok", true}, {"value", ret}};
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, 120s, [state] { return state->ready; }))
        return {{"ok", true}, {"value", BAMBU_NETWORK_ERR_TIMEOUT}};
    return {{"ok", true}, {"value", 0}, {"result", state->value.result}, {"status", state->value.status}};
}

template <typename Invoke>
nlohmann::json wait_model_task_callback(Invoke&& invoke)
{
    auto state = std::make_shared<AsyncCallbackState<nlohmann::json>>();
    state->value = nlohmann::json::object();
    const int ret = invoke([state](Slic3r::BBLModelTask* value) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->value = model_task_to_json(value);
            state->ready = true;
        }
        state->cv.notify_one();
    });
    if (ret != 0)
        return {{"ok", true}, {"value", ret}};
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, 120s, [state] { return state->ready; }))
        return {{"ok", true}, {"value", BAMBU_NETWORK_ERR_TIMEOUT}};
    return {{"ok", true}, {"value", 0}, {"subtask", state->value}};
}

}

LinuxRuntimeHost::LinuxRuntimeHost()
{
    g_active_host.store(this);
    load_modules();
    ensure_main_dispatcher();
}

LinuxRuntimeHost::~LinuxRuntimeHost()
{
    begin_shutdown();
    stop_main_dispatcher();
    cleanup_resources();
    if (g_active_host.load() == this)
        g_active_host.store(nullptr);
}

void LinuxRuntimeHost::begin_shutdown()
{
    if (m_shutting_down.exchange(true))
        return;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        for (auto& [id, job] : m_jobs) {
            if (!job)
                continue;
            job->cancel_requested = true;
            {
                std::lock_guard<std::mutex> wait_lock(job->wait_mutex);
                job->wait_reply_ready = true;
                job->wait_reply_value = false;
            }
            job->wait_cv.notify_all();
        }
        for (auto& [id, reply] : m_callback_replies) {
            if (!reply)
                continue;
            {
                std::lock_guard<std::mutex> reply_lock(reply->mutex);
                reply->ready = true;
                reply->string_value.clear();
            }
            reply->cv.notify_all();
        }
        for (auto& [id, state] : m_ft_job_states) {
            if (!state)
                continue;
            {
                std::lock_guard<std::mutex> state_lock(state->mutex);
                state->shutting_down = true;
            }
            state->cv.notify_all();
        }
    }

    std::vector<std::shared_ptr<HostHttpJobState>> http_jobs;
    {
        std::lock_guard<std::mutex> lock(m_http_jobs_mutex);
        for (auto& [id, state] : m_http_jobs) {
            if (state) {
                state->cancel_requested.store(true, std::memory_order_release);
                http_jobs.push_back(state);
            }
        }
        m_http_jobs.clear();
    }
    for (auto& state : http_jobs) {
        if (state->worker.joinable())
            state->worker.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_auth_mutex);
        stop_auth_browser_process();
    }
}

void LinuxRuntimeHost::cleanup_resources() noexcept
{
    std::map<std::int64_t, std::shared_ptr<HostResource>> agents;
    std::map<std::int64_t, std::shared_ptr<HostResource>> tunnels;
    std::map<std::int64_t, std::shared_ptr<HostResource>> ft_tunnels;
    std::map<std::int64_t, std::shared_ptr<HostResource>> ft_jobs;
    std::map<std::int64_t, std::shared_ptr<HostFtJobState>> ft_job_states;
    std::map<std::int64_t, void*> logger_contexts;
    std::map<std::int64_t, void*> stream_info_contexts;
    std::map<std::int64_t, void*> track_reporter_contexts;
    std::vector<HostRetiredCallbackContext> retired_contexts;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        agents.swap(m_agents);
        tunnels.swap(m_tunnels);
        ft_tunnels.swap(m_ft_tunnels);
        ft_jobs.swap(m_ft_jobs);
        ft_job_states.swap(m_ft_job_states);
        logger_contexts.swap(m_logger_contexts);
        stream_info_contexts.swap(m_stream_info_contexts);
        track_reporter_contexts.swap(m_track_reporter_contexts);
        retired_contexts.swap(m_retired_callback_contexts);
        m_country_codes.clear();
        m_jobs.clear();
        m_callback_replies.clear();
    }

    auto source_symbol = [this](const char* name) -> void* {
        return m_source ? dlsym(m_source, name) : nullptr;
    };
    auto component_symbol = [this](const char* name) -> void* {
        return m_component ? dlsym(m_component, name) : nullptr;
    };

    const auto set_logger = reinterpret_cast<void (*)(Bambu_Tunnel, Logger, void*)>(source_symbol("Bambu_SetLogger"));
    const auto set_stream_info = reinterpret_cast<void (*)(Bambu_Tunnel, StreamInfoCallback, void*)>(source_symbol("Bambu_SetStreamInfoCallback"));
    const auto set_track_reporter = reinterpret_cast<void (*)(Bambu_Tunnel, TrackReporter, void*)>(source_symbol("Bambu_SetTrackReporter"));
    const auto close_tunnel = reinterpret_cast<void (*)(Bambu_Tunnel)>(source_symbol("Bambu_Close"));
    const auto destroy_tunnel = reinterpret_cast<void (*)(Bambu_Tunnel)>(source_symbol("Bambu_Destroy"));

    for (auto& [id, resource] : tunnels) {
        auto tunnel = static_cast<Bambu_Tunnel>(wait_and_take_resource(resource));
        if (!tunnel)
            continue;
        if (set_logger)
            set_logger(tunnel, nullptr, nullptr);
        if (set_stream_info)
            set_stream_info(tunnel, nullptr, nullptr);
        if (set_track_reporter)
            set_track_reporter(tunnel, nullptr, nullptr);
        if (close_tunnel)
            close_tunnel(tunnel);
        if (destroy_tunnel)
            destroy_tunnel(tunnel);
    }

    const auto clear_result = reinterpret_cast<fn_ft_job_set_result_cb>(component_symbol("ft_job_set_result_cb"));
    const auto clear_msg = reinterpret_cast<fn_ft_job_set_msg_cb>(component_symbol("ft_job_set_msg_cb"));
    const auto cancel_job = reinterpret_cast<fn_ft_job_cancel>(component_symbol("ft_job_cancel"));
    const auto release_job = reinterpret_cast<fn_ft_job_release>(component_symbol("ft_job_release"));
    for (auto& [id, resource] : ft_jobs) {
        auto* job = static_cast<FT_JobHandle*>(wait_and_take_resource(resource));
        if (!job)
            continue;
        auto state_it = ft_job_states.find(id);
        auto state = state_it != ft_job_states.end() ? state_it->second : nullptr;
        if (state) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->shutting_down = true;
            }
            state->cv.notify_all();
        }
        if (clear_result && state && state->result_callback_enabled)
            (void) clear_result(job, nullptr, nullptr);
        if (clear_msg && state && state->msg_callback_enabled)
            (void) clear_msg(job, nullptr, nullptr);
        if (cancel_job)
            (void) cancel_job(job);
        if (state) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&state] { return state->active_callbacks == 0; });
        }
        if (release_job)
            release_job(job);
    }

    const auto shutdown_ft_tunnel = reinterpret_cast<fn_ft_tunnel_shutdown>(component_symbol("ft_tunnel_shutdown"));
    const auto release_ft_tunnel = reinterpret_cast<fn_ft_tunnel_release>(component_symbol("ft_tunnel_release"));
    for (auto& [id, resource] : ft_tunnels) {
        auto* tunnel = static_cast<FT_TunnelHandle*>(wait_and_take_resource(resource));
        if (!tunnel)
            continue;
        if (shutdown_ft_tunnel)
            (void) shutdown_ft_tunnel(tunnel);
        if (release_ft_tunnel)
            release_ft_tunnel(tunnel);
    }

    const auto destroy_agent = reinterpret_cast<int (*)(void*)>(component_symbol("bambu_network_destroy_agent"));
    if (destroy_agent) {
        for (auto& [id, resource] : agents) {
            void* agent = wait_and_take_resource(resource);
            if (agent)
                (void) destroy_agent(agent);
        }
    }

    const auto deinit_source = reinterpret_cast<void (*)()>(source_symbol("Bambu_Deinit"));
    if (deinit_source)
        deinit_source();

    for (auto& [id, pointer] : logger_contexts)
        destroy_callback_context<LoggerCallbackContext>(pointer);
    for (auto& [id, pointer] : stream_info_contexts)
        destroy_callback_context<StreamInfoCallbackContext>(pointer);
    for (auto& [id, pointer] : track_reporter_contexts)
        destroy_callback_context<TrackReporterCallbackContext>(pointer);
    for (auto& context : retired_contexts) {
        if (context.pointer && context.destroy)
            context.destroy(context.pointer);
    }

    if (m_source) {
        dlclose(m_source);
        m_source = nullptr;
    }
    if (m_component) {
        dlclose(m_component);
        m_component = nullptr;
    }
    m_source_status = "not_loaded";
    m_component_status = "not_loaded";
    m_component_actual_abi_version.clear();

    {
        std::lock_guard<std::mutex> lock(m_events_mutex);
        m_events.clear();
    }
}

void LinuxRuntimeHost::dispatch_logger_event(std::int64_t tunnel_handle, int level, const std::string& message)
{
    queue_tunnel_event(tunnel_handle, "logger", {{"level", level}, {"message", message}});
}

void LinuxRuntimeHost::dispatch_stream_info_event(std::int64_t tunnel_handle, const nlohmann::json& payload)
{
    queue_tunnel_event(tunnel_handle, "stream_info", payload);
}

void LinuxRuntimeHost::dispatch_track_event(std::int64_t tunnel_handle, const nlohmann::json& payload)
{
    queue_tunnel_event(tunnel_handle, "track_event", payload);
}

std::string LinuxRuntimeHost::refresh_camera_url_for_ft(const std::string& device, const std::string& dev_ver, const std::string& channel)
{
    auto f = net<int (*)(void*, std::string, std::function<void(std::string)>)>("bambu_network_get_camera_url");
    if (!f)
        return {};
    std::int64_t agent_id = 0;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_agents.empty())
            agent_id = m_agents.begin()->first;
    }
    auto agent = acquire_resource<void*>(m_agents, m_state_mutex, agent_id);
    if (!agent)
        return {};
    auto state = std::make_shared<AsyncCallbackState<std::string>>();
    std::string dev_arg = device + "|" + dev_ver + "|\"agora\"|" + channel;
    const int ret = f(agent, dev_arg, [state](std::string value) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->value = std::move(value);
            state->ready = true;
        }
        state->cv.notify_one();
    });
    if (ret != 0)
        return {};
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, 20s, [state] { return state->ready; }))
        return {};
    return state->value;
}

std::string LinuxRuntimeHost::refresh_agora_url_ptr_string() const
{
    const auto value = reinterpret_cast<std::uintptr_t>(&host_refresh_agora_url);
    std::ostringstream ss;
    ss << std::hex << value;
    return ss.str();
}

void LinuxRuntimeHost::ensure_main_dispatcher()
{
    if (m_main_dispatcher.joinable())
        return;
    m_stop_main_dispatcher = false;
    m_main_dispatcher = std::thread([this] { main_dispatch_loop(); });
}

void LinuxRuntimeHost::stop_main_dispatcher()
{
    m_stop_main_dispatcher = true;
    m_main_tasks_cv.notify_all();
    if (m_main_dispatcher.joinable())
        m_main_dispatcher.join();
}

void LinuxRuntimeHost::queue_main_task(std::function<void()> fn)
{
    if (!fn || m_shutting_down.load(std::memory_order_acquire))
        return;
    {
        std::lock_guard<std::mutex> lock(m_main_tasks_mutex);
        m_main_tasks.push_back(std::move(fn));
    }
    m_main_tasks_cv.notify_one();
}

void LinuxRuntimeHost::main_dispatch_loop()
{
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lock(m_main_tasks_mutex);
            m_main_tasks_cv.wait(lock, [this] { return m_stop_main_dispatcher.load() || !m_main_tasks.empty(); });
            if (m_stop_main_dispatcher.load() && m_main_tasks.empty())
                break;
            fn = std::move(m_main_tasks.front());
            m_main_tasks.pop_front();
        }
        try {
            fn();
        } catch (...) {
        }
    }
}

void LinuxRuntimeHost::set_thread_request_binary(std::vector<unsigned char> data)
{
    g_thread_request_binary = std::move(data);
}

bool LinuxRuntimeHost::consume_thread_reply_binary(std::vector<unsigned char>& out)
{
    if (g_thread_reply_binary.empty())
        return false;
    out = std::move(g_thread_reply_binary);
    g_thread_reply_binary.clear();
    return true;
}


void LinuxRuntimeHost::load_modules()
{
    std::lock_guard<std::recursive_mutex> module_lock(m_module_mutex);
    if (m_module_load_attempted)
        return;
    m_module_load_attempted = true;

    const std::filesystem::path component_folder = std::filesystem::path(env_or("SLICER_LINUX_RUNTIME_COMPONENT_DIR", "."));
    std::string manifest_reason;
    const bool have_manifest = path_exists(linux_component_manifest_path(component_folder));
    const bool manifest_ok = !have_manifest || validate_linux_component_set_against_manifest(component_folder, &manifest_reason);

    if (!manifest_ok) {
        m_component_status = "manifest invalid: " + manifest_reason;
        m_source_status = "manifest invalid: " + manifest_reason;
    }

    if (!m_component) {
        const auto path = component_path("SLICER_LINUX_RUNTIME_COMPONENT_SO",
                                         "libbambu_networking.so",
                                         component_folder);
        std::string reason;
        if (!manifest_ok) {
            m_component_status = "manifest invalid: " + manifest_reason;
        } else if (!validate_linux_component_file(path.string(), &reason)) {
            m_component_status = "validate failed for " + path.string() + ": " + reason;
        } else {
            m_component = dlopen(path.c_str(), RTLD_LAZY);
            if (!m_component) {
                const char* err = dlerror();
                m_component_status = err && *err ? std::string("dlopen failed: ") + err : "dlopen failed";
            } else {
                using get_version_fn = std::string (*)();
                auto gv = reinterpret_cast<get_version_fn>(dlsym(m_component, "bambu_network_get_version"));
                if (gv) {
                    std::string abi_reason;
                    const auto actual = gv();
                    m_component_actual_abi_version = actual;
                    if (!abi_version_matches_expected(actual, &abi_reason)) {
                        dlclose(m_component);
                        m_component = nullptr;
                        m_component_status = abi_reason;
                    } else {
                        m_component_status = "loaded";
                    }
                } else {
                    m_component_actual_abi_version.clear();
                    m_component_status = "loaded";
                }
            }
        }
    } else {
        m_component_status = "loaded";
    }

    if (!m_source) {
        const auto path = component_path("SLICER_LINUX_RUNTIME_SOURCE_SO",
                                         "libBambuSource.so",
                                         component_folder);
        std::string reason;
        if (!manifest_ok) {
            m_source_status = "manifest invalid: " + manifest_reason;
        } else if (!validate_linux_component_file(path.string(), &reason)) {
            m_source_status = "validate failed for " + path.string() + ": " + reason;
        } else {
            m_source = dlopen(path.c_str(), RTLD_LAZY);
            if (!m_source) {
                const char* err = dlerror();
                m_source_status = err && *err ? std::string("dlopen failed: ") + err : "dlopen failed";
            } else {
                m_source_status = "loaded";
            }
        }
    } else {
        m_source_status = "loaded";
    }
}

void* LinuxRuntimeHost::resolve_component(const char* name)
{
    std::lock_guard<std::recursive_mutex> module_lock(m_module_mutex);
    load_modules();
    return m_component ? dlsym(m_component, name) : nullptr;
}

void* LinuxRuntimeHost::resolve_source(const char* name)
{
    std::lock_guard<std::recursive_mutex> module_lock(m_module_mutex);
    load_modules();
    return m_source ? dlsym(m_source, name) : nullptr;
}

bool LinuxRuntimeHost::has_component_symbol(const char* name)
{
    std::lock_guard<std::recursive_mutex> module_lock(m_module_mutex);
    load_modules();
    return m_component && dlsym(m_component, name) != nullptr;
}

nlohmann::json LinuxRuntimeHost::auth_capabilities() const
{
    auto self = const_cast<LinuxRuntimeHost*>(this);
    std::lock_guard<std::recursive_mutex> module_lock(m_module_mutex);
    self->load_modules();
    return {
        {"component_loaded", self->m_component != nullptr},
        {"network_loaded", self->m_component != nullptr},
        {"source_loaded", self->m_source != nullptr},
        {"bambu_network_is_user_login", self->has_component_symbol("bambu_network_is_user_login")},
        {"bambu_network_get_user_id", self->has_component_symbol("bambu_network_get_user_id")},
        {"bambu_network_get_user_name", self->has_component_symbol("bambu_network_get_user_name")},
        {"bambu_network_get_user_avatar", self->has_component_symbol("bambu_network_get_user_avatar")},
        {"bambu_network_get_user_nickanme", self->has_component_symbol("bambu_network_get_user_nickanme")},
        {"bambu_network_build_login_cmd", self->has_component_symbol("bambu_network_build_login_cmd")},
        {"bambu_network_build_logout_cmd", self->has_component_symbol("bambu_network_build_logout_cmd")},
        {"bambu_network_build_login_info", self->has_component_symbol("bambu_network_build_login_info")},
        {"bambu_network_change_user", self->has_component_symbol("bambu_network_change_user")},
        {"bambu_network_get_my_profile", self->has_component_symbol("bambu_network_get_my_profile")},
        {"bambu_network_get_my_message", self->has_component_symbol("bambu_network_get_my_message")},
        {"bambu_network_get_my_token", self->has_component_symbol("bambu_network_get_my_token")}
    };
}


nlohmann::json LinuxRuntimeHost::auth_browser_capabilities() const
{
    const std::filesystem::path component_dir = env_or("SLICER_LINUX_RUNTIME_COMPONENT_DIR", ".");
    const std::string configured_browser = env_or("SLICER_LINUX_RUNTIME_AUTH_BROWSER", "");
    const auto browser = configured_browser.empty()
        ? component_dir / "slicer_linux_auth_browser"
        : std::filesystem::path(configured_browser);
    const auto runner = component_dir / "run_auth_browser.sh";
    const bool novnc_assets = path_exists_any(component_dir / "share/novnc/vnc.html") ||
                              path_exists_any("/usr/share/novnc/vnc.html") ||
                              path_exists_any("/usr/share/noVNC/vnc.html") ||
                              path_exists_any("/opt/novnc/vnc.html");
    const bool browser_present = ::access(browser.c_str(), X_OK) == 0;
    const bool runner_present = ::access(runner.c_str(), X_OK) == 0;
    const bool xvfb_present = command_exists("Xvfb");
    const bool x11vnc_present = command_exists("x11vnc");
    const bool websockify_present = command_exists("websockify");
    const bool external_browser_present = command_exists("epiphany") || command_exists("epiphany-browser");
    return {
        {"ok", browser_present && runner_present && xvfb_present && x11vnc_present && websockify_present && novnc_assets},
        {"platform", "linux"},
        {"browser_engine", "webkitgtk"},
        {"browser_present", browser_present},
        {"runner_present", runner_present},
        {"xvfb_present", xvfb_present},
        {"x11vnc_present", x11vnc_present},
        {"websockify_present", websockify_present},
        {"external_browser_present", external_browser_present},
        {"dbus_run_session_present", command_exists("dbus-run-session")},
        {"novnc_assets_present", novnc_assets},
        {"host_novnc_port", env_port("SLICER_LINUX_RUNTIME_AUTH_HOST_NOVNC_PORT", 0)},
        {"novnc_port", env_port("SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT", 0)},
        {"vnc_port", env_port("SLICER_LINUX_RUNTIME_AUTH_VNC_PORT", 0)},
        {"linux_release", linux_release()},
        {"linux_os_version", linux_os_version()}
    };
}

void LinuxRuntimeHost::stop_auth_browser_process()
{
    if (!m_auth_session)
        return;
    if (m_auth_session->process_group > 0) {
        ::kill(-m_auth_session->process_group, SIGTERM);
        for (int i = 0; i < 20; ++i) {
            int status = 0;
            const pid_t ret = ::waitpid(m_auth_session->process_group, &status, WNOHANG);
            if (ret == m_auth_session->process_group || ret < 0)
                break;
            std::this_thread::sleep_for(50ms);
        }
        ::kill(-m_auth_session->process_group, SIGKILL);
        int status = 0;
        (void) ::waitpid(m_auth_session->process_group, &status, WNOHANG);
        m_auth_session->process_group = 0;
    }
}

void LinuxRuntimeHost::clear_auth_profile()
{
    (void) cancel_auth_browser("logout");
    std::filesystem::path root = env_or("SLICER_LINUX_RUNTIME_AUTH_STATE_DIR", "");
    if (root.empty()) {
        const std::string home = env_or("HOME", "/tmp");
        root = std::filesystem::path(home) / ".local/state/orcastudio/linux-bambu-browser";
    }
    std::error_code ec;
    std::filesystem::remove_all(root / "webkit-profile", ec);
    std::filesystem::create_directories(root / "webkit-profile", ec);
    ::chmod((root / "webkit-profile").c_str(), S_IRWXU);
}

nlohmann::json LinuxRuntimeHost::cancel_auth_browser(const std::string& reason)
{
    if (!m_auth_session)
        return {{"ok", true}, {"state", "idle"}};
    stop_auth_browser_process();
    m_auth_session->state = reason.empty() ? "cancelled" : reason;
    const auto state = m_auth_session->state;
    std::filesystem::remove(m_auth_session->result_file);
    std::filesystem::remove(m_auth_session->state_dir / "vnc.pass");
    std::filesystem::remove(m_auth_session->state_dir / "viewer-password");
    std::filesystem::remove(m_auth_session->command_file);
    std::filesystem::remove(m_auth_session->event_file);
    std::filesystem::remove_all(m_auth_session->state_dir);
    m_auth_session.reset();
    return {{"ok", true}, {"state", state}};
}

nlohmann::json LinuxRuntimeHost::start_browser_session(
    const std::string& mode,
    std::int64_t agent_id,
    const std::string& url,
    const std::string& client_version,
    const std::string& language,
    const std::string& theme)
{
    const auto caps = auth_browser_capabilities();
    for (const char* key : {"browser_present", "runner_present", "xvfb_present", "x11vnc_present", "websockify_present", "novnc_assets_present"}) {
        if (!caps.value(key, false))
            return {{"ok", false}, {"state", "error"}, {"error", std::string("Linux browser dependency missing: ") + key}, {"capabilities", caps}};
    }
    if (mode != "auth" && mode != "browse")
        return {{"ok", false}, {"state", "error"}, {"error", "invalid Linux browser mode"}};
    if (!allowed_bambu_browser_url(url))
        return {{"ok", false}, {"state", "error"}, {"error", "URL is not an allowed HTTPS Bambu/MakerWorld URL"}};

    const std::filesystem::path component_dir = env_or("SLICER_LINUX_RUNTIME_COMPONENT_DIR", ".");
    const std::filesystem::path runner = component_dir / "run_auth_browser.sh";
    std::filesystem::path root = env_or("SLICER_LINUX_RUNTIME_AUTH_STATE_DIR", "");
    if (root.empty()) {
        const std::string home = env_or("HOME", "/tmp");
        root = std::filesystem::path(home) / ".local/state/orcastudio/linux-bambu-browser";
    }
    std::error_code root_error;
    std::filesystem::create_directories(root, root_error);
    if (root_error)
        return {{"ok", false}, {"state", "error"}, {"error", "failed to create Linux browser state root: " + root_error.message()}};
    ::chmod(root.c_str(), S_IRWXU);

    auto session = std::make_unique<HostAuthSession>();
    session->mode = mode;
    session->agent_handle = agent_id;
    session->session_id = random_alnum(24);
    session->password = random_alnum(18);
    session->host_novnc_port = env_port("SLICER_LINUX_RUNTIME_AUTH_HOST_NOVNC_PORT", 0);
    session->guest_novnc_port = env_port("SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT", 0);
    session->guest_vnc_port = env_port("SLICER_LINUX_RUNTIME_AUTH_VNC_PORT", 0);
    if (session->host_novnc_port <= 0 || session->guest_novnc_port <= 0 || session->guest_vnc_port <= 0 ||
        session->guest_novnc_port == session->guest_vnc_port)
        return {{"ok", false}, {"state", "error"}, {"error", "Linux browser transport ports are not configured"}};
    session->state_dir = root / session->session_id;
    session->result_file = session->state_dir / "result.json";
    session->command_file = session->state_dir / "browser-command.json";
    session->event_file = session->state_dir / "browser-events.ndjson";
    session->state = "starting";
    const auto password_file = session->state_dir / "viewer-password";
    const auto profile_dir = root / "webkit-profile";
    try {
        std::filesystem::create_directories(session->state_dir);
        ::chmod(session->state_dir.c_str(), S_IRWXU);
        write_private_file(password_file, session->password);
        std::filesystem::create_directories(profile_dir);
        ::chmod(profile_dir.c_str(), S_IRWXU);
    } catch (const std::exception& e) {
        std::error_code cleanup_error;
        std::filesystem::remove_all(session->state_dir, cleanup_error);
        return {{"ok", false}, {"state", "error"}, {"error", std::string("failed to prepare Linux browser state: ") + e.what()}};
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        const std::string error = std::string("fork failed: ") + std::strerror(errno);
        std::error_code cleanup_error;
        std::filesystem::remove_all(session->state_dir, cleanup_error);
        return {{"ok", false}, {"state", "error"}, {"error", error}};
    }
    if (pid == 0) {
        ::setsid();
        const std::string novnc_port = std::to_string(session->guest_novnc_port);
        const std::string vnc_port = std::to_string(session->guest_vnc_port);
        ::setenv("SLICER_LINUX_RUNTIME_AUTH_NOVNC_PORT", novnc_port.c_str(), 1);
        ::setenv("SLICER_LINUX_RUNTIME_AUTH_VNC_PORT", vnc_port.c_str(), 1);
        ::setenv("SLICER_LINUX_RUNTIME_AUTH_CLIENT_VERSION", client_version.c_str(), 1);
        ::setenv("SLICER_LINUX_RUNTIME_AUTH_LANGUAGE", language.c_str(), 1);
        ::setenv("SLICER_LINUX_RUNTIME_AUTH_THEME", theme.c_str(), 1);
        ::execl(runner.c_str(), runner.c_str(),
                mode.c_str(), session->state_dir.c_str(), url.c_str(), "-",
                session->result_file.c_str(), profile_dir.c_str(), password_file.c_str(),
                session->command_file.c_str(), session->event_file.c_str(),
                static_cast<char*>(nullptr));
        _exit(127);
    }

    (void) ::setpgid(pid, pid);
    session->process_group = static_cast<int>(pid);
    m_auth_session = std::move(session);

    bool ready = false;
    for (int i = 0; i < 300; ++i) {
        if (std::filesystem::exists(m_auth_session->state_dir / "ready")) {
            ready = true;
            break;
        }
        int status = 0;
        const pid_t ret = ::waitpid(pid, &status, WNOHANG);
        if (ret == pid) {
            m_auth_session->process_group = 0;
            break;
        }
        std::this_thread::sleep_for(100ms);
    }
    if (!ready) {
        std::string error = "Linux browser failed to become ready";
        try {
            const std::string raw = read_text_file(m_auth_session->result_file);
            if (!raw.empty()) {
                const auto result = nlohmann::json::parse(raw);
                if (result.value("kind", std::string()) == "error")
                    error = result.value("error", error);
            }
        } catch (...) {}
        const std::string diagnostic = browser_diagnostic(m_auth_session->state_dir);
        stop_auth_browser_process();
        std::filesystem::remove_all(m_auth_session->state_dir);
        m_auth_session.reset();
        return {{"ok", false}, {"state", "error"}, {"error", error}, {"diagnostic", diagnostic}};
    }

    m_auth_session->state = "running";
    const std::string diagnostic = browser_diagnostic(m_auth_session->state_dir);
    return {
        {"ok", true},
        {"state", "running"},
        {"mode", mode},
        {"session_id", m_auth_session->session_id},
        {"viewer_url", "http://127.0.0.1:" + std::to_string(m_auth_session->host_novnc_port) + "/vnc.html?autoconnect=1&resize=scale&reconnect=1&password=" + m_auth_session->password},
        {"novnc_port", m_auth_session->host_novnc_port},
        {"guest_novnc_port", m_auth_session->guest_novnc_port},
        {"platform", "linux"},
        {"browser_engine", "webkitgtk"},
        {"client_version", client_version},
        {"language", language},
        {"theme", theme},
        {"diagnostic", diagnostic}
    };
}

nlohmann::json LinuxRuntimeHost::start_auth_browser(const nlohmann::json& payload)
{
    if (m_auth_session)
        return {{"ok", false}, {"state", "busy"}, {"mode", m_auth_session->mode}, {"error", "a Linux browser session is already active"}};

    const std::int64_t agent_id = payload.value("agent", 0LL);
    auto agent = acquire_resource<void*>(m_agents, m_state_mutex, agent_id);
    if (!agent)
        return {{"ok", false}, {"state", "error"}, {"error", "agent not found"}};

    const std::string client_version = auth_metadata_token(
        payload.value("client_version", std::string()), "0.0.0.0", 32, true);
    const std::string language = auth_metadata_token(
        payload.value("language", std::string()), "en", 16, false);
    const std::string requested_theme = payload.value("theme", std::string("light"));
    const std::string theme = requested_theme == "dark" ? "dark" : "light";

    return start_browser_session(
        "auth",
        agent_id,
        payload.value("login_url", std::string()),
        client_version,
        language,
        theme);
}

nlohmann::json LinuxRuntimeHost::start_generic_browser(const nlohmann::json& payload)
{
    if (m_auth_session)
        return {{"ok", false}, {"state", "busy"}, {"mode", m_auth_session->mode}, {"error", "a Linux browser session is already active"}};

    std::string target = payload.value("url", std::string());
    const bool bind_ticket = payload.value("bind_ticket", false);
    if (bind_ticket) {
        const std::int64_t agent_id = payload.value("agent", 0LL);
        auto agent = acquire_resource<void*>(m_agents, m_state_mutex, agent_id);
        auto request_ticket = net<int (*)(void*, std::string*)>("bambu_network_request_bind_ticket");
        if (!agent || !request_ticket)
            return not_supported("browser.start:bambu_network_request_bind_ticket");
        std::string ticket;
        const int ret = request_ticket(agent, &ticket);
        if (ret != 0 || ticket.empty())
            return {{"ok", false}, {"state", "error"}, {"error", "Linux Bambu component failed to create a bind ticket"}, {"code", ret}};
        target = build_bind_ticket_url(target, ticket);
        if (target.empty())
            return {{"ok", false}, {"state", "error"}, {"error", "failed to build Linux bind-ticket URL"}};
    }
    return start_browser_session("browse", 0, target, "0.0.0.0", "en", "light");
}

nlohmann::json LinuxRuntimeHost::drain_browser_events()
{
    nlohmann::json events = nlohmann::json::array();
    if (!m_auth_session || m_auth_session->event_file.empty())
        return events;
    std::ifstream in(m_auth_session->event_file, std::ios::binary);
    if (!in)
        return events;
    in.seekg(static_cast<std::streamoff>(m_auth_session->event_offset));
    std::string line;
    while (std::getline(in, line)) {
        const auto next = in.tellg();
        if (line.empty()) {
            if (next >= 0)
                m_auth_session->event_offset = static_cast<std::uintmax_t>(next);
            continue;
        }
        try { events.push_back(nlohmann::json::parse(line)); }
        catch (...) { break; }
        if (next >= 0)
            m_auth_session->event_offset = static_cast<std::uintmax_t>(next);
        else {
            std::error_code ec;
            m_auth_session->event_offset = std::filesystem::file_size(m_auth_session->event_file, ec);
        }
    }
    return events;
}

nlohmann::json LinuxRuntimeHost::browser_command(const nlohmann::json& payload)
{
    if (!m_auth_session || m_auth_session->mode != "browse")
        return {{"ok", false}, {"state", "idle"}, {"error", "Linux browse session is not running"}};
    const std::string command = payload.value("command", std::string());
    static const std::vector<std::string> allowed{"back", "forward", "reload", "load_url", "post_message", "close"};
    if (std::find(allowed.begin(), allowed.end(), command) == allowed.end())
        return {{"ok", false}, {"error", "unsupported browser command"}};
    if (command == "load_url" && !allowed_bambu_browser_url(payload.value("url", std::string())))
        return {{"ok", false}, {"error", "URL is not an allowed HTTPS Bambu/MakerWorld URL"}};
    write_private_file(m_auth_session->command_file, payload.dump());
    return {{"ok", true}, {"state", "running"}};
}

nlohmann::json LinuxRuntimeHost::generic_browser_status()
{
    if (!m_auth_session)
        return {{"ok", true}, {"state", "idle"}, {"events", nlohmann::json::array()}};
    if (m_auth_session->mode != "browse")
        return {{"ok", false}, {"state", m_auth_session->state}, {"error", "authentication session is active"}, {"events", nlohmann::json::array()}};

    auto events = drain_browser_events();
    const std::string raw = read_text_file(m_auth_session->result_file);
    int status = 0;
    const pid_t ret = m_auth_session->process_group > 0 ? ::waitpid(m_auth_session->process_group, &status, WNOHANG) : 0;
    if (!raw.empty() || ret == m_auth_session->process_group) {
        stop_auth_browser_process();
        std::string state = "closed";
        if (raw.empty())
            state = "error";
        std::filesystem::remove_all(m_auth_session->state_dir);
        m_auth_session.reset();
        return {{"ok", state == "closed"}, {"state", state}, {"events", std::move(events)}};
    }
    return {{"ok", true}, {"state", "running"}, {"session_id", m_auth_session->session_id}, {"events", std::move(events)}};
}

nlohmann::json LinuxRuntimeHost::process_auth_browser_result(const nlohmann::json& result)
{
    if (!m_auth_session)
        return {{"ok", false}, {"state", "error"}, {"error", "auth session missing"}};

    auto agent = acquire_resource<void*>(m_agents, m_state_mutex, m_auth_session->agent_handle);
    if (!agent)
        return {{"ok", false}, {"state", "error"}, {"error", "auth agent no longer exists"}};

    const std::string kind = result.value("kind", std::string());
    if (kind == "cancelled")
        return {{"ok", true}, {"state", "cancelled"}};
    if (kind == "error")
        return {{"ok", false}, {"state", "error"}, {"error", result.value("error", std::string("Linux authentication browser failed"))}};

    nlohmann::json login_message;
    std::string access_token;
    std::string refresh_token;
    std::string expires_in;
    std::string refresh_expires_in;
    const std::string autotest_token = result.value("autotest_token", std::string());

    if (kind == "user_login") {
        if (!result.contains("message") || !result["message"].is_object())
            return {{"ok", false}, {"state", "error"}, {"error", "invalid user_login result"}};
        login_message = normalize_change_user_payload(result["message"]);
    } else {
        if (kind == "ticket") {
            auto get_token = net<int (*)(void*, std::string, unsigned int*, std::string*)>("bambu_network_get_my_token");
            if (!get_token)
                return not_supported("auth.result:bambu_network_get_my_token");
            unsigned int http_code = 0;
            std::string body;
            const int ret = get_token(agent, result.value("ticket", std::string()), &http_code, &body);
            if (ret != 0)
                return {{"ok", false}, {"state", "error"}, {"error", "ticket exchange failed"}, {"http_code", http_code}, {"code", ret}};
            try {
                const auto token = nlohmann::json::parse(body);
                access_token = token.value("accessToken", std::string());
                refresh_token = token.value("refreshToken", std::string());
                if (token.contains("expiresIn")) expires_in = token["expiresIn"].is_string() ? token["expiresIn"].get<std::string>() : std::to_string(token["expiresIn"].get<double>());
                if (token.contains("refreshExpiresIn")) refresh_expires_in = token["refreshExpiresIn"].is_string() ? token["refreshExpiresIn"].get<std::string>() : std::to_string(token["refreshExpiresIn"].get<double>());
            } catch (...) {
                return {{"ok", false}, {"state", "error"}, {"error", "ticket response JSON invalid"}};
            }
        } else {
            return {{"ok", false}, {"state", "error"}, {"error", "unsupported auth result kind"}};
        }

        if (access_token.empty())
            return {{"ok", false}, {"state", "error"}, {"error", "access token missing"}};
        auto get_profile = net<int (*)(void*, std::string, unsigned int*, std::string*)>("bambu_network_get_my_profile");
        if (!get_profile)
            return not_supported("auth.result:bambu_network_get_my_profile");
        unsigned int http_code = 0;
        std::string body;
        const int ret = get_profile(agent, access_token, &http_code, &body);
        if (ret != 0)
            return {{"ok", false}, {"state", "error"}, {"error", "profile request failed"}, {"http_code", http_code}, {"code", ret}};
        nlohmann::json profile;
        try { profile = nlohmann::json::parse(body); }
        catch (...) { return {{"ok", false}, {"state", "error"}, {"error", "profile JSON invalid"}}; }
        login_message = {
            {"command", "user_login"},
            {"data", {
                {"autotest_token", autotest_token},
                {"refresh_token", refresh_token},
                {"token", access_token},
                {"expires_in", expires_in},
                {"refresh_expires_in", refresh_expires_in},
                {"user", {
                    {"uid", profile.value("uidStr", std::string())},
                    {"name", profile.value("name", std::string())},
                    {"account", profile.value("account", std::string())},
                    {"avatar", profile.value("avatar", std::string())}
                }}
            }}
        };
    }

    auto change_user = net<int (*)(void*, std::string)>("bambu_network_change_user");
    auto is_login = net<bool (*)(void*)>("bambu_network_is_user_login");
    if (!change_user || !is_login)
        return not_supported("auth.result:bambu_network_change_user");
    const int ret = change_user(agent, normalize_change_user_payload(login_message).dump());
    if (ret != 0)
        return {{"ok", false}, {"state", "error"}, {"error", "change_user failed"}, {"code", ret}};
    for (int i = 0; i < 50 && !is_login(agent); ++i)
        std::this_thread::sleep_for(100ms);
    if (!is_login(agent))
        return {{"ok", false}, {"state", "error"}, {"error", "network component did not accept login"}};
    return {{"ok", true}, {"state", "success"}, {"logged_in", true}};
}

nlohmann::json LinuxRuntimeHost::auth_browser_status()
{
    if (!m_auth_session)
        return {{"ok", true}, {"state", "idle"}};
    if (m_auth_session->mode != "auth")
        return {{"ok", false}, {"state", m_auth_session->state}, {"error", "generic Linux browser session is active"}};
    if (m_auth_session->processed) {
        nlohmann::json out{{"ok", m_auth_session->state == "success"},
                           {"state", m_auth_session->state},
                           {"error", m_auth_session->error}};
        m_auth_session.reset();
        return out;
    }

    const std::string raw = read_text_file(m_auth_session->result_file);
    if (raw.empty()) {
        int status = 0;
        const pid_t ret = ::waitpid(m_auth_session->process_group, &status, WNOHANG);
        if (ret == m_auth_session->process_group) {
            m_auth_session->process_group = 0;
            m_auth_session->processed = true;
            m_auth_session->state = "error";
            m_auth_session->error = "Linux auth browser exited before returning a result";
            const std::string diagnostic = browser_diagnostic(m_auth_session->state_dir);
            std::filesystem::remove(m_auth_session->state_dir / "vnc.pass");
            std::filesystem::remove(m_auth_session->state_dir / "viewer-password");
            std::filesystem::remove_all(m_auth_session->state_dir);
            const std::string error = m_auth_session->error;
            m_auth_session.reset();
            return {{"ok", false}, {"state", "error"}, {"error", error}, {"diagnostic", diagnostic}};
        }
        return {{"ok", true}, {"state", "running"}, {"session_id", m_auth_session->session_id}};
    }

    nlohmann::json result;
    try { result = nlohmann::json::parse(raw); }
    catch (...) {
        m_auth_session->processed = true;
        m_auth_session->state = "error";
        m_auth_session->error = "Linux auth browser returned invalid JSON";
        const std::string diagnostic = browser_diagnostic(m_auth_session->state_dir);
        stop_auth_browser_process();
        std::filesystem::remove(m_auth_session->result_file);
        std::filesystem::remove(m_auth_session->state_dir / "vnc.pass");
        std::filesystem::remove(m_auth_session->state_dir / "viewer-password");
        std::filesystem::remove_all(m_auth_session->state_dir);
        const std::string error = m_auth_session->error;
        m_auth_session.reset();
        return {{"ok", false}, {"state", "error"}, {"error", error}, {"diagnostic", diagnostic}};
    }

    std::filesystem::remove(m_auth_session->result_file);
    std::filesystem::remove(m_auth_session->state_dir / "vnc.pass");
    std::filesystem::remove(m_auth_session->state_dir / "viewer-password");

    auto processed = process_auth_browser_result(result);
    m_auth_session->processed = true;
    m_auth_session->state = processed.value("state", std::string("error"));
    m_auth_session->error = processed.value("error", std::string());

    if (result.value("external_callback", false)) {
        const auto reply_file = m_auth_session->state_dir / "auth-reply.json";
        const auto complete_file = m_auth_session->state_dir / "callback-complete";
        try {
            write_private_file(reply_file, nlohmann::json({
                {"success", processed.value("state", std::string()) == "success"},
                {"schema", 1}
            }).dump());
            for (int i = 0; i < 200 && !std::filesystem::exists(complete_file); ++i)
                std::this_thread::sleep_for(50ms);
        } catch (const std::exception& e) {
            if (m_auth_session->error.empty())
                m_auth_session->error = std::string("failed to acknowledge Linux OAuth callback: ") + e.what();
        }
    }

    const std::string diagnostic = browser_diagnostic(m_auth_session->state_dir);
    stop_auth_browser_process();
    std::filesystem::remove_all(m_auth_session->state_dir);
    m_auth_session.reset();
    if (!processed.value("ok", false) && !diagnostic.empty())
        processed["diagnostic"] = diagnostic;
    return processed;
}

nlohmann::json LinuxRuntimeHost::not_supported(const std::string& method) const
{
    std::size_t agent_count = 0;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        agent_count = m_agents.size();
    }
    return {
        {"ok", false},
        {"error", method + " unsupported in host"},
        {"reason", "missing_symbol_or_invalid_agent"},
        {"agent_count", agent_count},
        {"auth_capabilities", auth_capabilities()}
    };
}

void LinuxRuntimeHost::queue_event(std::int64_t agent_handle, const std::string& name, const nlohmann::json& payload)
{
    if (m_shutting_down.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(m_events_mutex);
    m_events.push_back({{"agent", agent_handle}, {"name", name}, {"payload", payload}});
}

void LinuxRuntimeHost::queue_tunnel_event(std::int64_t tunnel_handle, const std::string& name, const nlohmann::json& payload)
{
    if (m_shutting_down.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(m_events_mutex);
    m_events.push_back({{"tunnel", tunnel_handle}, {"name", name}, {"payload", payload}});
}

nlohmann::json LinuxRuntimeHost::drain_events(std::size_t limit)
{
    std::lock_guard<std::mutex> lock(m_events_mutex);
    nlohmann::json arr = nlohmann::json::array();
    while (!m_events.empty() && arr.size() < limit) {
        arr.push_back(m_events.front());
        m_events.pop_front();
    }
    return {{"ok", true}, {"events", arr}};
}

std::shared_ptr<HostJobState> LinuxRuntimeHost::get_job(std::int64_t job_id)
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    auto it = m_jobs.find(job_id);
    return it == m_jobs.end() ? nullptr : it->second;
}

void LinuxRuntimeHost::register_job(const std::shared_ptr<HostJobState>& job)
{
    if (!job)
        return;
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_jobs[job->job_id] = job;
}

void LinuxRuntimeHost::unregister_job(std::int64_t job_id)
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_jobs.erase(job_id);
}

void LinuxRuntimeHost::set_job_cancel(std::int64_t job_id, bool value)
{
    auto job = get_job(job_id);
    if (job)
        job->cancel_requested = value;
}

void LinuxRuntimeHost::set_job_wait_reply(std::int64_t job_id, std::int64_t request_id, bool value)
{
    auto job = get_job(job_id);
    if (!job)
        return;
    {
        std::lock_guard<std::mutex> lock(job->wait_mutex);
        job->wait_request_id = request_id;
        job->wait_reply_value = value;
        job->wait_reply_ready = true;
    }
    job->wait_cv.notify_all();
}

std::shared_ptr<HostCallbackReplyState> LinuxRuntimeHost::register_callback_request(std::int64_t request_id)
{
    auto state = std::make_shared<HostCallbackReplyState>();
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_callback_replies[request_id] = state;
    return state;
}

void LinuxRuntimeHost::unregister_callback_request(std::int64_t request_id)
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_callback_replies.erase(request_id);
}

void LinuxRuntimeHost::set_callback_reply(std::int64_t request_id, const std::string& value)
{
    std::shared_ptr<HostCallbackReplyState> state;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        auto it = m_callback_replies.find(request_id);
        if (it == m_callback_replies.end())
            return;
        state = it->second;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->string_value = value;
        state->ready = true;
    }
    state->cv.notify_all();
}


nlohmann::json LinuxRuntimeHost::perform_http_request(const nlohmann::json& payload)
{
    std::vector<unsigned char> request_body = std::move(g_thread_request_binary);
    g_thread_request_binary.clear();
    return perform_http_request_impl(payload, std::move(request_body), {});
}

nlohmann::json LinuxRuntimeHost::perform_http_request_impl(
    const nlohmann::json& payload,
    std::vector<unsigned char> request_body,
    const std::shared_ptr<HostHttpJobState>& job)
{
    ensure_curl_initialized();

    const std::string url = payload.value("url", std::string());
    if (!valid_http_url(url))
        return {{"ok", false}, {"transport_ok", false}, {"error", "only absolute HTTP(S) URLs are allowed"}};

    std::string method = payload.value("method", std::string("GET"));
    std::transform(method.begin(), method.end(), method.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    static const std::vector<std::string> allowed_methods{"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};
    if (std::find(allowed_methods.begin(), allowed_methods.end(), method) == allowed_methods.end())
        return {{"ok", false}, {"transport_ok", false}, {"error", "unsupported HTTP method"}};

    const auto max_bytes_raw = payload.value("max_bytes", 128ULL * 1024ULL * 1024ULL);
    const std::size_t max_bytes = static_cast<std::size_t>(std::min<unsigned long long>(max_bytes_raw, 1024ULL * 1024ULL * 1024ULL));
    const long connect_timeout_ms = std::clamp<long>(payload.value("connect_timeout_ms", 15000L), 1000L, 120000L);
    const long timeout_ms = std::clamp<long>(payload.value("timeout_ms", 1800000L), 1000L, 1800000L);
    const std::string range = payload.value("range", std::string());
    if (range.size() > 256 || range.find_first_of("\r\n") != std::string::npos)
        return {{"ok", false}, {"transport_ok", false}, {"error", "invalid HTTP range"}};
    if (request_body.size() > 1024ULL * 1024ULL * 1024ULL)
        return {{"ok", false}, {"transport_ok", false}, {"error", "request body exceeds Linux runtime limit"}};

    struct WriteContext {
        std::shared_ptr<HostHttpJobState> job;
        std::vector<unsigned char> local;
        std::size_t max_bytes{0};
        bool overflow{false};
    } write_context;
    write_context.job = job;
    write_context.max_bytes = std::max<std::size_t>(max_bytes, 1U);

    std::string response_headers;
    response_headers.reserve(4096);

    CURL* curl = curl_easy_init();
    if (!curl)
        return {{"ok", false}, {"transport_ok", false}, {"error", "curl_easy_init failed"}};

    curl_slist* header_list = nullptr;
    curl_mime* multipart = nullptr;
    try {
        const auto append_header = [&](const std::string& name, const std::string& value) {
            if (!valid_http_header(name, value))
                throw std::runtime_error("invalid HTTP header: " + name);
            const std::string line = name + ": " + value;
            curl_slist* next = curl_slist_append(header_list, line.c_str());
            if (!next)
                throw std::bad_alloc();
            header_list = next;
        };

        const auto headers = payload.value("headers", nlohmann::json::array());
        std::size_t header_count = 0;
        if (headers.is_array()) {
            for (const auto& item : headers) {
                if (++header_count > 128)
                    throw std::runtime_error("too many HTTP headers");
                if (!item.is_string())
                    throw std::runtime_error("HTTP header array must contain strings");
                const std::string line = item.get<std::string>();
                const auto colon = line.find(':');
                if (colon == std::string::npos)
                    throw std::runtime_error("invalid HTTP header line");
                std::string value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                    value.erase(value.begin());
                append_header(line.substr(0, colon), value);
            }
        } else if (headers.is_object()) {
            for (auto it = headers.begin(); it != headers.end(); ++it) {
                if (++header_count > 128)
                    throw std::runtime_error("too many HTTP headers");
                append_header(it.key(), it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
            }
        } else {
            throw std::runtime_error("headers must be an array or object");
        }

        const auto multipart_parts = payload.value("multipart_parts", nlohmann::json::array());
        const bool has_multipart = multipart_parts.is_array() && !multipart_parts.empty();
        if (has_multipart) {
            multipart = curl_mime_init(curl);
            if (!multipart)
                throw std::runtime_error("curl_mime_init failed");
            if (multipart_parts.size() > 256)
                throw std::runtime_error("too many multipart parts");
            for (const auto& item : multipart_parts) {
                if (!item.is_object())
                    throw std::runtime_error("invalid multipart part");
                const std::string name = item.value("name", std::string());
                const std::string filename = item.value("filename", std::string());
                const std::string content_type = item.value("content_type", std::string());
                const std::uint64_t offset = item.value("offset", 0ULL);
                const std::uint64_t size = item.value("size", 0ULL);
                if (name.empty() || name.size() > 1024 || filename.size() > 4096 || content_type.size() > 256)
                    throw std::runtime_error("invalid multipart metadata");
                if (offset > request_body.size() || size > request_body.size() - static_cast<std::size_t>(offset))
                    throw std::runtime_error("multipart data range is outside request body");
                curl_mimepart* part = curl_mime_addpart(multipart);
                if (!part)
                    throw std::runtime_error("curl_mime_addpart failed");
                curl_mime_name(part, name.c_str());
                if (!filename.empty())
                    curl_mime_filename(part, filename.c_str());
                if (!content_type.empty())
                    curl_mime_type(part, content_type.c_str());
                const char* data = size == 0 ? "" : reinterpret_cast<const char*>(request_body.data() + offset);
                if (curl_mime_data(part, data, static_cast<size_t>(size)) != CURLE_OK)
                    throw std::runtime_error("curl_mime_data failed");
            }
        }

        char error_buffer[CURL_ERROR_SIZE] = {0};
        const std::string ca_bundle = runtime_ca_bundle_path();
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        if (method == "GET") {
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        } else if (method == "HEAD") {
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
        } else if (has_multipart) {
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, multipart);
            if (method != "POST")
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        } else if (method == "POST") {
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
        } else {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        }
        if (!has_multipart && method != "GET" && method != "HEAD") {
            const char* data = request_body.empty() ? "" : reinterpret_cast<const char*>(request_body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request_body.size()));
        }
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, std::size_t size, std::size_t nmemb, void* userdata) -> std::size_t {
            auto* context = static_cast<WriteContext*>(userdata);
            if (!context || !ptr || (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size))
                return 0;
            const std::size_t bytes = size * nmemb;
            const auto* first = reinterpret_cast<const unsigned char*>(ptr);
            if (context->job) {
                std::lock_guard<std::mutex> lock(context->job->mutex);
                auto& data = context->job->response_binary;
                if (bytes > context->max_bytes || data.size() > context->max_bytes - bytes) {
                    context->overflow = true;
                    return 0;
                }
                data.insert(data.end(), first, first + bytes);
            } else {
                if (bytes > context->max_bytes || context->local.size() > context->max_bytes - bytes) {
                    context->overflow = true;
                    return 0;
                }
                context->local.insert(context->local.end(), first, first + bytes);
            }
            return bytes;
        });
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &write_context);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, +[](char* ptr, std::size_t size, std::size_t nmemb, void* userdata) -> std::size_t {
            auto* headers_out = static_cast<std::string*>(userdata);
            if (!headers_out || !ptr || (size != 0 && nmemb > std::numeric_limits<std::size_t>::max() / size))
                return 0;
            const std::size_t bytes = size * nmemb;
            if (headers_out->size() + bytes > 1024U * 1024U)
                return 0;
            headers_out->append(ptr, bytes);
            return bytes;
        });
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
        curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        if (!ca_bundle.empty())
            curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR,
            url.rfind("https://", 0) == 0 ? "https" : "http,https");
#else
        const long allowed_protocols = static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS);
        const long redirect_protocols = url.rfind("https://", 0) == 0
            ? static_cast<long>(CURLPROTO_HTTPS)
            : allowed_protocols;
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS, allowed_protocols);
        curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, redirect_protocols);
#endif
        const std::string user_agent = payload.value("user_agent", std::string());
        if (!user_agent.empty())
            curl_easy_setopt(curl, CURLOPT_USERAGENT, user_agent.c_str());
        if (!range.empty())
            curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
        if (header_list)
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        if (job) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                +[](void* userdata, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
                    auto* state = static_cast<HostHttpJobState*>(userdata);
                    if (!state)
                        return 0;
                    {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->download_total = dltotal > 0 ? static_cast<std::uint64_t>(dltotal) : 0;
                        state->download_now = dlnow > 0 ? static_cast<std::uint64_t>(dlnow) : 0;
                        state->upload_total = ultotal > 0 ? static_cast<std::uint64_t>(ultotal) : 0;
                        state->upload_now = ulnow > 0 ? static_cast<std::uint64_t>(ulnow) : 0;
                    }
                    return state->cancel_requested.load(std::memory_order_acquire) ? 1 : 0;
                });
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job.get());
        }

        const CURLcode code = curl_easy_perform(curl);
        long status = 0;
        char* effective_url = nullptr;
        char* content_type = nullptr;
        char* primary_ip = nullptr;
        curl_off_t upload_speed = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
        curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
        curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary_ip);
        curl_easy_getinfo(curl, CURLINFO_SPEED_UPLOAD_T, &upload_speed);
        if (job) {
            std::lock_guard<std::mutex> lock(job->mutex);
            job->upload_speed = static_cast<double>(upload_speed);
        }

        const bool cancelled = job && job->cancel_requested.load(std::memory_order_acquire) && code == CURLE_ABORTED_BY_CALLBACK;
        const bool transport_ok = code == CURLE_OK;
        const std::size_t response_size = job ? [&] {
            std::lock_guard<std::mutex> lock(job->mutex);
            return job->response_binary.size();
        }() : write_context.local.size();
        nlohmann::json out{
            {"ok", transport_ok && status >= 200 && status < 300},
            {"transport_ok", transport_ok},
            {"cancelled", cancelled},
            {"curl_code", static_cast<int>(code)},
            {"http_status", status},
            {"bytes", response_size},
            {"effective_url", std::string(effective_url ? effective_url : "")},
            {"content_type", std::string(content_type ? content_type : "")},
            {"primary_ip", std::string(primary_ip ? primary_ip : "")},
            {"response_headers", response_headers}
        };

        if (!transport_ok) {
            std::string error = error_buffer[0] ? error_buffer : curl_easy_strerror(code);
            if (cancelled)
                error = "cancelled";
            else if (write_context.overflow)
                error = "response exceeds max_bytes";
            else if (response_headers.size() >= 1024U * 1024U)
                error = "response headers exceed limit";
            out["error"] = error;
        }

        if (!job && !write_context.local.empty()) {
            g_thread_reply_binary = std::move(write_context.local);
            out["__binary_pending"] = true;
            out["binary_size"] = g_thread_reply_binary.size();
        } else {
            out["binary_size"] = response_size;
        }

        if (multipart)
            curl_mime_free(multipart);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        return out;
    } catch (const std::exception& e) {
        if (multipart)
            curl_mime_free(multipart);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);
        return {{"ok", false}, {"transport_ok", false}, {"error", e.what()}};
    }
}

nlohmann::json LinuxRuntimeHost::start_http_request(const nlohmann::json& payload)
{
    if (m_shutting_down.load(std::memory_order_acquire))
        return {{"ok", false}, {"error", "runtime host is shutting down"}};

    std::vector<unsigned char> request_body = std::move(g_thread_request_binary);
    g_thread_request_binary.clear();
    const std::int64_t id = m_next_http_job.fetch_add(1, std::memory_order_relaxed);
    auto state = std::make_shared<HostHttpJobState>();
    {
        std::lock_guard<std::mutex> lock(m_http_jobs_mutex);
        if (m_shutting_down.load(std::memory_order_acquire))
            return {{"ok", false}, {"error", "runtime host is shutting down"}};
        state->worker = std::thread([this, state, payload, request_body = std::move(request_body)]() mutable {
            nlohmann::json result;
            try {
                result = perform_http_request_impl(payload, std::move(request_body), state);
            } catch (const std::exception& e) {
                result = {{"ok", false}, {"transport_ok", false}, {"error", e.what()}};
            } catch (...) {
                result = {{"ok", false}, {"transport_ok", false}, {"error", "unknown HTTP worker failure"}};
            }
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = std::move(result);
                state->done = true;
            }
        });
        m_http_jobs[id] = state;
    }
    return {{"ok", true}, {"job", id}};
}

nlohmann::json LinuxRuntimeHost::http_request_status(const nlohmann::json& payload)
{
    const std::int64_t id = payload.value("job", 0LL);
    std::shared_ptr<HostHttpJobState> state;
    {
        std::lock_guard<std::mutex> lock(m_http_jobs_mutex);
        auto it = m_http_jobs.find(id);
        if (it == m_http_jobs.end())
            return {{"ok", false}, {"error", "unknown HTTP job"}};
        state = it->second;
    }

    nlohmann::json out{{"ok", true}, {"job", id}};
    std::lock_guard<std::mutex> lock(state->mutex);
    out["done"] = state->done;
    out["download_total"] = state->download_total;
    out["download_now"] = state->download_now;
    out["upload_total"] = state->upload_total;
    out["upload_now"] = state->upload_now;
    out["upload_speed"] = state->upload_speed;
    if (state->reported_response_bytes < state->response_binary.size()) {
        const std::size_t offset = state->reported_response_bytes;
        g_thread_reply_binary.assign(state->response_binary.begin() + static_cast<std::ptrdiff_t>(offset), state->response_binary.end());
        state->reported_response_bytes = state->response_binary.size();
        out["__binary_pending"] = true;
        out["binary_size"] = g_thread_reply_binary.size();
        out["chunk_offset"] = offset;
        out["chunk_size"] = g_thread_reply_binary.size();
    }
    if (state->done) {
        for (auto it = state->result.begin(); it != state->result.end(); ++it)
            out[it.key()] = it.value();
        out["done"] = true;
    }
    return out;
}

nlohmann::json LinuxRuntimeHost::cancel_http_request(const nlohmann::json& payload)
{
    const std::int64_t id = payload.value("job", 0LL);
    std::lock_guard<std::mutex> lock(m_http_jobs_mutex);
    auto it = m_http_jobs.find(id);
    if (it == m_http_jobs.end())
        return {{"ok", false}, {"error", "unknown HTTP job"}};
    it->second->cancel_requested.store(true, std::memory_order_release);
    return {{"ok", true}, {"value", 0}};
}

nlohmann::json LinuxRuntimeHost::release_http_request(const nlohmann::json& payload)
{
    const std::int64_t id = payload.value("job", 0LL);
    std::shared_ptr<HostHttpJobState> state;
    {
        std::lock_guard<std::mutex> lock(m_http_jobs_mutex);
        auto it = m_http_jobs.find(id);
        if (it == m_http_jobs.end())
            return {{"ok", true}, {"value", 0}};
        state = it->second;
        m_http_jobs.erase(it);
    }
    state->cancel_requested.store(true, std::memory_order_release);
    if (state->worker.joinable())
        state->worker.join();
    return {{"ok", true}, {"value", 0}};
}

nlohmann::json LinuxRuntimeHost::handle(const std::string& method, const nlohmann::json& raw_payload)
{
    using namespace BBL;

    clear_thread_reply_binary();

    const nlohmann::json empty_payload = nlohmann::json::object();
    const nlohmann::json& payload = raw_payload.is_null() ? empty_payload : raw_payload;
    if (!payload.is_object())
        return {{"ok", false}, {"error", "request payload must be object"}, {"method", method}};

    if (method == "runtime.poll_events")
        return drain_events(payload.value("limit", 64U));

    if (method == "http.start")
        return start_http_request(payload);
    if (method == "http.status")
        return http_request_status(payload);
    if (method == "http.cancel")
        return cancel_http_request(payload);
    if (method == "http.release")
        return release_http_request(payload);
    if (method == "http.get" || method == "http.request")
        return perform_http_request(payload);

    if (method.rfind("auth.", 0) == 0 || method.rfind("browser.", 0) == 0) {
        std::lock_guard<std::mutex> lock(m_auth_mutex);
        if (method == "auth.capabilities")
            return auth_browser_capabilities();
        if (method == "auth.start")
            return start_auth_browser(payload);
        if (method == "auth.status")
            return auth_browser_status();
        if (method == "auth.cancel")
            return cancel_auth_browser(payload.value("reason", std::string("cancelled")));
        if (method == "auth.clear_profile") {
            clear_auth_profile();
            return {{"ok", true}, {"value", 0}};
        }
        if (method == "browser.start")
            return start_generic_browser(payload);
        if (method == "browser.status")
            return generic_browser_status();
        if (method == "browser.command")
            return browser_command(payload);
        if (method == "browser.cancel")
            return cancel_auth_browser(payload.value("reason", std::string("cancelled")));
    }

    if (method == "runtime.ping")
        return {{"ok", true}, {"value", "pong"}};

    if (method == "runtime.get_refresh_agora_url_ptr") {
        return {{"ok", true}, {"value", 0}, {"result", refresh_agora_url_ptr_string()}};
    }
    if (method == "runtime.handshake") {
        load_modules();
        const std::filesystem::path component_folder = std::filesystem::path(env_or("SLICER_LINUX_RUNTIME_COMPONENT_DIR", "."));
        nlohmann::json out = nlohmann::json::object();
        out["ok"] = true;
        out["protocol_version"] = 1;
        out["runtime_version"] = "SLICER-LINUX-RUNTIME-0.6";
        out["component_abi_version"] = expected_component_abi_version();
        out["network_abi_version"] = expected_component_abi_version();
        out["guest_arch"] = host_arch_string();
        out["component_dir"] = component_folder.string();
        out["component_so_present"] = path_exists(component_folder / linux_component_library_name());
        out["source_so_present"] = path_exists(component_folder / linux_source_library_name());
        out["manifest_present"] = path_exists(component_folder / linux_component_manifest_file_name());
        {
            std::lock_guard<std::recursive_mutex> module_lock(m_module_mutex);
            out["component_actual_abi_version"] = m_component_actual_abi_version;
            out["network_actual_abi_version"] = m_component_actual_abi_version;
            out["component_loaded"] = m_component != nullptr;
            out["network_loaded"] = m_component != nullptr;
            out["source_loaded"] = m_source != nullptr;
            out["component_status"] = m_component_status;
            out["network_status"] = m_component_status;
            out["source_status"] = m_source_status;
        }
        out["auth_capabilities"] = auth_capabilities();
        {
            std::lock_guard<std::mutex> state_lock(m_state_mutex);
            out["agent_count"] = m_agents.size();
        }
        return out;
    }
    if (method == "runtime.capabilities") {
        std::size_t agent_count = 0;
        {
            std::lock_guard<std::mutex> state_lock(m_state_mutex);
            agent_count = m_agents.size();
        }
        return {{"ok", true}, {"agent_count", agent_count}, {"auth_capabilities", auth_capabilities()}};
    }
    if (method == "runtime.runtime_info") {
        load_modules();
        char cwd_buf[4096] = {0};
        std::string cwd;
        if (::getcwd(cwd_buf, sizeof(cwd_buf) - 1))
            cwd = cwd_buf;
        nlohmann::json out = nlohmann::json::object();
        out["ok"] = true;
        out["cwd"] = cwd;
        out["home"] = env_or("HOME", "");
        out["component_dir"] = env_or("SLICER_LINUX_RUNTIME_COMPONENT_DIR", "");
        out["component_so"] = env_or("SLICER_LINUX_RUNTIME_COMPONENT_SO", "");
        out["source_so"] = env_or("SLICER_LINUX_RUNTIME_SOURCE_SO", "");
        out["ssl_cert_file"] = env_or("SSL_CERT_FILE", "");
        out["ssl_cert_dir"] = env_or("SSL_CERT_DIR", "");
        out["curl_ca_bundle"] = env_or("CURL_CA_BUNDLE", "");
        out["runtime_ca_bundle"] = runtime_ca_bundle_path();
        out["ld_library_path"] = env_or("LD_LIBRARY_PATH", "");
        {
            std::lock_guard<std::recursive_mutex> module_lock(m_module_mutex);
            out["component_loaded"] = m_component != nullptr;
            out["network_loaded"] = m_component != nullptr;
            out["source_loaded"] = m_source != nullptr;
            out["component_status"] = m_component_status;
            out["network_status"] = m_component_status;
            out["source_status"] = m_source_status;
        }
        host_log_json("runtime.runtime_info", out);
        return out;
    }

    if (method == "runtime.job_cancel") {
        set_job_cancel(payload.value("job_id", 0LL), payload.value("cancel", true));
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "runtime.job_wait_reply") {
        set_job_wait_reply(payload.value("job_id", 0LL), payload.value("request_id", 0LL), payload.value("reply", true));
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "runtime.callback_reply") {
        set_callback_reply(payload.value("request_id", 0LL), payload.value("value", std::string()));
        return {{"ok", true}, {"value", 0}};
    }

    if (method == "net.create_agent") {
        auto f = net<void* (*)(std::string)>("bambu_network_create_agent");
        if (!f) return not_supported(method);
        const std::string log_dir = windows_path_to_wsl(payload.value("log_dir", std::string()));
        host_log_json("net.create_agent", {{"log_dir", log_dir}, {"log_dir_exists", path_exists_any(log_dir)}});
        void* raw = f(log_dir);
        const auto id = m_next_agent.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_agents[id] = make_host_resource(raw);
            m_country_codes[id] = payload.value("country_code", std::string());
        }
        return {{"ok", true}, {"value", id}};
    }
    if (method == "net.destroy_agent") {
        auto f = net<int (*)(void*)>("bambu_network_destroy_agent");
        if (!f) return not_supported(method);
        const auto id = payload.value("agent", 0LL);
        auto resource = detach_resource(m_agents, m_state_mutex, id);
        if (!resource)
            return {{"ok", false}, {"error", "agent not found"}};
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_country_codes.erase(id);
        }
        void* raw = wait_and_take_resource(resource);
        const int ret = f(raw);
        return {{"ok", true}, {"value", ret}};
    }

    const auto lookup_agent = [&]() {
        return acquire_resource<void*>(m_agents, m_state_mutex, payload.value("agent", 0LL));
    };

    const auto agent_id = payload.value("agent", 0LL);

    const auto lookup_tunnel = [&]() {
        return acquire_resource<Bambu_Tunnel>(m_tunnels, m_state_mutex, payload.value("tunnel", 0LL));
    };

    const auto lookup_ft_tunnel = [&]() {
        return acquire_resource<FT_TunnelHandle*>(m_ft_tunnels, m_state_mutex, payload.value("tunnel", 0LL));
    };

    const auto lookup_ft_job = [&]() {
        return acquire_resource<FT_JobHandle*>(m_ft_jobs, m_state_mutex, payload.value("job", 0LL));
    };

    if (method == "net.set_config_dir") {
        auto f = net<int (*)(void*, std::string)>("bambu_network_set_config_dir");
        auto a = lookup_agent();
        const std::string config_dir = windows_path_to_wsl(payload.value("config_dir", std::string()));
        if (!f || !a) return not_supported(method);
        const int ret = f(a, config_dir);
        host_log_json("net.set_config_dir", {{"config_dir", config_dir}, {"config_dir_exists", path_exists_any(config_dir)}, {"value", ret}});
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "net.set_cert_file") {
        auto f = net<int (*)(void*, std::string, std::string)>("bambu_network_set_cert_file");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto folder = windows_path_to_wsl(payload.value("folder", std::string()));
        const auto filename = payload.value("filename", std::string());
        const int ret = f(a, folder, filename);
        nlohmann::json r{{"ok", true}, {"value", ret}, {"folder", folder}, {"folder_exists", path_exists_any(folder)}, {"filename", filename}, {"cert_file_exists", path_exists(folder.empty() ? std::filesystem::path(filename) : std::filesystem::path(folder) / filename)}, {"ssl_cert_file", env_or("SSL_CERT_FILE", "")}, {"curl_ca_bundle", env_or("CURL_CA_BUNDLE", "")}};
        host_log_json("net.set_cert_file", r);
        return r;
    }
    if (method == "net.set_country_code") {
        auto f = net<int (*)(void*, std::string)>("bambu_network_set_country_code");
        auto a = lookup_agent();
        const auto code = payload.value("country_code", std::string());
        { std::lock_guard<std::mutex> lock(m_state_mutex); m_country_codes[agent_id] = code; }
        return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, code)}} : not_supported(method);
    }
    if (method == "net.init_log") { auto f = net<int (*)(void*)>("bambu_network_init_log"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.start") { auto f = net<int (*)(void*)>("bambu_network_start"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.connect_server") { auto f = net<int (*)(void*)>("bambu_network_connect_server"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.is_server_connected") { auto f = net<bool (*)(void*)>("bambu_network_is_server_connected"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.refresh_connection") { auto f = net<int (*)(void*)>("bambu_network_refresh_connection"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.start_subscribe") { auto f = net<int (*)(void*, std::string)>("bambu_network_start_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("module", std::string()))}} : not_supported(method); }
    if (method == "net.stop_subscribe") { auto f = net<int (*)(void*, std::string)>("bambu_network_stop_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("module", std::string()))}} : not_supported(method); }
    if (method == "net.add_subscribe") { auto f = net<int (*)(void*, std::vector<std::string>)>("bambu_network_add_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("devs", std::vector<std::string>()))}} : not_supported(method); }
    if (method == "net.del_subscribe") { auto f = net<int (*)(void*, std::vector<std::string>)>("bambu_network_del_subscribe"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("devs", std::vector<std::string>()))}} : not_supported(method); }
    if (method == "net.enable_multi_machine") { auto f = net<void (*)(void*, bool)>("bambu_network_enable_multi_machine"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); f(a, payload.value("enable", false)); return {{"ok", true}, {"value", 0}}; }
    if (method == "net.send_message") { auto f = net<int (*)(void*, std::string, std::string, int, int)>("bambu_network_send_message"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()), payload.value("msg", std::string()), payload.value("qos", 0), payload.value("flag", 0))}} : not_supported(method); }
    if (method == "net.connect_printer") {
        auto f = net<int (*)(void*, std::string, std::string, std::string, std::string, bool)>("bambu_network_connect_printer");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);

        const auto dev_id = payload.value("dev_id", std::string());
        const auto dev_ip = payload.value("dev_ip", std::string());
        const auto username = payload.value("username", std::string());
        const auto password = payload.value("password", std::string());
        const bool use_ssl = payload.value("use_ssl", false);

        host_log_json("net.connect_printer.begin", {{"agent", agent_id}, {"dev_id", dev_id}, {"dev_ip", dev_ip}, {"username", username}, {"use_ssl", use_ssl}});
        const int ret = f(a, dev_id, dev_ip, username, password, use_ssl);
        host_log_json("net.connect_printer.end", {{"agent", agent_id}, {"dev_id", dev_id}, {"dev_ip", dev_ip}, {"value", ret}});
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "net.disconnect_printer") { auto f = net<int (*)(void*)>("bambu_network_disconnect_printer"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.send_message_to_printer") { auto f = net<int (*)(void*, std::string, std::string, int, int)>("bambu_network_send_message_to_printer"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()), payload.value("msg", std::string()), payload.value("qos", 0), payload.value("flag", 0))}} : not_supported(method); }
    if (method == "net.update_cert") { auto f = net<int (*)(void*)>("bambu_network_update_cert"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.install_device_cert") { auto f = net<void (*)(void*, std::string, bool)>("bambu_network_install_device_cert"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); f(a, payload.value("dev_id", std::string()), payload.value("lan_only", false)); return {{"ok", true}, {"value", 0}}; }
    if (method == "net.start_discovery") { auto f = net<bool (*)(void*, bool, bool)>("bambu_network_start_discovery"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("start", false), payload.value("sending", false))}} : not_supported(method); }

    if (method == "net.set_on_ssdp_msg_fn") {
        auto f = net<int (*)(void*, OnMsgArrivedFn)>("bambu_network_set_on_ssdp_msg_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_info_json_str) { queue_event(agent_id, "on_ssdp_msg", {{"dev_info_json_str", dev_info_json_str}}); })}};
    }
    if (method == "net.set_on_user_login_fn") {
        auto f = net<int (*)(void*, OnUserLoginFn)>("bambu_network_set_on_user_login_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](int online_login, bool login) { queue_event(agent_id, "on_user_login", {{"online_login", online_login}, {"login", login}}); })}};
    }
    if (method == "net.set_on_printer_connected_fn") {
        auto f = net<int (*)(void*, OnPrinterConnectedFn)>("bambu_network_set_on_printer_connected_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string topic_str) { queue_event(agent_id, "on_printer_connected", {{"topic_str", topic_str}}); })}};
    }
    if (method == "net.set_on_server_connected_fn") {
        auto f = net<int (*)(void*, OnServerConnectedFn)>("bambu_network_set_on_server_connected_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](int return_code, int reason_code) { queue_event(agent_id, "on_server_connected", {{"return_code", return_code}, {"reason_code", reason_code}}); })}};
    }
    if (method == "net.set_on_http_error_fn") {
        auto f = net<int (*)(void*, OnHttpErrorFn)>("bambu_network_set_on_http_error_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](unsigned http_code, std::string http_body) { queue_event(agent_id, "on_http_error", {{"http_code", http_code}, {"http_body", http_body}}); })}};
    }
    if (method == "net.set_get_country_code_fn") {
        auto f = net<int (*)(void*, GetCountryCodeFn)>("bambu_network_set_get_country_code_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id]() {
            const auto request_id = m_next_callback_request.fetch_add(1);
            auto state = register_callback_request(request_id);
            queue_event(agent_id, "callback.get_country_code", {{"request_id", request_id}});
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->cv.wait_for(lock, 30s, [&] { return state->ready; })) {
                unregister_callback_request(request_id);
                std::lock_guard<std::mutex> s_lock(m_state_mutex);
                auto it = m_country_codes.find(agent_id);
                return it == m_country_codes.end() ? std::string() : it->second;
            }
            const auto value = state->string_value;
            lock.unlock();
            unregister_callback_request(request_id);
            return value;
        })}};
    }
    if (method == "net.set_on_subscribe_failure_fn") {
        auto f = net<int (*)(void*, GetSubscribeFailureFn)>("bambu_network_set_on_subscribe_failure_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string topic) { queue_event(agent_id, "on_subscribe_failure", {{"topic", topic}}); })}};
    }
    if (method == "net.set_on_message_fn") {
        auto f = net<int (*)(void*, OnMessageFn)>("bambu_network_set_on_message_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_id, std::string msg) { queue_event(agent_id, "on_message", {{"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_on_user_message_fn") {
        auto f = net<int (*)(void*, OnMessageFn)>("bambu_network_set_on_user_message_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_id, std::string msg) { queue_event(agent_id, "on_user_message", {{"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_on_local_connect_fn") {
        auto f = net<int (*)(void*, OnLocalConnectedFn)>("bambu_network_set_on_local_connect_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](int status, std::string dev_id, std::string msg) { queue_event(agent_id, "on_local_connect", {{"status", status}, {"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_on_local_message_fn") {
        auto f = net<int (*)(void*, OnMessageFn)>("bambu_network_set_on_local_message_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string dev_id, std::string msg) { queue_event(agent_id, "on_local_message", {{"dev_id", dev_id}, {"msg", msg}}); })}};
    }
    if (method == "net.set_queue_on_main_fn") {
        auto f = net<int (*)(void*, QueueOnMainFn)>("bambu_network_set_queue_on_main_fn");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this](std::function<void()> fn) { queue_main_task(std::move(fn)); })}};
    }
    if (method == "net.set_server_callback") {
        auto f = net<int (*)(void*, OnServerErrFn)>("bambu_network_set_server_callback");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        return {{"ok", true}, {"value", f(a, [this, agent_id](std::string url, int status) { queue_event(agent_id, "on_server_error", {{"url", url}, {"status", status}}); })}};
    }

    if (method == "net.change_user") {
        auto f = net<int (*)(void*, std::string)>("bambu_network_change_user");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto original_user_info = payload.value("user_info", std::string());
        const auto normalized_user_info = normalize_change_user_payload_string(original_user_info);
        const int ret = f(a, normalized_user_info);
        nlohmann::json r{{"ok", true}, {"value", ret}, {"user_info_original", original_user_info}, {"user_info_normalized", normalized_user_info}, {"user_info_was_normalized", normalized_user_info != original_user_info}};
        auto g1 = net<bool (*)(void*)>("bambu_network_is_user_login"); if (g1) r["logged_in"] = g1(a);
        auto g2 = net<std::string (*)(void*)>("bambu_network_get_user_id"); if (g2) r["user_id"] = g2(a);
        auto g3 = net<std::string (*)(void*)>("bambu_network_get_user_name"); if (g3) r["user_name"] = g3(a);
        auto g4 = net<std::string (*)(void*)>("bambu_network_get_user_avatar"); if (g4) r["user_avatar"] = g4(a);
        auto g5 = net<std::string (*)(void*)>("bambu_network_get_user_nickanme"); if (g5) r["user_nickname"] = g5(a);
        auto g6 = net<std::string (*)(void*)>("bambu_network_build_login_cmd"); if (g6) r["login_cmd"] = g6(a);
        auto g7 = net<std::string (*)(void*)>("bambu_network_build_logout_cmd"); if (g7) r["logout_cmd"] = g7(a);
        auto g8 = net<std::string (*)(void*)>("bambu_network_build_login_info"); if (g8) r["login_info"] = g8(a);
        auto g9 = net<std::string (*)(void*)>("bambu_network_get_bambulab_host"); if (g9) r["bambulab_host"] = g9(a);
        host_log_json("net.change_user", r);
        return r;
    }

    if (method == "net.is_user_login") { auto f = net<bool (*)(void*)>("bambu_network_is_user_login"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.user_logout") {
        auto f = net<int (*)(void*, bool)>("bambu_network_user_logout");
        auto a = lookup_agent();
        if (!f || !a)
            return not_supported(method);

        const bool request = payload.value("request", false);
        bool auth_running = false;
        {
            std::lock_guard<std::mutex> lock(m_auth_mutex);
            auth_running = m_auth_session &&
                           m_auth_session->mode == "auth" &&
                           (m_auth_session->state == "starting" || m_auth_session->state == "running");
        }
        if (!request && auth_running)
            return {{"ok", true}, {"value", 0}, {"suppressed_during_auth", true}};

        const int value = f(a, request);
        if (request) {
            std::lock_guard<std::mutex> lock(m_auth_mutex);
            clear_auth_profile();
        }
        return {{"ok", true}, {"value", value}};
    }
    if (method == "net.get_user_id") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_id"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_name") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_name"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_avatar") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_avatar"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_nickname") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_nickanme"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.build_login_cmd") { auto f = net<std::string (*)(void*)>("bambu_network_build_login_cmd"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.build_logout_cmd") { auto f = net<std::string (*)(void*)>("bambu_network_build_logout_cmd"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.build_login_info") { auto f = net<std::string (*)(void*)>("bambu_network_build_login_info"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.ping_bind") { auto f = net<int (*)(void*, std::string)>("bambu_network_ping_bind"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("ping_code", std::string()))}} : not_supported(method); }
    if (method == "net.bind_detect") { auto f = net<int (*)(void*, std::string, std::string, detectResult&)>("bambu_network_bind_detect"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); detectResult det; const int ret = f(a, payload.value("dev_ip", std::string()), payload.value("sec_link", std::string()), det); return {{"ok", true}, {"value", ret}, {"detect", {{"result_msg", det.result_msg}, {"command", det.command}, {"dev_id", det.dev_id}, {"model_id", det.model_id}, {"dev_name", det.dev_name}, {"version", det.version}, {"bind_state", det.bind_state}, {"connect_type", det.connect_type}}}}; }
    if (method == "net.report_consent") { auto f = net<int (*)(void*, std::string)>("bambu_network_report_consent"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("expand", std::string()))}} : not_supported(method); }
    if (method == "net.bind") {
        auto f = net<int (*)(void*, std::string, std::string, std::string, std::string, std::string, bool, OnUpdateStatusFn)>("bambu_network_bind");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "bind";
        register_job(job);
        const int ret = f(a, params.value("dev_ip", std::string()), params.value("dev_id", std::string()), params.value("dev_model", std::string()), params.value("sec_link", std::string()), params.value("timezone", std::string()), params.value("improved", false), [this, job](int status, int code, std::string msg) {
            queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
        });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.unbind") { auto f = net<int (*)(void*, std::string)>("bambu_network_unbind"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()))}} : not_supported(method); }
    if (method == "net.get_bambulab_host") { auto f = net<std::string (*)(void*)>("bambu_network_get_bambulab_host"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.get_user_selected_machine") { auto f = net<std::string (*)(void*)>("bambu_network_get_user_selected_machine"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.set_user_selected_machine") { auto f = net<int (*)(void*, std::string)>("bambu_network_set_user_selected_machine"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()))}} : not_supported(method); }
    if (method == "net.start_print") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)>("bambu_network_start_print");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto params = print_params_from_json(params_json);
        host_log_json("net.start_print.params", {
            {"dev_id", params.dev_id},
            {"connection_type", params.connection_type},
            {"comments", params.comments},
            {"filename", params.filename},
            {"filename_exists", path_exists(params.filename)},
            {"config_filename", params.config_filename},
            {"config_exists", params.config_filename.empty() ? true : path_exists(params.config_filename)},
            {"plate_index", params.plate_index},
            {"task_use_ams", params.task_use_ams},
            {"task_record_timelapse", params.task_record_timelapse},
            {"task_timelapse_use_internal", params.task_timelapse_use_internal},
            {"try_emmc_print", params.try_emmc_print},
            {"svc_context_len", params.svc_context.size()}
        });
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_print";
        register_job(job);
        const int ret = f(a, params,
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            },
            [this, job](int status, std::string job_info) {
                queue_event(job->agent_handle, "job.wait", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"job_info", job_info}});
                return !job->cancel_requested.load();
            });
        unregister_job(job_id);
        host_log_json("net.start_print.result", {{"value", ret}, {"job_id", job_id}});
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }

    if (method == "net.start_local_print_with_record") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)>("bambu_network_start_local_print_with_record");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_local_print_with_record";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            },
            [this, job](int status, std::string job_info) {
                queue_event(job->agent_handle, "job.wait", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"job_info", job_info}});
                return !job->cancel_requested.load();
            });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.start_local_print") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn)>("bambu_network_start_local_print");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_local_print";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.start_send_gcode_to_sdcard") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn, OnWaitFn)>("bambu_network_start_send_gcode_to_sdcard");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_send_gcode_to_sdcard";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            },
            nullptr);
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.start_sdcard_print") {
        auto f = net<int (*)(void*, BBL::PrintParams, OnUpdateStatusFn, WasCancelledFn)>("bambu_network_start_sdcard_print");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        const auto params_json = payload.value("params", nlohmann::json::object());
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_sdcard_print";
        register_job(job);
        const int ret = f(a, print_params_from_json(params_json),
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            });
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}};
    }
    if (method == "net.get_studio_info_url") { auto f = net<std::string (*)(void*)>("bambu_network_get_studio_info_url"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.modify_printer_name") { auto f = net<int (*)(void*, std::string, std::string)>("bambu_network_modify_printer_name"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("dev_id", std::string()), payload.value("dev_name", std::string()))}} : not_supported(method); }
    if (method == "net.get_task_plate_index") { auto f = net<int (*)(void*, std::string, int*)>("bambu_network_get_task_plate_index"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); int plate_index = -1; const int ret = f(a, payload.value("task_id", std::string()), &plate_index); return {{"ok", true}, {"value", ret}, {"plate_index", plate_index}}; }
    if (method == "net.get_user_info") { auto f = net<int (*)(void*, int*)>("bambu_network_get_user_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); int identifier = 0; const int ret = f(a, &identifier); return {{"ok", true}, {"value", ret}, {"identifier", identifier}}; }
    if (method == "net.request_bind_ticket") { auto f = net<int (*)(void*, std::string*)>("bambu_network_request_bind_ticket"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string ticket; const int ret = f(a, &ticket); return {{"ok", true}, {"value", ret}, {"ticket", ticket}}; }
    if (method == "net.query_bind_status") { auto f = net<int (*)(void*, std::vector<std::string>, unsigned int*, std::string*)>("bambu_network_query_bind_status"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("query_list", std::vector<std::string>()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_filament_spools") { auto f = net<int (*)(void*, FilamentQueryParams, std::string*)>("bambu_network_get_filament_spools"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); FilamentQueryParams params; params.category = payload.value("category", std::string()); params.status = payload.value("status", std::string()); params.spool_id = payload.value("spool_id", std::string()); params.rfid = payload.value("rfid", std::string()); params.offset = payload.value("offset", 0); params.limit = payload.value("limit", 20); std::string http_body; const int ret = f(a, params, &http_body); return {{"ok", true}, {"value", ret}, {"http_body", http_body}}; }
    if (method == "net.create_filament_spool") { auto f = net<int (*)(void*, std::string, std::string*)>("bambu_network_create_filament_spool"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string http_body; const int ret = f(a, payload.value("request_body", std::string()), &http_body); return {{"ok", true}, {"value", ret}, {"http_body", http_body}}; }
    if (method == "net.update_filament_spool") { auto f = net<int (*)(void*, std::string, std::string, std::string*)>("bambu_network_update_filament_spool"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string http_body; const int ret = f(a, payload.value("spool_id", std::string()), payload.value("request_body", std::string()), &http_body); return {{"ok", true}, {"value", ret}, {"http_body", http_body}}; }
    if (method == "net.delete_filament_spools") { auto f = net<int (*)(void*, FilamentDeleteParams, std::string*)>("bambu_network_delete_filament_spools"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); FilamentDeleteParams params; params.ids = payload.value("ids", std::vector<std::string>()); params.rfids = payload.value("rfids", std::vector<std::string>()); std::string http_body; const int ret = f(a, params, &http_body); return {{"ok", true}, {"value", ret}, {"http_body", http_body}}; }
    if (method == "net.get_filament_config") { auto f = net<int (*)(void*, std::string*)>("bambu_network_get_filament_config"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string http_body; const int ret = f(a, &http_body); return {{"ok", true}, {"value", ret}, {"http_body", http_body}}; }
    if (method == "net.sync_ams_filaments") {
        auto f = net<int (*)(void*, AmsSyncParams, std::string*)>("bambu_network_sync_ams_filaments");
        auto a = lookup_agent();
        if (!f || !a)
            return not_supported(method);

        AmsSyncParams params;
        params.devId = payload.value("dev_id", std::string());
        for (const auto& value : payload.value("items", nlohmann::json::array())) {
            if (!value.is_object())
                continue;
            AmsSyncItem item;
            item.RFID = value.value("RFID", std::string());
            item.filamentVendor = value.value("filamentVendor", std::string());
            item.filamentType = value.value("filamentType", std::string());
            item.filamentName = value.value("filamentName", std::string());
            item.filamentId = value.value("filamentId", std::string());
            item.isSupport = value.value("isSupport", false);
            item.color = value.value("color", std::string());
            item.colorType = value.value("colorType", 0);
            item.colors = value.value("colors", std::vector<std::string>());
            item.netWeight = value.value("netWeight", 0);
            item.totalNetWeight = value.value("totalNetWeight", 0);
            item.trayIdName = value.value("trayIdName", std::string());
            item.note = value.value("note", std::string());
            item.amsSn = value.value("amsSn", std::string());
            item.slotId = value.value("slotId", std::string());
            item.amsId = value.value("amsId", 0);
            item.amsType = value.value("amsType", 0);
            item.createNew = value.value("createNew", false);
            params.items.push_back(std::move(item));
        }

        std::string http_body;
        const int ret = f(a, std::move(params), &http_body);
        return {{"ok", true}, {"value", ret}, {"http_body", http_body}};
    }
    if (method == "net.get_printer_firmware") { auto f = net<int (*)(void*, std::string, unsigned*, std::string*)>("bambu_network_get_printer_firmware"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned http_code = 0; std::string http_body; const int ret = f(a, payload.value("dev_id", std::string()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_my_profile") { auto f = net<int (*)(void*, std::string, unsigned int*, std::string*)>("bambu_network_get_my_profile"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("token", std::string()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.request_setting_id") { auto f = net<std::string (*)(void*, std::string, std::map<std::string, std::string>*, unsigned int*)>("bambu_network_request_setting_id"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto values = json_to_string_map(payload.value("values", nlohmann::json::object())); unsigned int http_code = 0; std::string setting_id = f(a, payload.value("name", std::string()), &values, &http_code); return {{"ok", true}, {"value", 0}, {"setting_id", setting_id}, {"http_code", http_code}}; }
    if (method == "net.get_user_presets") { auto f = net<int (*)(void*, std::map<std::string, std::map<std::string, std::string>>*)>("bambu_network_get_user_presets"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::map<std::string, std::map<std::string, std::string>> user_presets; const int ret = f(a, &user_presets); return {{"ok", true}, {"value", ret}, {"user_presets", nested_string_map_to_json(user_presets)}}; }
    if (method == "net.get_setting_list") { auto f = net<int (*)(void*, std::string, ProgressFn, WasCancelledFn)>("bambu_network_get_setting_list"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); const auto job_id = payload.value("client_job_id", 0LL); const auto params = payload.value("params", nlohmann::json::object()); auto job = std::make_shared<HostJobState>(); job->job_id = job_id; job->agent_handle = agent_id; job->kind = "get_setting_list"; register_job(job); const int ret = f(a, params.value("bundle_version", std::string()), [this, job](int progress) { queue_event(job->agent_handle, "job.progress", {{"job_id", job->job_id}, {"kind", job->kind}, {"progress", progress}}); }, [job]() { return job->cancel_requested.load(); }); unregister_job(job_id); return {{"ok", true}, {"value", ret}, {"job_id", job_id}}; }
    if (method == "net.get_setting_list2") { auto f = net<int (*)(void*, std::string, CheckFn, ProgressFn, WasCancelledFn)>("bambu_network_get_setting_list2"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); const auto job_id = payload.value("client_job_id", 0LL); const auto params = payload.value("params", nlohmann::json::object()); auto job = std::make_shared<HostJobState>(); job->job_id = job_id; job->agent_handle = agent_id; job->kind = "get_setting_list2"; register_job(job); const int ret = f(a, params.value("bundle_version", std::string()), [this, job](std::map<std::string, std::string> info) { const auto request_id = m_next_wait_request.fetch_add(1); { std::lock_guard<std::mutex> lock(job->wait_mutex); job->wait_request_id = request_id; job->wait_reply_ready = false; job->wait_reply_value = true; } queue_event(job->agent_handle, "job.check", {{"job_id", job->job_id}, {"kind", job->kind}, {"request_id", request_id}, {"info", info}}); std::unique_lock<std::mutex> lock(job->wait_mutex); job->wait_cv.wait(lock, [&] { return job->wait_reply_ready && job->wait_request_id == request_id; }); return job->wait_reply_value; }, [this, job](int progress) { queue_event(job->agent_handle, "job.progress", {{"job_id", job->job_id}, {"kind", job->kind}, {"progress", progress}}); }, [job]() { return job->cancel_requested.load(); }); unregister_job(job_id); return {{"ok", true}, {"value", ret}, {"job_id", job_id}}; }
    if (method == "net.put_setting") { auto f = net<int (*)(void*, std::string, std::string, std::map<std::string, std::string>*, unsigned int*)>("bambu_network_put_setting"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto values = json_to_string_map(payload.value("values", nlohmann::json::object())); unsigned int http_code = 0; const int ret = f(a, payload.value("setting_id", std::string()), payload.value("name", std::string()), &values, &http_code); return {{"ok", true}, {"value", ret}, {"http_code", http_code}}; }
    if (method == "net.delete_setting") { auto f = net<int (*)(void*, std::string)>("bambu_network_delete_setting"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("setting_id", std::string()))}} : not_supported(method); }
    if (method == "net.set_extra_http_header") { auto f = net<int (*)(void*, std::map<std::string, std::string>)>("bambu_network_set_extra_http_header"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto headers = json_to_string_map(payload.value("headers", nlohmann::json::object())); const int ret = f(a, headers); nlohmann::json r{{"ok", true}, {"value", ret}, {"headers", headers}}; host_log_json("net.set_extra_http_header", r); return r; }
    if (method == "net.get_my_message") { auto f = net<int (*)(void*, int, int, int, unsigned int*, std::string*)>("bambu_network_get_my_message"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("type", 0), payload.value("after", 0), payload.value("limit", 20), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.check_user_task_report") { auto f = net<int (*)(void*, int*, bool*)>("bambu_network_check_user_task_report"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); int task_id = 0; bool printable = false; const int ret = f(a, &task_id, &printable); return {{"ok", true}, {"value", ret}, {"task_id", task_id}, {"printable", printable}}; }
    if (method == "net.get_user_print_info") { auto f = net<int (*)(void*, unsigned int*, std::string*)>("bambu_network_get_user_print_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_user_tasks") { auto f = net<int (*)(void*, TaskQueryParams, std::string*)>("bambu_network_get_user_tasks"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); auto params = task_query_from_json(payload.value("params", nlohmann::json::object())); std::string http_body; const int ret = f(a, params, &http_body); return {{"ok", true}, {"value", ret}, {"http_body", http_body}}; }
    if (method == "net.get_subtask_info") { auto f = net<int (*)(void*, std::string, std::string*, unsigned int*, std::string*)>("bambu_network_get_subtask_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string task_json; unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("subtask_id", std::string()), &task_json, &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"task_json", task_json}, {"http_code", http_code}, {"http_body", http_body}}; }
    if (method == "net.get_slice_info") { auto f = net<int (*)(void*, std::string, std::string, int, std::string*)>("bambu_network_get_slice_info"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string slice_json; const int ret = f(a, payload.value("project_id", std::string()), payload.value("profile_id", std::string()), payload.value("plate_index", 0), &slice_json); return {{"ok", true}, {"value", ret}, {"slice_json", slice_json}}; }
    if (method == "net.get_camera_url") {
        auto f = net<int (*)(void*, std::string, std::function<void(std::string)>)>("bambu_network_get_camera_url");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const std::string dev_id = payload.value("dev_id", std::string());
        auto r = wait_string_callback([&](auto cb) { return f(a, dev_id, cb); });
        const std::string result = r.value("result", std::string());
        host_log_json("net.get_camera_url.result", {{"dev_id", dev_id}, {"value", r.value("value", -9999)}, {"result_len", result.size()}, {"result_is_bambu", result.rfind("bambu:///", 0) == 0}});
        return r;
    }
    if (method == "net.get_camera_url_for_golive") {
        auto f = net<int (*)(void*, std::string, std::string, std::function<void(std::string)>)>("bambu_network_get_camera_url_for_golive");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const std::string dev_id = payload.value("dev_id", std::string());
        const std::string sdev_id = payload.value("sdev_id", std::string());
        auto r = wait_string_callback([&](auto cb) { return f(a, dev_id, sdev_id, cb); });
        const std::string result = r.value("result", std::string());
        host_log_json("net.get_camera_url_for_golive.result", {{"dev_id", dev_id}, {"sdev_id", sdev_id}, {"value", r.value("value", -9999)}, {"result_len", result.size()}, {"result_is_bambu", result.rfind("bambu:///", 0) == 0}});
        return r;
    }
    if (method == "net.get_design_staffpick") { auto f = net<int (*)(void*, int, int, std::function<void(std::string)>)>("bambu_network_get_design_staffpick"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, payload.value("offset", 0), payload.value("limit", 0), cb); }); }
    if (method == "net.start_publish") {
        auto f = net<int (*)(void*, BBL::PublishParams, OnUpdateStatusFn, WasCancelledFn, std::string*)>("bambu_network_start_publish");
        auto a = lookup_agent();
        if (!f || !a) return not_supported(method);
        const auto job_id = payload.value("client_job_id", 0LL);
        auto params = JsonRuntime::publish_params_from_json(payload.value("params", nlohmann::json::object()));
        translate_publish_params_paths(params);
        auto job = std::make_shared<HostJobState>();
        job->job_id = job_id;
        job->agent_handle = agent_id;
        job->kind = "start_publish";
        register_job(job);
        std::string out;
        const int ret = f(a, params,
            [this, job](int status, int code, std::string msg) {
                queue_event(job->agent_handle, "job.update_status", {{"job_id", job->job_id}, {"kind", job->kind}, {"status", status}, {"code", code}, {"msg", msg}});
            },
            [job]() {
                return job->cancel_requested.load();
            },
            &out);
        unregister_job(job_id);
        return {{"ok", true}, {"value", ret}, {"job_id", job_id}, {"out", out}};
    }
    if (method == "net.get_model_publish_url") { auto f = net<int (*)(void*, std::string*)>("bambu_network_get_model_publish_url"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string url; const int ret = f(a, &url); return {{"ok", true}, {"value", ret}, {"url", url}}; }
    if (method == "net.get_model_mall_home_url") { auto f = net<int (*)(void*, std::string*)>("bambu_network_get_model_mall_home_url"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string url; const int ret = f(a, &url); return {{"ok", true}, {"value", ret}, {"url", url}}; }
    if (method == "net.get_model_mall_detail_url") { auto f = net<int (*)(void*, std::string*, std::string)>("bambu_network_get_model_mall_detail_url"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string url; const int ret = f(a, &url, payload.value("id", std::string())); return {{"ok", true}, {"value", ret}, {"url", url}}; }
    if (method == "net.get_subtask") { auto f = net<int (*)(void*, Slic3r::BBLModelTask*, OnGetSubTaskFn)>("bambu_network_get_subtask"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); Slic3r::BBLModelTask task{}; if (payload.contains("task") && payload["task"].is_object()) json_to_model_task(payload["task"], task); return wait_model_task_callback([&](auto cb) { return f(a, &task, cb); }); }
    if (method == "net.put_model_mall_rating") { auto f = net<int (*)(void*, int, int, std::string, std::vector<std::string>, unsigned int&, std::string&)>("bambu_network_put_model_mall_rating"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_error; const int ret = f(a, payload.value("rating_id", 0), payload.value("score", 0), payload.value("content", std::string()), windows_paths_to_wsl(payload.value("images", std::vector<std::string>())), http_code, http_error); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.get_oss_config") { auto f = net<int (*)(void*, std::string&, std::string, unsigned int&, std::string&)>("bambu_network_get_oss_config"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string config; unsigned int http_code = 0; std::string http_error; const int ret = f(a, config, payload.value("country_code", std::string()), http_code, http_error); return {{"ok", true}, {"value", ret}, {"config", config}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.put_rating_picture_oss") { auto f = net<int (*)(void*, std::string&, std::string&, std::string, int, unsigned int&, std::string&)>("bambu_network_put_rating_picture_oss"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string config = payload.value("config", std::string()); std::string pic_oss_path = windows_path_to_wsl(payload.value("pic_oss_path", std::string())); unsigned int http_code = 0; std::string http_error; const int ret = f(a, config, pic_oss_path, payload.value("model_id", std::string()), payload.value("profile_id", 0), http_code, http_error); return {{"ok", true}, {"value", ret}, {"config", config}, {"pic_oss_path", pic_oss_path}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.get_model_mall_rating") { auto f = net<int (*)(void*, int, std::string&, unsigned int&, std::string&)>("bambu_network_get_model_mall_rating"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string rating_result; unsigned int http_code = 0; std::string http_error; const int ret = f(a, payload.value("job_id", 0), rating_result, http_code, http_error); return {{"ok", true}, {"value", ret}, {"rating_result", rating_result}, {"http_code", http_code}, {"http_error", http_error}}; }
    if (method == "net.get_mw_user_preference") { auto f = net<int (*)(void*, std::function<void(std::string)>)>("bambu_network_get_mw_user_preference"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, cb); }); }
    if (method == "net.get_mw_user_4ulist") { auto f = net<int (*)(void*, int, int, std::function<void(std::string)>)>("bambu_network_get_mw_user_4ulist"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); return wait_string_callback([&](auto cb) { return f(a, payload.value("seed", 0), payload.value("limit", 0), cb); }); }
    if (method == "net.get_hms_snapshot") { auto f = net<int (*)(void*, std::string&, std::string&, std::function<void(std::string, int)>)>("bambu_network_get_hms_snapshot"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string dev_id = payload.value("dev_id", std::string()); std::string file_name = windows_path_to_wsl(payload.value("file_name", std::string())); return wait_string_int_callback([&](auto cb) { return f(a, dev_id, file_name, cb); }); }

    if (method == "net.get_my_token") { auto f = net<int (*)(void*, std::string, unsigned int*, std::string*)>("bambu_network_get_my_token"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); unsigned int http_code = 0; std::string http_body; const int ret = f(a, payload.value("ticket", std::string()), &http_code, &http_body); return {{"ok", true}, {"value", ret}, {"http_code", http_code}, {"http_body", http_body}}; }

    if (method == "net.track_enable") { auto f = net<int (*)(void*, bool)>("bambu_network_track_enable"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("enable", false))}} : not_supported(method); }
    if (method == "net.track_remove_files") { auto f = net<int (*)(void*)>("bambu_network_track_remove_files"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a)}} : not_supported(method); }
    if (method == "net.track_event") { auto f = net<int (*)(void*, std::string, std::string)>("bambu_network_track_event"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("evt_key", std::string()), payload.value("content", std::string()))}} : not_supported(method); }
    if (method == "net.track_header") { auto f = net<int (*)(void*, std::string)>("bambu_network_track_header"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("header", std::string()))}} : not_supported(method); }
    if (method == "net.track_update_property") { auto f = net<int (*)(void*, std::string, std::string, std::string)>("bambu_network_track_update_property"); auto a = lookup_agent(); return f && a ? nlohmann::json{{"ok", true}, {"value", f(a, payload.value("name", std::string()), payload.value("value", std::string()), payload.value("type", std::string()))}} : not_supported(method); }
    if (method == "net.track_get_property") { auto f = net<int (*)(void*, std::string, std::string&, std::string)>("bambu_network_track_get_property"); auto a = lookup_agent(); if (!f || !a) return not_supported(method); std::string value; const int ret = f(a, payload.value("name", std::string()), value, payload.value("type", std::string())); return {{"ok", true}, {"value", ret}, {"property_value", value}}; }

    if (method == "ft.capabilities") {
        return {
            {"ok", true},
            {"ft_abi_version", has_component_symbol("ft_abi_version")},
            {"ft_tunnel_create", has_component_symbol("ft_tunnel_create")},
            {"ft_tunnel_sync_connect", has_component_symbol("ft_tunnel_sync_connect")},
            {"ft_tunnel_release", has_component_symbol("ft_tunnel_release")},
            {"ft_tunnel_shutdown", has_component_symbol("ft_tunnel_shutdown")},
            {"ft_job_create", has_component_symbol("ft_job_create")},
            {"ft_job_release", has_component_symbol("ft_job_release")},
            {"ft_job_set_result_cb", has_component_symbol("ft_job_set_result_cb")},
            {"ft_job_get_result", has_component_symbol("ft_job_get_result")},
            {"ft_tunnel_start_job", has_component_symbol("ft_tunnel_start_job")},
            {"ft_job_cancel", has_component_symbol("ft_job_cancel")},
            {"ft_job_set_msg_cb", has_component_symbol("ft_job_set_msg_cb")},
            {"ft_job_try_get_msg", has_component_symbol("ft_job_try_get_msg")},
            {"ft_job_get_msg", has_component_symbol("ft_job_get_msg")}
        };
    }
    if (method == "ft.tunnel_create") {
        auto f = net<fn_ft_tunnel_create>("ft_tunnel_create");
        if (!f) return not_supported(method);
        FT_TunnelHandle* tunnel = nullptr;
        const std::string raw_url = payload.value("url", std::string());
        const std::string url = raw_url.rfind("bambu:///", 0) == 0
            ? replace_url_param_value(raw_url, "refresh_url", refresh_agora_url_ptr_string())
            : raw_url;
        const int ret = static_cast<int>(f(url.c_str(), &tunnel));
        if (ret != 0 || !tunnel)
            return {{"ok", true}, {"value", ret}, {"tunnel", 0}};
        const auto id = m_next_ft_tunnel.fetch_add(1);
        { std::lock_guard<std::mutex> lock(m_state_mutex); m_ft_tunnels[id] = make_host_resource(tunnel); }
        return {{"ok", true}, {"value", ret}, {"tunnel", id}};
    }
    if (method == "ft.tunnel_sync_connect") {
        auto f = net<fn_ft_tunnel_sync_connect>("ft_tunnel_sync_connect");
        auto t = lookup_ft_tunnel();
        return f && t ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(t))}} : not_supported(method);
    }
    if (method == "ft.tunnel_shutdown") {
        auto f = net<fn_ft_tunnel_shutdown>("ft_tunnel_shutdown");
        auto t = lookup_ft_tunnel();
        return f && t ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(t))}} : not_supported(method);
    }
    if (method == "ft.tunnel_release") {
        auto f = net<fn_ft_tunnel_release>("ft_tunnel_release");
        if (!f) return not_supported(method);
        const auto id = payload.value("tunnel", 0LL);
        auto resource = detach_resource(m_ft_tunnels, m_state_mutex, id);
        if (!resource)
            return {{"ok", false}, {"error", "tunnel not found"}};
        auto* tunnel = static_cast<FT_TunnelHandle*>(wait_and_take_resource(resource));
        if (!tunnel)
            return {{"ok", false}, {"error", "tunnel not found"}};
        f(tunnel);
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "ft.job_create") {
        auto f = net<fn_ft_job_create>("ft_job_create");
        auto set_result_cb = net<fn_ft_job_set_result_cb>("ft_job_set_result_cb");
        auto set_msg_cb = net<fn_ft_job_set_msg_cb>("ft_job_set_msg_cb");
        auto free_result = net<fn_ft_job_result_destroy>("ft_job_result_destroy");
        auto free_msg = net<fn_ft_job_msg_destroy>("ft_job_msg_destroy");
        auto free_mem = net<fn_ft_free>("ft_free");
        if (!f) return not_supported(method);
        FT_JobHandle* job = nullptr;
        const int ret = static_cast<int>(f(payload.value("params_json", std::string()).c_str(), &job));
        if (ret != 0 || !job)
            return {{"ok", true}, {"value", ret}, {"job", 0}};
        const auto id = m_next_ft_job.fetch_add(1);
        auto state = std::make_shared<HostFtJobState>();
        state->handle = job;
        state->result_destroy = reinterpret_cast<void*>(free_result);
        state->msg_destroy = reinterpret_cast<void*>(free_msg);
        state->free_mem = reinterpret_cast<void*>(free_mem);
        if (set_result_cb) {
            const int callback_ret = static_cast<int>(set_result_cb(job, [](void* user, ft_job_result result) noexcept {
                auto* state = static_cast<HostFtJobState*>(user);
                if (!state)
                    return;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    ++state->active_callbacks;
                }
                try {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->shutting_down) {
                        copy_ft_job_result_payload(*state, result);
                        state->result_ready = true;
                    }
                } catch (...) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->shutting_down) {
                        state->result_ec = -1;
                        state->result_resp_ec = -1;
                        state->result_json.clear();
                        state->result_bin.clear();
                        state->result_ready = true;
                    }
                }
                if (auto destroy = reinterpret_cast<fn_ft_job_result_destroy>(state->result_destroy)) {
                    destroy(&result);
                } else if (auto free_mem = reinterpret_cast<fn_ft_free>(state->free_mem)) {
                    if (result.json) free_mem((void*) result.json);
                    if (result.bin) free_mem((void*) result.bin);
                }
                finish_ft_callback(state);
            }, state.get()));
            state->result_callback_enabled = callback_ret == 0;
        }
        if (set_msg_cb) {
            const int callback_ret = static_cast<int>(set_msg_cb(job, [](void* user, ft_job_msg msg) noexcept {
                auto* state = static_cast<HostFtJobState*>(user);
                if (!state)
                    return;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    ++state->active_callbacks;
                }
                try {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (!state->shutting_down)
                        state->messages.emplace_back(msg.kind, std::string(msg.json ? msg.json : ""));
                } catch (...) {
                }
                if (auto destroy = reinterpret_cast<fn_ft_job_msg_destroy>(state->msg_destroy)) {
                    destroy(&msg);
                } else if (auto free_mem = reinterpret_cast<fn_ft_free>(state->free_mem)) {
                    if (msg.json) free_mem((void*) msg.json);
                }
                finish_ft_callback(state);
            }, state.get()));
            state->msg_callback_enabled = callback_ret == 0;
        }
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_ft_jobs[id] = make_host_resource(job);
            m_ft_job_states[id] = state;
        }
        return {{"ok", true}, {"value", ret}, {"job", id}};
    }
    if (method == "ft.job_start") {
        auto f = net<fn_ft_tunnel_start_job>("ft_tunnel_start_job");
        auto t = lookup_ft_tunnel();
        auto j = lookup_ft_job();
        return f && t && j ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(t, j))}} : not_supported(method);
    }
    if (method == "ft.job_cancel") {
        auto f = net<fn_ft_job_cancel>("ft_job_cancel");
        auto j = lookup_ft_job();
        return f && j ? nlohmann::json{{"ok", true}, {"value", static_cast<int>(f(j))}} : not_supported(method);
    }
    if (method == "ft.job_get_result") {
        auto f = net<fn_ft_job_get_result>("ft_job_get_result");
        auto free_result = net<fn_ft_job_result_destroy>("ft_job_result_destroy");
        auto free_mem = net<fn_ft_free>("ft_free");
        const auto job_id = payload.value("job", 0LL);
        auto j = lookup_ft_job();
        auto state = lookup_ft_job_state(m_ft_job_states, m_state_mutex, job_id);
        if (!j) return not_supported(method);
        const auto timeout_ms = payload.value("timeout_ms", 0U);
        if (state && state->result_callback_enabled) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (!state->result_ready) {
                if (timeout_ms == 0)
                    state->cv.wait(lock, [&state] { return state->result_ready || state->shutting_down; });
                else
                    state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&state] { return state->result_ready || state->shutting_down; });
            }
            if (state->shutting_down && !state->result_ready) {
                clear_thread_reply_binary();
                return {{"ok", true}, {"value", -5}, {"binary_size", 0}};
            }
            if (state->result_ready) {
                nlohmann::json out{{"ok", true}, {"value", 0}, {"ec", state->result_ec}, {"resp_ec", state->result_resp_ec}, {"json", state->result_json}};
                if (!state->result_bin.empty()) {
                    g_thread_reply_binary = state->result_bin;
                    out["binary_size"] = state->result_bin.size();
                    out["__binary_pending"] = true;
                } else {
                    clear_thread_reply_binary();
                    out["binary_size"] = 0;
                }
                return out;
            }
            clear_thread_reply_binary();
            return {{"ok", true}, {"value", -4}, {"binary_size", 0}};
        }
        if (!f) return not_supported(method);
        ft_job_result result{};
        const int ret = static_cast<int>(f(j, timeout_ms, &result));
        nlohmann::json out{{"ok", true}, {"value", ret}};
        if (ret == 0) {
            out["ec"] = result.ec;
            out["resp_ec"] = result.resp_ec;
            out["json"] = std::string(result.json ? result.json : "");
            if (result.bin_size > kMaxFtPayloadBytes || (result.bin_size != 0 && !result.bin)) {
                out["value"] = -1;
                out["ec"] = -1;
                out["resp_ec"] = -1;
                out["json"] = "invalid file-transfer payload";
                clear_thread_reply_binary();
                out["binary_size"] = 0;
            } else if (result.bin_size) {
                g_thread_reply_binary.assign(static_cast<const unsigned char*>(result.bin), static_cast<const unsigned char*>(result.bin) + result.bin_size);
                out["binary_size"] = result.bin_size;
                out["__binary_pending"] = true;
            } else {
                clear_thread_reply_binary();
                out["binary_size"] = 0;
            }
            if (free_result) free_result(&result);
            else if (free_mem) {
                if (result.json) free_mem((void*) result.json);
                if (result.bin) free_mem((void*) result.bin);
            }
        }
        return out;
    }
    if (method == "ft.job_try_get_msg" || method == "ft.job_get_msg") {
        auto f_try = net<fn_ft_job_try_get_msg>("ft_job_try_get_msg");
        auto f_get = net<fn_ft_job_get_msg>("ft_job_get_msg");
        auto free_msg = net<fn_ft_job_msg_destroy>("ft_job_msg_destroy");
        auto free_mem = net<fn_ft_free>("ft_free");
        const auto job_id = payload.value("job", 0LL);
        auto j = lookup_ft_job();
        auto state = lookup_ft_job_state(m_ft_job_states, m_state_mutex, job_id);
        if (!j) return not_supported(method);
        if (state && state->msg_callback_enabled) {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (method == "ft.job_get_msg" && state->messages.empty() && !state->result_ready) {
                const auto timeout_ms = payload.value("timeout_ms", 0U);
                if (timeout_ms == 0)
                    state->cv.wait(lock, [&state] { return !state->messages.empty() || state->result_ready || state->shutting_down; });
                else
                    state->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&state] { return !state->messages.empty() || state->result_ready || state->shutting_down; });
            }
            if (state->shutting_down && state->messages.empty() && !state->result_ready)
                return {{"ok", true}, {"value", -5}};
            if (!state->messages.empty()) {
                auto msg = std::move(state->messages.front());
                state->messages.pop_front();
                return {{"ok", true}, {"value", 0}, {"kind", msg.first}, {"json", msg.second}};
            }
            if (state->result_ready)
                return {{"ok", true}, {"value", -2}};
        }
        ft_job_msg msg{};
        int ret = -1;
        if (method == "ft.job_try_get_msg") {
            if (!f_try) return not_supported(method);
            ret = static_cast<int>(f_try(j, &msg));
        } else {
            if (!f_get) return not_supported(method);
            ret = static_cast<int>(f_get(j, payload.value("timeout_ms", 0U), &msg));
        }
        nlohmann::json out{{"ok", true}, {"value", ret}};
        if (ret == 0) {
            out["kind"] = msg.kind;
            out["json"] = std::string(msg.json ? msg.json : "");
            if (free_msg) free_msg(&msg);
            else if (free_mem && msg.json) free_mem((void*) msg.json);
        }
        return out;
    }
    if (method == "ft.job_release") {
        auto f = net<fn_ft_job_release>("ft_job_release");
        if (!f) return not_supported(method);
        const auto id = payload.value("job", 0LL);
        auto resource = detach_resource(m_ft_jobs, m_state_mutex, id);
        if (!resource)
            return {{"ok", false}, {"error", "job not found"}};
        std::shared_ptr<HostFtJobState> state;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_ft_job_states.find(id);
            if (it != m_ft_job_states.end()) {
                state = it->second;
                m_ft_job_states.erase(it);
            }
        }
        auto* job = static_cast<FT_JobHandle*>(wait_and_take_resource(resource));
        if (!job)
            return {{"ok", false}, {"error", "job not found"}};
        if (state) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->shutting_down = true;
            }
            state->cv.notify_all();
        }
        if (state && state->result_callback_enabled) {
            if (auto clear_result = net<fn_ft_job_set_result_cb>("ft_job_set_result_cb"))
                (void) clear_result(job, nullptr, nullptr);
        }
        if (state && state->msg_callback_enabled) {
            if (auto clear_msg = net<fn_ft_job_set_msg_cb>("ft_job_set_msg_cb"))
                (void) clear_msg(job, nullptr, nullptr);
        }
        if (state) {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->cv.wait(lock, [&state] { return state->active_callbacks == 0; });
        }
        f(job);
        return {{"ok", true}, {"value", 0}};
    }

    if (method == "src.init") { auto f = src<int (*)()>("Bambu_Init"); return f ? nlohmann::json{{"ok", true}, {"value", f()}} : nlohmann::json{{"ok", true}, {"value", 0}}; }
    if (method == "src.deinit") { auto f = src<void (*)()>("Bambu_Deinit"); if (f) f(); return {{"ok", true}, {"value", 0}}; }
    if (method == "src.get_last_error_msg") { auto f = src<const char* (*)()>("Bambu_GetLastErrorMsg"); const char* message = f ? f() : nullptr; return nlohmann::json{{"ok", true}, {"message", std::string(message ? message : "")}}; }
    if (method == "src.free_log_msg") { return {{"ok", true}, {"value", 0}}; }
    if (method == "src.create") {
        auto f = src<int (*)(Bambu_Tunnel*, const char*)>("Bambu_Create");
        if (!f) return not_supported(method);
        Bambu_Tunnel tunnel = nullptr;
        const std::string raw_path = windows_path_to_wsl(payload.value("path", std::string()));
        const std::string path = raw_path.rfind("bambu:///", 0) == 0
            ? replace_url_param_value(raw_path, "refresh_url", refresh_agora_url_ptr_string())
            : raw_path;
        nlohmann::json create_meta{{"path_len", path.size()},
                                   {"path_is_bambu", path.rfind("bambu:///", 0) == 0},
                                   {"path_is_tutk", path.rfind("bambu:///tutk", 0) == 0},
                                   {"path_is_local", path.rfind("bambu:///local", 0) == 0},
                                   {"has_refresh_url", path.find("refresh_url=") != std::string::npos}};
        host_log_json("src.create.begin", create_meta);
        const int ret = f(&tunnel, path.c_str());
        nlohmann::json log_payload = create_meta;
        log_payload["value"] = ret;
        if (ret != 0) {
            log_payload["tunnel"] = 0;
            host_log_json("src.create", log_payload);
            return {{"ok", true}, {"value", ret}, {"tunnel", 0}};
        }
        const auto id = m_next_tunnel.fetch_add(1);
        { std::lock_guard<std::mutex> lock(m_state_mutex); m_tunnels[id] = make_host_resource(tunnel); }
        log_payload["tunnel"] = id;
        host_log_json("src.create", log_payload);
        return {{"ok", true}, {"value", ret}, {"tunnel", id}};
    }
    if (method == "src.open") {
        auto f = src<int (*)(Bambu_Tunnel)>("Bambu_Open");
        auto t = lookup_tunnel();
        if (!f || !t) return not_supported(method);
        const int ret = f(t);
        host_log_json("src.open", {{"value", ret}, {"tunnel", payload.value("tunnel", 0LL)}});
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "src.start_stream") {
        auto f = src<int (*)(Bambu_Tunnel, bool)>("Bambu_StartStream");
        auto t = lookup_tunnel();
        if (!f || !t) return not_supported(method);
        const bool video = payload.value("video", false);
        const int ret = f(t, video);
        host_log_json("src.start_stream", {{"value", ret}, {"video", video}, {"tunnel", payload.value("tunnel", 0LL)}});
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "src.start_stream_ex") {
        auto f = src<int (*)(Bambu_Tunnel, int)>("Bambu_StartStreamEx");
        auto t = lookup_tunnel();
        if (!f || !t) return not_supported(method);
        const int type = payload.value("type", 0);
        const int ret = f(t, type);
        host_log_json("src.start_stream_ex", {{"value", ret}, {"type", type}, {"tunnel", payload.value("tunnel", 0LL)}});
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "src.get_stream_count") {
        auto f = src<int (*)(Bambu_Tunnel)>("Bambu_GetStreamCount");
        auto t = lookup_tunnel();
        if (!f || !t) return not_supported(method);
        const int ret = f(t);
        host_log_json("src.get_stream_count", {{"value", ret}, {"tunnel", payload.value("tunnel", 0LL)}});
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "src.get_stream_info") {
        auto f = src<int (*)(Bambu_Tunnel, int, Bambu_StreamInfo*)>("Bambu_GetStreamInfo");
        auto t = lookup_tunnel();
        if (!f || !t) return not_supported(method);
        const int index = payload.value("index", 0);
        Bambu_StreamInfo info{};
        const int ret = f(t, index, &info);
        nlohmann::json out{{"ok", true}, {"value", ret}, {"binary_size", 0}};
        nlohmann::json log_payload{{"value", ret}, {"index", index}, {"tunnel", payload.value("tunnel", 0LL)}};
        if (ret == 0) {
            nlohmann::json ji{{"type", info.type}, {"sub_type", info.sub_type}, {"format_type", info.format_type}, {"format_size", info.format_size}, {"max_frame_size", info.max_frame_size}};
            if (info.type == VIDE) ji.update({{"width", info.format.video.width}, {"height", info.format.video.height}, {"frame_rate", info.format.video.frame_rate}});
            else ji.update({{"sample_rate", info.format.audio.sample_rate}, {"channel_count", info.format.audio.channel_count}, {"sample_size", info.format.audio.sample_size}});
            if (info.format_buffer && info.format_size > 0 && info.format_size <= 16 * 1024 * 1024) {
                g_thread_reply_binary.assign(info.format_buffer, info.format_buffer + static_cast<std::size_t>(info.format_size));
                out["binary_size"] = g_thread_reply_binary.size();
                out["__binary_pending"] = true;
            } else {
                ji["format_size"] = 0;
            }
            out["info"] = ji;
            log_payload.update({{"type", info.type}, {"sub_type", info.sub_type}, {"format_type", info.format_type}, {"width", info.type == VIDE ? info.format.video.width : 0}, {"height", info.type == VIDE ? info.format.video.height : 0}, {"frame_rate", info.type == VIDE ? info.format.video.frame_rate : 0}});
        }
        host_log_json("src.get_stream_info", log_payload);
        return out;
    }
    if (method == "src.get_duration") { auto f = src<unsigned long (*)(Bambu_Tunnel)>("Bambu_GetDuration"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t)}} : not_supported(method); }
    if (method == "src.seek") { auto f = src<int (*)(Bambu_Tunnel, unsigned long)>("Bambu_Seek"); auto t = lookup_tunnel(); return f && t ? nlohmann::json{{"ok", true}, {"value", f(t, payload.value("time", 0UL))}} : not_supported(method); }
    if (method == "src.send_message") {
        auto f = src<int (*)(Bambu_Tunnel, int, const char*, int)>("Bambu_SendMessage");
        auto t = lookup_tunnel();
        if (!f || !t)
            return not_supported(method);

        std::string fallback = payload.value("data", std::string());
        const char* data_ptr = fallback.c_str();
        int data_len = static_cast<int>(fallback.size());

        if (payload.value("__binary_request", false)) {
            data_ptr = reinterpret_cast<const char*>(g_thread_request_binary.data());
            data_len = static_cast<int>(g_thread_request_binary.size());
        }

        const int ret = f(t, payload.value("ctrl", 0), data_ptr, data_len);
        g_thread_request_binary.clear();
        return {{"ok", true}, {"value", ret}};
    }
    if (method == "src.recv_message") {
        auto f = src<int (*)(Bambu_Tunnel, int*, char*, int*)>("Bambu_RecvMessage");
        auto t = lookup_tunnel();
        if (!f || !t)
            return not_supported(method);

        int ctrl = 0;
        int len = payload.value("buffer_size", 65536);
        if (len < 0 || len > 16 * 1024 * 1024)
            return {{"ok", false}, {"error", "invalid receive buffer size"}, {"value", -1}};
        std::vector<unsigned char> buffer(static_cast<std::size_t>(len), 0);
        char* buffer_ptr = buffer.empty() ? nullptr : reinterpret_cast<char*>(buffer.data());

        const int ret = f(t, &ctrl, buffer_ptr, &len);
        nlohmann::json out{{"ok", true}, {"value", ret}, {"ctrl", ctrl}, {"binary_size", 0}};

        if (ret == 0 && len >= 0 && static_cast<std::size_t>(len) <= buffer.size()) {
            out["message_len"] = len;
            if (len > 0) {
                g_thread_reply_binary.assign(buffer.begin(), buffer.begin() + static_cast<std::size_t>(len));
                out["binary_size"] = g_thread_reply_binary.size();
                out["__binary_pending"] = true;
            }
        } else if (ret == 0) {
            clear_thread_reply_binary();
            return {{"ok", false}, {"error", "Bambu_RecvMessage returned an invalid length"}, {"value", -1}, {"required_len", len}};
        } else {
            out["required_len"] = len;
        }

        return out;
    }
    if (method == "src.read_sample") {
        auto f = src<int (*)(Bambu_Tunnel, Bambu_Sample*)>("Bambu_ReadSample");
        auto t = lookup_tunnel();
        if (!f || !t)
            return not_supported(method);

        Bambu_Sample sample{};
        const int ret = f(t, &sample);
        nlohmann::json j{{"ok", true}, {"value", ret}, {"binary_size", 0}};
        if (ret == 0) {
            if (sample.size < 0 || sample.size > 64 * 1024 * 1024 || (sample.size > 0 && !sample.buffer))
                return {{"ok", false}, {"error", "Bambu_ReadSample returned an invalid sample"}, {"value", -1}};
            j["sample"] = {{"itrack", sample.itrack}, {"size", sample.size}, {"flags", sample.flags}, {"decode_time", sample.decode_time}};
            if (sample.size > 0) {
                g_thread_reply_binary.assign(sample.buffer, sample.buffer + static_cast<std::size_t>(sample.size));
                j["binary_size"] = g_thread_reply_binary.size();
                j["__binary_pending"] = true;
            }
        }
        static std::atomic<int> read_sample_log_budget{20};
        int budget = read_sample_log_budget.load(std::memory_order_relaxed);
        while (budget > 0 && !read_sample_log_budget.compare_exchange_weak(budget, budget - 1, std::memory_order_relaxed)) {}
        if (ret != Bambu_would_block || budget > 0)
            host_log_json("src.read_sample", {{"value", ret}, {"size", ret == 0 ? sample.size : 0}, {"flags", ret == 0 ? sample.flags : 0}, {"tunnel", payload.value("tunnel", 0LL)}});
        return j;
    }
    if (method == "src.close") { auto f = src<void (*)(Bambu_Tunnel)>("Bambu_Close"); auto t = lookup_tunnel(); if (!f || !t) return not_supported(method); f(t); return {{"ok", true}, {"value", 0}}; }
    if (method == "src.destroy") {
        auto f = src<void (*)(Bambu_Tunnel)>("Bambu_Destroy");
        const auto id = payload.value("tunnel", 0LL);
        auto resource = detach_resource(m_tunnels, m_state_mutex, id);
        if (!f || !resource)
            return not_supported(method);
        Bambu_Tunnel t = static_cast<Bambu_Tunnel>(wait_and_take_resource(resource));
        if (!t)
            return not_supported(method);

        void* logger_ctx = nullptr;
        void* stream_info_ctx = nullptr;
        void* track_reporter_ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto take_context = [](auto& contexts, std::int64_t context_id) -> void* {
                auto it = contexts.find(context_id);
                if (it == contexts.end())
                    return nullptr;
                void* context = it->second;
                contexts.erase(it);
                return context;
            };
            logger_ctx = take_context(m_logger_contexts, id);
            stream_info_ctx = take_context(m_stream_info_contexts, id);
            track_reporter_ctx = take_context(m_track_reporter_contexts, id);
        }
        if (logger_ctx) {
            if (auto clear_logger = src<void (*)(Bambu_Tunnel, Logger, void*)>("Bambu_SetLogger"))
                clear_logger(t, nullptr, nullptr);
        }
        if (stream_info_ctx) {
            if (auto clear_stream_info = src<void (*)(Bambu_Tunnel, StreamInfoCallback, void*)>("Bambu_SetStreamInfoCallback"))
                clear_stream_info(t, nullptr, nullptr);
        }
        if (track_reporter_ctx) {
            if (auto clear_track_reporter = src<void (*)(Bambu_Tunnel, TrackReporter, void*)>("Bambu_SetTrackReporter"))
                clear_track_reporter(t, nullptr, nullptr);
        }
        std::vector<HostRetiredCallbackContext> retired_contexts;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_retired_callback_contexts.begin();
            while (it != m_retired_callback_contexts.end()) {
                if (it->tunnel_id == id) {
                    retired_contexts.push_back(*it);
                    it = m_retired_callback_contexts.erase(it);
                } else {
                    ++it;
                }
            }
        }
        f(t);
        if (logger_ctx)
            destroy_callback_context<LoggerCallbackContext>(logger_ctx);
        if (stream_info_ctx)
            destroy_callback_context<StreamInfoCallbackContext>(stream_info_ctx);
        if (track_reporter_ctx)
            destroy_callback_context<TrackReporterCallbackContext>(track_reporter_ctx);
        for (auto& context : retired_contexts) {
            if (context.pointer && context.destroy)
                context.destroy(context.pointer);
        }
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "src.set_logger") {
        auto f = src<void (*)(Bambu_Tunnel, Logger, void*)>("Bambu_SetLogger");
        auto free_f = src<void (*)(tchar const*)>("Bambu_FreeLogMsg");
        auto t = lookup_tunnel();
        const auto tunnel_id = payload.value("tunnel", 0LL);
        if (!f || !t)
            return not_supported(method);

        void* old_context = nullptr;
        if (!payload.value("enabled", true)) {
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                auto it = m_logger_contexts.find(tunnel_id);
                if (it != m_logger_contexts.end()) {
                    old_context = it->second;
                    m_logger_contexts.erase(it);
                }
            }
            f(t, nullptr, nullptr);
            if (old_context) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_retired_callback_contexts.push_back({tunnel_id, old_context, &destroy_callback_context<LoggerCallbackContext>});
            }
            return {{"ok", true}, {"value", 0}};
        }

        auto* logger_ctx = new LoggerCallbackContext{this, tunnel_id, free_f};
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_logger_contexts.find(tunnel_id);
            if (it != m_logger_contexts.end()) {
                m_retired_callback_contexts.push_back({tunnel_id, it->second, &destroy_callback_context<LoggerCallbackContext>});
                it->second = logger_ctx;
            } else {
                m_logger_contexts.emplace(tunnel_id, logger_ctx);
            }
        }
        f(t, logger_callback_forwarder, logger_ctx);
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "src.set_stream_info_callback") {
        auto f = src<void (*)(Bambu_Tunnel, StreamInfoCallback, void*)>("Bambu_SetStreamInfoCallback");
        auto t = lookup_tunnel();
        const auto tunnel_id = payload.value("tunnel", 0LL);
        if (!f || !t)
            return not_supported(method);

        void* old_context = nullptr;
        if (!payload.value("enabled", true)) {
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                auto it = m_stream_info_contexts.find(tunnel_id);
                if (it != m_stream_info_contexts.end()) {
                    old_context = it->second;
                    m_stream_info_contexts.erase(it);
                }
            }
            f(t, nullptr, nullptr);
            if (old_context) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_retired_callback_contexts.push_back({tunnel_id, old_context, &destroy_callback_context<StreamInfoCallbackContext>});
            }
            return {{"ok", true}, {"value", 0}};
        }

        auto* callback_ctx = new StreamInfoCallbackContext{this, tunnel_id};
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_stream_info_contexts.find(tunnel_id);
            if (it != m_stream_info_contexts.end()) {
                m_retired_callback_contexts.push_back({tunnel_id, it->second, &destroy_callback_context<StreamInfoCallbackContext>});
                it->second = callback_ctx;
            } else {
                m_stream_info_contexts.emplace(tunnel_id, callback_ctx);
            }
        }
        f(t, stream_info_callback_forwarder, callback_ctx);
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "src.set_track_reporter") {
        auto f = src<void (*)(Bambu_Tunnel, TrackReporter, void*)>("Bambu_SetTrackReporter");
        auto t = lookup_tunnel();
        const auto tunnel_id = payload.value("tunnel", 0LL);
        if (!f || !t)
            return not_supported(method);

        void* old_context = nullptr;
        if (!payload.value("enabled", true)) {
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                auto it = m_track_reporter_contexts.find(tunnel_id);
                if (it != m_track_reporter_contexts.end()) {
                    old_context = it->second;
                    m_track_reporter_contexts.erase(it);
                }
            }
            f(t, nullptr, nullptr);
            if (old_context) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_retired_callback_contexts.push_back({tunnel_id, old_context, &destroy_callback_context<TrackReporterCallbackContext>});
            }
            return {{"ok", true}, {"value", 0}};
        }

        auto* reporter_ctx = new TrackReporterCallbackContext{this, tunnel_id};
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_track_reporter_contexts.find(tunnel_id);
            if (it != m_track_reporter_contexts.end()) {
                m_retired_callback_contexts.push_back({tunnel_id, it->second, &destroy_callback_context<TrackReporterCallbackContext>});
                it->second = reporter_ctx;
            } else {
                m_track_reporter_contexts.emplace(tunnel_id, reporter_ctx);
            }
        }
        f(t, track_reporter_callback_forwarder, reporter_ctx);
        return {{"ok", true}, {"value", 0}};
    }
    if (method == "src.get_session_stat") {
        auto f = src<void (*)(Bambu_Tunnel, Bambu_SessionStat*)>("Bambu_GetSessionStat");
        auto t = lookup_tunnel();
        if (!f || !t)
            return not_supported(method);
        Bambu_SessionStat stat{};
        f(t, &stat);
        return {
            {"ok", true},
            {"session_duration_ms", stat.session_duration_ms},
            {"freeze_total_duration_ms", stat.freeze_total_duration_ms},
            {"freeze_count", stat.freeze_count},
            {"avg_fps", stat.avg_fps},
            {"avg_bitrate_kbps", stat.avg_bitrate_kbps},
            {"avg_jitter_ms", stat.avg_jitter_ms},
            {"max_jitter_ms", stat.max_jitter_ms}
        };
    }

    return not_supported(method);
}

}
