#include "LinuxRuntimeHost.hpp"
#include "../../src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeRpcProtocol.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <curl/curl.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <streambuf>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace Slic3r::SlicerLinuxRuntime;

namespace {

class FdOutputBuffer : public std::streambuf {
public:
    explicit FdOutputBuffer(int fd) : m_fd(fd) { setp(m_buffer, m_buffer + sizeof(m_buffer)); }

    ~FdOutputBuffer() override
    {
        sync();
        if (m_fd >= 0)
            ::close(m_fd);
    }

    FdOutputBuffer(const FdOutputBuffer&) = delete;
    FdOutputBuffer& operator=(const FdOutputBuffer&) = delete;

protected:
    int overflow(int ch) override
    {
        if (flush_buffer() != 0)
            return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return ch;
    }

    int sync() override { return flush_buffer(); }

private:
    int flush_buffer()
    {
        const char* data = pbase();
        std::ptrdiff_t size = pptr() - pbase();
        while (size > 0) {
            const ssize_t written = ::write(m_fd, data, static_cast<std::size_t>(size));
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                return -1;
            }
            if (written == 0)
                return -1;
            data += written;
            size -= written;
        }
        setp(m_buffer, m_buffer + sizeof(m_buffer));
        return 0;
    }

    int m_fd{-1};
    char m_buffer[16384]{};
};

int open_rpc_output(std::unique_ptr<FdOutputBuffer>& buffer, std::unique_ptr<std::ostream>& out)
{
    const int rpc_fd = ::dup(STDOUT_FILENO);
    if (rpc_fd < 0)
        return 100;

    std::fflush(stdout);
    if (::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        ::close(rpc_fd);
        return 102;
    }

    buffer = std::make_unique<FdOutputBuffer>(rpc_fd);
    out = std::make_unique<std::ostream>(buffer.get());
    return out->good() ? 0 : 101;
}

int run_probe_load()
{
    LinuxRuntimeHost host;
    auto hs = host.handle("runtime.handshake", nlohmann::json::object());
    std::cerr << hs.dump() << std::endl;
    if (!hs.value("component_loaded", false))
        return 2;
    if (!hs.value("source_loaded", false))
        return 3;
    host.begin_shutdown();
    return 0;
}

int run_probe_stdio_roundtrip()
{
    std::unique_ptr<FdOutputBuffer> rpc_buffer;
    std::unique_ptr<std::ostream> rpc_out;
    const int rc = open_rpc_output(rpc_buffer, rpc_out);
    if (rc != 0)
        return rc;

    char ch = 0;
    if (::read(STDIN_FILENO, &ch, 1) != 1)
        return 110;

    *rpc_out << "SLICER_RUNTIME_STDIO_OK\n";
    rpc_out->flush();
    return rpc_out->good() ? 0 : 111;
}

int run_probe_runtime()
{
    const char* component_dir = std::getenv("SLICER_LINUX_RUNTIME_COMPONENT_DIR");
    if (!component_dir || !*component_dir)
        return 120;

    const auto ca_path = std::filesystem::path(component_dir) / "ca-certificates.crt";
    std::ifstream ca(ca_path, std::ios::binary);
    if (!ca)
        return 121;
    const std::string pem{std::istreambuf_iterator<char>{ca}, std::istreambuf_iterator<char>{}};
    if (pem.size() < 65536 || pem.find("-----BEGIN CERTIFICATE-----") == std::string::npos)
        return 122;

    const CURLcode init = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (init != CURLE_OK)
        return 123;
    CURL* easy = curl_easy_init();
    if (!easy) {
        curl_global_cleanup();
        return 124;
    }
    const std::string ca_path_string = ca_path.string();
    const CURLcode ca_result = curl_easy_setopt(easy, CURLOPT_CAINFO, ca_path_string.c_str());
    const CURLcode peer_result = curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
    const CURLcode host_result = curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    const curl_version_info_data* version = curl_version_info(CURLVERSION_NOW);
    const bool features_ok = version &&
        (version->features & CURL_VERSION_SSL) != 0 &&
        (version->features & CURL_VERSION_LIBZ) != 0;
    if (version) {
        std::cerr << "curl=" << (version->version ? version->version : "unknown")
                  << " ssl=" << (version->ssl_version ? version->ssl_version : "unknown")
                  << " ca=" << ca_path_string << std::endl;
    }
    curl_easy_cleanup(easy);
    curl_global_cleanup();
    if (ca_result != CURLE_OK || peer_result != CURLE_OK || host_result != CURLE_OK)
        return 125;
    return features_ok ? 0 : 126;
}

