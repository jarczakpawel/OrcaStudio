#include "SlicerLinuxRuntimeRpcClient.hpp"
#include "SlicerLinuxRuntimeLauncher.hpp"
#include "SlicerLinuxRuntimeRpcProtocol.hpp"

#include <boost/process/environment.hpp>
#if defined(_WIN32)
#include <boost/process/windows.hpp>
#endif

#include <chrono>
#include <stdexcept>

namespace bp = boost::process;

namespace Slic3r::SlicerLinuxRuntime {

namespace {

nlohmann::json make_error_payload(const std::string& error)
{
    return {{"ok", false}, {"error", error}};
}

}

RpcClient& RpcClient::instance()
{
    static RpcClient client;
    return client;
}

RpcClient::~RpcClient()
{
    stop();
}

void RpcClient::shutdown()
{
    stop();
}

bool RpcClient::start_locked()
{
    if (m_proc && m_proc->child.running())
        return true;

    try {
        auto spec = build_default_launch_spec();
        if (spec.argv.empty()) {
            m_last_error = "empty launch spec";
            return false;
        }

        bp::environment env = boost::this_process::environment();
        for (const auto& kv : spec.env)
            env[kv.first] = kv.second;

        std::vector<std::string> args;
        for (std::size_t i = 1; i < spec.argv.size(); ++i)
            args.push_back(spec.argv[i]);

        const auto preflight_error = launch_preflight_error();
        if (!preflight_error.empty()) {
            m_last_error = preflight_error;
            return false;
        }

        auto proc = std::make_shared<Proc>();
#if defined(_WIN32)
        proc->child = bp::child(spec.argv[0], bp::args(args), bp::std_in < proc->in, bp::std_out > proc->out,
                                bp::windows::create_no_window, bp::windows::hide, env);
#else
        proc->child = bp::child(spec.argv[0], bp::args(args), bp::std_in < proc->in, bp::std_out > proc->out, env);
#endif
        m_proc = std::move(proc);
        m_reader_stop.store(false, std::memory_order_release);
        m_handshake_ok = false;
        m_reader_failed = false;
        m_reader = std::thread([this] { reader_loop(); });
        m_last_error.clear();
        return true;
    } catch (const std::exception& e) {
        m_last_error = e.what();
        m_proc.reset();
        m_handshake_ok = false;
        return false;
    }
}

void RpcClient::stop()
{
    std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
    stop_locked();
}

void RpcClient::stop_locked()
{
    std::shared_ptr<Proc> proc;
    std::thread reader;
    std::map<int, std::shared_ptr<Pending>> pending;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_reader_stop.store(true, std::memory_order_release);
        m_handshake_ok = false;
        m_reader_failed = false;
        proc = std::move(m_proc);
        reader = std::move(m_reader);
        pending = std::move(m_pending);
        m_pending.clear();
    }

    if (proc) {
        {
            std::lock_guard<std::mutex> wlock(m_write_mutex);
            try { proc->in.pipe().close(); } catch (...) {}
        }
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (proc->child.running() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (proc->child.running())
                proc->child.terminate();
            proc->child.wait();
        } catch (...) {}
        try { proc->out.pipe().close(); } catch (...) {}
    }

    if (reader.joinable())
        reader.join();

    for (auto& it : pending) {
        std::lock_guard<std::mutex> plock(it.second->mutex);
        if (!it.second->ready) {
            it.second->payload = make_error_payload("runtime host stopped");
            it.second->ready = true;
            it.second->cv.notify_all();
        }
    }
}

bool RpcClient::ensure_started()
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            std::lock_guard<std::mutex> lifecycle_lock(m_lifecycle_mutex);
            bool recover = false;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                recover = m_reader_failed || (m_reader.joinable() && (!m_proc || !m_proc->child.running()));
            }
            if (recover)
                stop_locked();
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (!start_locked())
                    return false;
            }
        }
        if (ensure_handshake())
            return true;
    }
    return false;
}

bool RpcClient::is_started() const
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_proc && m_proc->child.running();
}

bool RpcClient::ensure_handshake()
{
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_proc || !m_proc->child.running() || m_reader_failed)
            return false;
        if (m_handshake_ok)
            return true;
    }

    const auto reply = request_impl("runtime.handshake", nlohmann::json::object(), {}, true);
    if (!reply.payload.value("ok", false)) {
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = reply.payload.value("error", std::string("runtime handshake failed"));
        }
        stop();
        return false;
    }

    if (reply.payload.value("protocol_version", 0) != 1) {
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_last_error = "runtime protocol version mismatch";
        }
        stop();
        return false;
    }


    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_handshake_ok = true;
        m_last_error.clear();
    }
    return true;
}

