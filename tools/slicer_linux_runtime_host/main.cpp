#include "LinuxRuntimeHost.hpp"
#include "../../src/slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeRpcProtocol.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

int run_probe_auth()
{
    LinuxRuntimeHost host;
    auto hs = host.handle("runtime.handshake", nlohmann::json::object());
    if (!hs.value("component_loaded", false))
        return 2;

    const std::string log_dir = std::getenv("SLICER_LINUX_RUNTIME_PROBE_LOG_DIR") ? std::getenv("SLICER_LINUX_RUNTIME_PROBE_LOG_DIR") : std::string(".");
    const std::string country = std::getenv("SLICER_LINUX_RUNTIME_COUNTRY_CODE") ? std::getenv("SLICER_LINUX_RUNTIME_COUNTRY_CODE") : std::string("PL");
    auto created = host.handle("net.create_agent", {{"log_dir", log_dir}, {"country_code", country}});
    if (!created.value("ok", false))
        return 3;
    const auto agent = created.value("value", 0LL);
    if (agent <= 0)
        return 4;

    auto step = [&](const char* method, nlohmann::json payload) -> bool {
        payload["agent"] = agent;
        auto r = host.handle(method, payload);
        return r.value("ok", false);
    };

    if (!step("net.set_config_dir", {{"config_dir", log_dir}}))
        return 5;
    if (!step("net.init_log", nlohmann::json::object()))
        return 6;
    step("net.set_country_code", {{"country_code", country}});
    step("net.start", nlohmann::json::object());

    auto is_login = host.handle("net.is_user_login", {{"agent", agent}});
    if (!is_login.value("ok", false))
        return 7;

    return 0;
}

}

int main(int argc, char** argv)
{
    std::ios::sync_with_stdio(false);

    if (argc > 1 && std::string(argv[1]) == "--probe-load")
        return run_probe_load();

    if (argc > 1 && std::string(argv[1]) == "--probe-stdio-roundtrip")
        return run_probe_stdio_roundtrip();

    if (argc > 1 && std::string(argv[1]) == "--probe-auth")
        return run_probe_auth();

    std::unique_ptr<FdOutputBuffer> rpc_buffer;
    std::unique_ptr<std::ostream> rpc_out_ptr;
    const int rc = open_rpc_output(rpc_buffer, rpc_out_ptr);
    if (rc != 0)
        return rc;
    std::ostream& rpc_out = *rpc_out_ptr;

    LinuxRuntimeHost host;
    std::mutex out_mutex;

    while (true) {
        RawRpcFrame raw;
        std::string err;
        if (!read_raw_frame(std::cin, raw, err))
            break;

        RpcFrame req;
        if (!read_request_frame(raw, req, err)) {
            std::lock_guard<std::mutex> lock(out_mutex);
            write_json_frame(rpc_out, RpcFrameType::json_response, 0, {{"ok", false}, {"error", err}});
            continue;
        }

        std::vector<unsigned char> request_binary;
        if (req.payload.is_object() && req.payload.value("__binary_request", false)) {
            RawRpcFrame binary_raw;
            if (!read_raw_frame(std::cin, binary_raw, err) ||
                binary_raw.type != RpcFrameType::binary_data ||
                binary_raw.id != req.id) {
                std::lock_guard<std::mutex> lock(out_mutex);
                write_json_frame(rpc_out, RpcFrameType::json_response, req.id, {{"ok", false}, {"error", "missing binary request payload"}});
                continue;
            }
            request_binary = std::move(binary_raw.payload);
        }

        std::thread([&host, &rpc_out, &out_mutex, req_id = req.id, req_method = req.method, req_payload = req.payload, request_binary = std::move(request_binary)]() mutable {
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

    return 0;
}