int run_probe_auth()
{
    const char* component_dir_value = std::getenv("SLICER_LINUX_RUNTIME_COMPONENT_DIR");
    if (!component_dir_value || !*component_dir_value)
        return 130;

    const std::filesystem::path component_dir(component_dir_value);
    const std::filesystem::path cert_path = component_dir / "slicer_base64.cer";
    std::error_code cert_error;
    if (!std::filesystem::is_regular_file(cert_path, cert_error) || cert_error)
        return 131;

    LinuxRuntimeHost host;
    const auto handshake = host.handle("runtime.handshake", nlohmann::json::object());
    if (!handshake.value("component_loaded", false))
        return 2;

    const std::string log_dir = std::getenv("SLICER_LINUX_RUNTIME_PROBE_LOG_DIR")
        ? std::getenv("SLICER_LINUX_RUNTIME_PROBE_LOG_DIR")
        : std::string(".");
    const std::string country = std::getenv("SLICER_LINUX_RUNTIME_COUNTRY_CODE")
        ? std::getenv("SLICER_LINUX_RUNTIME_COUNTRY_CODE")
        : std::string();

    const auto created = host.handle("net.create_agent", {{"log_dir", log_dir}, {"country_code", country}});
    if (!created.value("ok", false))
        return 3;
    std::int64_t agent = created.value("value", 0LL);
    if (agent <= 0)
        return 4;

    auto integer_step = [&](const char* method, nlohmann::json payload, int& value) {
        payload["agent"] = agent;
        const auto response = host.handle(method, std::move(payload));
        if (!response.value("ok", false) || !response.contains("value") || !response["value"].is_number_integer())
            return false;
        value = response["value"].get<int>();
        return true;
    };

    auto finish = [&](int result) {
        if (agent > 0) {
            int destroy_result = -1;
            const bool destroyed = integer_step("net.destroy_agent", nlohmann::json::object(), destroy_result);
            agent = 0;
            if (result == 0 && (!destroyed || destroy_result != 0))
                result = 11;
        }
        host.begin_shutdown();
        return result;
    };

    int config_result = -1;
    int init_log_result = -1;
    int cert_result = -1;

    const bool config_ok = integer_step("net.set_config_dir", {{"config_dir", log_dir}}, config_result);
    const bool init_log_ok = integer_step("net.init_log", nlohmann::json::object(), init_log_result);
    const bool cert_ok = integer_step(
        "net.set_cert_file",
        {{"folder", component_dir.string()}, {"filename", cert_path.filename().string()}},
        cert_result);

    if (!config_ok || config_result != 0)
        return finish(5);
    if (!init_log_ok || init_log_result != 0)
        return finish(6);
    if (!cert_ok || cert_result != 0)
        return finish(7);

    int country_result = -1;
    if (!integer_step("net.set_country_code", {{"country_code", country}}, country_result) || country_result != 0)
        return finish(8);

    int start_result = -1;
    if (!integer_step("net.start", nlohmann::json::object(), start_result) || start_result != 0)
        return finish(9);

    const auto is_login = host.handle("net.is_user_login", {{"agent", agent}});
    if (!is_login.value("ok", false) || !is_login.contains("value") || !is_login["value"].is_boolean())
        return finish(10);

    return finish(0);
}

}