void RpcClient::reader_loop()
{
    while (!m_reader_stop.load(std::memory_order_acquire)) {
        std::shared_ptr<Proc> proc;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            if (!m_proc)
                break;
            proc = m_proc;
        }

        RawRpcFrame raw;
        std::string error;
        if (!read_raw_frame(proc->out, raw, error)) {
            std::map<int, std::shared_ptr<Pending>> pending;
            std::string local_error;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                if (m_reader_stop.load(std::memory_order_acquire))
                    break;
                m_last_error = error.empty() ? "runtime host closed stdout" : error;
                m_handshake_ok = false;
                m_reader_failed = true;
                try {
                    if (proc && proc->child.valid() && !proc->child.running())
                        m_last_error += ", child exit code=" + std::to_string(proc->child.exit_code());
                } catch (...) {}
                local_error = m_last_error;
                pending = m_pending;
                m_pending.clear();
            }
            for (auto& it : pending) {
                std::lock_guard<std::mutex> plock(it.second->mutex);
                it.second->payload = make_error_payload(local_error);
                it.second->ready = true;
                it.second->cv.notify_all();
            }
            break;
        }

        std::shared_ptr<Pending> pending;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            auto it = m_pending.find(raw.id);
            if (it != m_pending.end())
                pending = it->second;
        }
        if (!pending)
            continue;

        if (raw.type == RpcFrameType::json_response) {
            nlohmann::json payload;
            if (!read_json_frame(raw, payload, error) || !payload.is_object())
                payload = make_error_payload(error.empty() ? "invalid runtime JSON response" : error);

            bool ready = false;
            {
                std::lock_guard<std::mutex> plock(pending->mutex);
                pending->payload = std::move(payload);
                pending->expects_binary = pending->payload.value("__binary_pending", false);
                pending->expected_binary_size = pending->payload.value("binary_size", std::size_t(0));
                if (pending->expected_binary_size > 1024ULL * 1024ULL * 1024ULL) {
                    pending->payload = make_error_payload("runtime binary response exceeds limit");
                    pending->expects_binary = false;
                } else if (pending->binary_received && pending->expects_binary && pending->binary.size() != pending->expected_binary_size) {
                    pending->payload = make_error_payload("runtime binary response size mismatch");
                    pending->expects_binary = false;
                    pending->binary.clear();
                }
                pending->json_received = true;
                ready = !pending->expects_binary || pending->binary_received;
                pending->ready = ready;
            }

            if (ready) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_pending.erase(raw.id);
                pending->cv.notify_all();
            }
            continue;
        }

        if (raw.type == RpcFrameType::binary_data) {
            bool ready = false;
            {
                std::lock_guard<std::mutex> plock(pending->mutex);
                pending->binary = std::move(raw.payload);
                pending->binary_received = true;
                if (pending->json_received && pending->expects_binary && pending->binary.size() != pending->expected_binary_size) {
                    pending->payload = make_error_payload("runtime binary response size mismatch");
                    pending->expects_binary = false;
                    pending->binary.clear();
                }
                ready = pending->json_received;
                pending->ready = ready;
            }

            if (ready) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_pending.erase(raw.id);
                pending->cv.notify_all();
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> plock(pending->mutex);
            pending->payload = make_error_payload("unexpected runtime frame type");
            pending->ready = true;
        }
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_pending.erase(raw.id);
        }
        pending->cv.notify_all();
    }
}

RpcBinaryReply RpcClient::request_impl(const std::string& method, const nlohmann::json& payload, const std::vector<unsigned char>& request_binary, bool skip_handshake)
{
    if (!skip_handshake && !ensure_started())
        return {make_error_payload(last_error()), {}};

    std::shared_ptr<Pending> pending = std::make_shared<Pending>();
    std::shared_ptr<Proc> proc;
    int id = 0;

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_proc || !m_proc->child.running() || m_reader_failed)
            return {make_error_payload(m_last_error.empty() ? "Linux runtime host is not running" : m_last_error), {}};
        proc = m_proc;
        id = m_next_id++;
        m_pending[id] = pending;
    }

    try {
        RpcFrame frame;
        frame.id = id;
        frame.method = method;
        frame.payload = payload;
        if (!request_binary.empty()) {
            frame.payload["__binary_request"] = true;
            frame.payload["__binary_request_size"] = request_binary.size();
        }

        std::lock_guard<std::mutex> wlock(m_write_mutex);
        if (!proc || !proc->child.running())
            throw std::runtime_error("Linux runtime host is not running");
        if (!write_request_frame(proc->in, frame))
            throw std::runtime_error("failed to write request frame");
        if (!request_binary.empty() && !write_raw_frame(proc->in, RpcFrameType::binary_data, id, request_binary.data(), request_binary.size()))
            throw std::runtime_error("failed to write request binary frame");
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_pending.erase(id);
        m_last_error = e.what();
        m_handshake_ok = false;
        m_reader_failed = true;
        return {make_error_payload(m_last_error), {}};
    }

    std::unique_lock<std::mutex> plock(pending->mutex);
    const auto timeout = method == "runtime.handshake" ? std::chrono::seconds(30) : std::chrono::hours(2);
    if (!pending->cv.wait_for(plock, timeout, [&] { return pending->ready; })) {
        plock.unlock();
        const std::string error = "Linux runtime request timed out: " + method;
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_pending.erase(id);
            m_last_error = error;
            m_handshake_ok = false;
            m_reader_failed = true;
        }
        stop();
        return {make_error_payload(error), {}};
    }
    return {pending->payload, pending->binary};
}

int RpcClient::invoke_int(const std::string& method, const nlohmann::json& payload)
{
    const auto reply = request_impl(method, payload, {}, false);
    if (reply.payload.contains("ret"))
        return reply.payload.value("ret", -1);
    return reply.payload.value("value", -1);
}

bool RpcClient::invoke_bool(const std::string& method, const nlohmann::json& payload)
{
    const auto reply = request_impl(method, payload, {}, false);
    return reply.payload.value("value", false);
}

std::string RpcClient::invoke_string(const std::string& method, const nlohmann::json& payload)
{
    const auto reply = request_impl(method, payload, {}, false);
    return reply.payload.value("value", std::string());
}

nlohmann::json RpcClient::invoke_json(const std::string& method, const nlohmann::json& payload)
{
    return request_impl(method, payload, {}, false).payload;
}

RpcBinaryReply RpcClient::invoke_binary(const std::string& method, const nlohmann::json& payload, const std::vector<unsigned char>& request_binary)
{
    return request_impl(method, payload, request_binary, false);
}

void RpcClient::invoke_void(const std::string& method, const nlohmann::json& payload)
{
    (void) request_impl(method, payload, {}, false);
}

std::string RpcClient::last_error() const
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_last_error;
}

}