int main(int argc, char** argv)
{
    if (const char* compat = std::getenv("SLICER_LINUX_RUNTIME_ROSETTA_SPLITLOCK_COMPAT");
        compat && std::strcmp(compat, "1") == 0) {
        (void)::unsetenv("LD_PRELOAD");
    }

    std::ios::sync_with_stdio(false);

    if (argc > 1 && std::string(argv[1]) == "--probe-load")
        return run_probe_load();

    if (argc > 1 && std::string(argv[1]) == "--probe-stdio-roundtrip")
        return run_probe_stdio_roundtrip();

    if (argc > 1 && std::string(argv[1]) == "--probe-auth")
        return run_probe_auth();

    if (argc > 1 && std::string(argv[1]) == "--probe-runtime")
        return run_probe_runtime();

    std::unique_ptr<FdOutputBuffer> rpc_buffer;
    std::unique_ptr<std::ostream> rpc_out_ptr;
    const int rc = open_rpc_output(rpc_buffer, rpc_out_ptr);
    if (rc != 0)
        return rc;
    std::ostream& rpc_out = *rpc_out_ptr;

    LinuxRuntimeHost host;
    std::mutex out_mutex;
    std::mutex workers_mutex;
    std::condition_variable workers_cv;
    std::size_t active_workers = 0;

    while (true) {
        RawRpcFrame raw;
        std::string err;
        if (!read_raw_frame(std::cin, raw, err))
            break;

        RpcFrame req;
        if (!read_request_frame(raw, req, err)) {
            std::lock_guard<std::mutex> lock(out_mutex);
            write_json_frame(rpc_out, RpcFrameType::json_response, raw.id, {{"ok", false}, {"error", err}});
            continue;
        }

        std::vector<unsigned char> request_binary;
        bool has_binary_request = false;
        std::size_t expected_binary_size = 0;
        try {
            if (req.payload.is_object()) {
                has_binary_request = req.payload.value("__binary_request", false);
                expected_binary_size = req.payload.value("__binary_request_size", std::size_t(0));
            }
        } catch (const std::exception&) {
            std::lock_guard<std::mutex> lock(out_mutex);
            write_json_frame(rpc_out, RpcFrameType::json_response, req.id, {{"ok", false}, {"error", "invalid binary request metadata"}});
            continue;
        }
        if (has_binary_request) {
            RawRpcFrame binary_raw;
            if (!read_raw_frame(std::cin, binary_raw, err) ||
                binary_raw.type != RpcFrameType::binary_data ||
                binary_raw.id != req.id ||
                binary_raw.payload.size() != expected_binary_size) {
                std::lock_guard<std::mutex> lock(out_mutex);
                write_json_frame(rpc_out, RpcFrameType::json_response, req.id, {{"ok", false}, {"error", "missing or invalid binary request payload"}});
                continue;
            }
            request_binary = std::move(binary_raw.payload);
        }

        {
            std::lock_guard<std::mutex> lock(workers_mutex);
            ++active_workers;
        }
        std::thread([&host, &rpc_out, &out_mutex, &workers_mutex, &workers_cv, &active_workers,
                     req_id = req.id, req_method = req.method, req_payload = req.payload,
                     request_binary = std::move(request_binary)]() mutable {
            struct WorkerDone {
                std::mutex& mutex;
                std::condition_variable& cv;
                std::size_t& count;
                ~WorkerDone()
                {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        --count;
                    }
                    cv.notify_all();
                }
            } done{workers_mutex, workers_cv, active_workers};

            LinuxRuntimeHost::set_thread_request_binary(std::move(request_binary));

            nlohmann::json resp;
            try {
                resp = host.handle(req_method, req_payload);
            } catch (const std::exception& e) {
                std::cerr << "[SLRUNTIME] unhandled exception in method " << req_method << ": " << e.what() << std::endl;
                resp = {{"ok", false}, {"error", e.what()}, {"method", req_method}};
            } catch (...) {
                std::cerr << "[SLRUNTIME] unknown exception in method " << req_method << std::endl;
                resp = {{"ok", false}, {"error", "unknown exception"}, {"method", req_method}};
            }

            std::vector<unsigned char> reply_binary;
            const bool has_reply_binary = LinuxRuntimeHost::consume_thread_reply_binary(reply_binary);

            std::lock_guard<std::mutex> lock(out_mutex);
            write_json_frame(rpc_out, RpcFrameType::json_response, req_id, resp);
            if (has_reply_binary)
                write_raw_frame(rpc_out, RpcFrameType::binary_data, req_id, reply_binary.data(), reply_binary.size());
        }).detach();
    }

    host.begin_shutdown();
    {
        std::unique_lock<std::mutex> lock(workers_mutex);
        workers_cv.wait(lock, [&] { return active_workers == 0; });
    }
    return 0;
}
