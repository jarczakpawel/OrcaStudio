#include "SlicerLinuxRuntimeForwarderState.hpp"
#include <climits>
#include "SlicerLinuxRuntimeCompat.hpp"
#include "SlicerLinuxRuntimeConfig.hpp"
#include "SlicerLinuxRuntimeEventPump.hpp"
#include "SlicerLinuxRuntimeRpcClient.hpp"
#include "../../../../shared/slicer_linux_runtime_core/RuntimeCoreJson.hpp"
#include "libslic3r_version.h"

#include "../../GUI/Printer/BambuTunnel.h"
#include "../FileTransferUtils.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <filesystem>
#include <boost/log/trivial.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"
#endif

#if defined(_WIN32)
#define SLICER_LINUX_RUNTIME_EXPORT extern "C" __declspec(dllexport)
#else
#define SLICER_LINUX_RUNTIME_EXPORT extern "C"
#endif

namespace Slic3r {

struct FT_TunnelHandle {
    std::atomic<int> refs{1};
    std::int64_t remote{0};
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    std::size_t active_conn_callbacks{0};
    std::size_t active_status_callbacks{0};
    void (*conn_cb)(void*, int, int, const char*){nullptr};
    void* conn_user{nullptr};
    void (*status_cb)(void*, int, int, int, const char*){nullptr};
    void* status_user{nullptr};
};

struct FT_JobHandle {
    std::atomic<int> refs{1};
    std::int64_t remote{0};
    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    std::size_t active_result_callbacks{0};
    std::size_t active_msg_callbacks{0};
    bool started{false};
    bool result_poller_started{false};
    bool msg_poller_started{false};
    void (*result_cb)(void*, ft_job_result){nullptr};
    void* result_user{nullptr};
    void (*msg_cb)(void*, ft_job_msg){nullptr};
    void* msg_user{nullptr};
};

}

namespace Slic3r::SlicerLinuxRuntime {

static std::mutex g_last_error_mutex;
static std::string g_last_error;
static thread_local std::string g_last_error_snapshot;
static std::atomic<bool> g_forwarder_shutting_down{false};
static std::mutex g_worker_mutex;
static std::vector<std::thread> g_workers;

template <typename Fn>
static bool start_forwarder_worker(Fn&& fn)
{
    std::lock_guard<std::mutex> lock(g_worker_mutex);
    if (g_forwarder_shutting_down.load(std::memory_order_acquire))
        return false;
    g_workers.emplace_back(std::forward<Fn>(fn));
    return true;
}

static void join_forwarder_workers()
{
    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(g_worker_mutex);
        workers.swap(g_workers);
    }
    for (auto& worker : workers) {
        if (worker.joinable())
            worker.join();
    }
}

static void set_last_error(std::string value)
{
    std::lock_guard<std::mutex> lock(g_last_error_mutex);
    g_last_error = std::move(value);
}

static const char* last_error_c_str()
{
    std::lock_guard<std::mutex> lock(g_last_error_mutex);
    g_last_error_snapshot = g_last_error;
    return g_last_error_snapshot.c_str();
}

static std::string runtime_reported_version()
{
    const auto j = RpcClient::instance().invoke_json("runtime.handshake", nlohmann::json::object());
    if (!j.value("ok", false)) {
        if (j.contains("error"))
            set_last_error(j["error"].get<std::string>());
        return {};
    }
    const bool component_loaded = j.value("component_loaded", j.value("network_loaded", false));
    const bool source_loaded = j.value("source_loaded", false);
    const std::string component_status = j.value("component_status", j.value("network_status", std::string("unknown")));
    const std::string source_status = j.value("source_status", std::string("unknown"));
    if (!component_loaded || !source_loaded) {
        set_last_error("Linux runtime host failed to load Linux package: component=" + component_status + ", source=" + source_status);
        return {};
    }
    const auto actual = j.value("component_actual_abi_version", std::string());
    if (!actual.empty())
        return actual;
    const auto reported = j.value("component_abi_version", std::string());
    if (!reported.empty())
        return reported;
    return expected_component_abi_version();
}

static int invalid_handle()
{
    set_last_error("invalid handle");
    return BAMBU_NETWORK_ERR_INVALID_HANDLE;
}

static RuntimeAgent* require_agent(void* handle)
{
    return as_agent(handle);
}

static RuntimeTunnelLease require_tunnel(Bambu_Tunnel tunnel)
{
    return acquire_tunnel_handle(tunnel);
}

static nlohmann::json ok_or_error(const nlohmann::json& j)
{
    if (!j.value("ok", false) && j.contains("error"))
        set_last_error(j["error"].get<std::string>());
    return j;
}

static std::int64_t agent_id(RuntimeAgent* a)
{
    return a ? a->remote_handle : 0;
}

static char* copy_c_string(const std::string& value)
{
    auto* out = static_cast<char*>(std::malloc(value.size() + 1));
    if (!out)
        return nullptr;
    if (!value.empty())
        std::memcpy(out, value.data(), value.size());
    out[value.size()] = '\0';
    return out;
}

static const void* copy_binary_buffer(const std::vector<unsigned char>& value)
{
    if (value.empty())
        return nullptr;
    auto* out = std::malloc(value.size());
    if (!out)
        return nullptr;
    std::memcpy(out, value.data(), value.size());
    return out;
}

static Slic3r::ft_err ft_value(const nlohmann::json& j, int fallback = -128)
{
    return static_cast<Slic3r::ft_err>(j.value("value", fallback));
}

static void notify_tunnel_status(Slic3r::FT_TunnelHandle* tunnel, int old_status, int new_status, int err, const char* msg)
{
    if (!tunnel || g_forwarder_shutting_down.load(std::memory_order_acquire))
        return;
    void (*callback)(void*, int, int, int, const char*) = nullptr;
    void* user = nullptr;
    {
        std::lock_guard<std::mutex> lock(tunnel->callback_mutex);
        callback = tunnel->status_cb;
        user = tunnel->status_user;
        if (callback)
            ++tunnel->active_status_callbacks;
    }
    if (!callback)
        return;
    callback(user, old_status, new_status, err, msg ? msg : "");
    {
        std::lock_guard<std::mutex> lock(tunnel->callback_mutex);
        if (tunnel->active_status_callbacks > 0)
            --tunnel->active_status_callbacks;
    }
    tunnel->callback_cv.notify_all();
}

static void retain_job_handle(Slic3r::FT_JobHandle* job)
{
    if (job)
        job->refs.fetch_add(1, std::memory_order_relaxed);
}

static void retain_tunnel_handle(Slic3r::FT_TunnelHandle* tunnel)
{
    if (tunnel)
        tunnel->refs.fetch_add(1, std::memory_order_relaxed);
}

static void release_job_handle(Slic3r::FT_JobHandle* job)
{
    if (!job)
        return;
    if (job->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        {
            std::unique_lock<std::mutex> lock(job->callback_mutex);
            job->result_cb = nullptr;
            job->result_user = nullptr;
            job->msg_cb = nullptr;
            job->msg_user = nullptr;
            job->callback_cv.wait(lock, [job] {
                return job->active_result_callbacks == 0 && job->active_msg_callbacks == 0;
            });
        }
        if (!g_forwarder_shutting_down.load(std::memory_order_acquire))
            RpcClient::instance().invoke_void("ft.job_release", {{"job", job->remote}});
        delete job;
    }
}

static void release_tunnel_handle(Slic3r::FT_TunnelHandle* tunnel)
{
    if (!tunnel)
        return;
    if (tunnel->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        {
            std::unique_lock<std::mutex> lock(tunnel->callback_mutex);
            tunnel->conn_cb = nullptr;
            tunnel->conn_user = nullptr;
            tunnel->status_cb = nullptr;
            tunnel->status_user = nullptr;
            tunnel->callback_cv.wait(lock, [tunnel] {
                return tunnel->active_conn_callbacks == 0 && tunnel->active_status_callbacks == 0;
            });
        }
        if (!g_forwarder_shutting_down.load(std::memory_order_acquire))
            RpcClient::instance().invoke_void("ft.tunnel_release", {{"tunnel", tunnel->remote}});
        delete tunnel;
    }
}

static bool poll_ft_messages(Slic3r::FT_JobHandle* job)
{
    retain_job_handle(job);
    const bool started = start_forwarder_worker([job] {
        while (!g_forwarder_shutting_down.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(job->callback_mutex);
                if (!job->msg_cb)
                    break;
            }
            const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.job_get_msg", {{"job", job->remote}, {"timeout_ms", 250U}}));
            if (g_forwarder_shutting_down.load(std::memory_order_acquire))
                break;
            const auto rc = j.value("value", -128);
            if (rc == -2 || rc == -5)
                break;
            if (rc == 0) {
                void (*callback)(void*, Slic3r::ft_job_msg) = nullptr;
                void* user = nullptr;
                {
                    std::lock_guard<std::mutex> lock(job->callback_mutex);
                    callback = job->msg_cb;
                    user = job->msg_user;
                    if (callback)
                        ++job->active_msg_callbacks;
                }
                if (callback) {
                    Slic3r::ft_job_msg msg{};
                    msg.kind = j.value("kind", 0);
                    msg.json = copy_c_string(j.value("json", std::string()));
                    callback(user, msg);
                    {
                        std::lock_guard<std::mutex> lock(job->callback_mutex);
                        if (job->active_msg_callbacks > 0)
                            --job->active_msg_callbacks;
                    }
                    job->callback_cv.notify_all();
                }
            }
            if (rc != 0 && rc != -4)
                break;
        }
        {
            std::lock_guard<std::mutex> lock(job->callback_mutex);
            job->msg_poller_started = false;
        }
        job->callback_cv.notify_all();
        release_job_handle(job);
    });
    if (!started)
        release_job_handle(job);
    return started;
}

static bool poll_ft_result(Slic3r::FT_JobHandle* job)
{
    retain_job_handle(job);
    const bool started = start_forwarder_worker([job] {
        while (!g_forwarder_shutting_down.load(std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(job->callback_mutex);
                if (!job->result_cb)
                    break;
            }
            const auto reply = RpcClient::instance().invoke_binary("ft.job_get_result", {{"job", job->remote}, {"timeout_ms", 250U}});
            if (g_forwarder_shutting_down.load(std::memory_order_acquire))
                break;
            const auto j = ok_or_error(reply.payload);
            const auto rc = j.value("value", -128);
            if (rc == -4)
                continue;
            if (rc == 0) {
                void (*callback)(void*, Slic3r::ft_job_result) = nullptr;
                void* user = nullptr;
                {
                    std::lock_guard<std::mutex> lock(job->callback_mutex);
                    callback = job->result_cb;
                    user = job->result_user;
                    if (callback)
                        ++job->active_result_callbacks;
                }
                if (callback) {
                    Slic3r::ft_job_result result{};
                    result.ec = j.value("ec", 0);
                    result.resp_ec = j.value("resp_ec", 0);
                    result.json = copy_c_string(j.value("json", std::string()));
                    result.bin = copy_binary_buffer(reply.binary);
                    result.bin_size = static_cast<uint32_t>(reply.binary.size());
                    callback(user, result);
                    {
                        std::lock_guard<std::mutex> lock(job->callback_mutex);
                        if (job->active_result_callbacks > 0)
                            --job->active_result_callbacks;
                    }
                    job->callback_cv.notify_all();
                }
            }
            break;
        }
        {
            std::lock_guard<std::mutex> lock(job->callback_mutex);
            job->result_poller_started = false;
        }
        job->callback_cv.notify_all();
        release_job_handle(job);
    });
    if (!started)
        release_job_handle(job);
    return started;
}

static bool ensure_ft_message_poller(Slic3r::FT_JobHandle* job)
{
    bool launch = false;
    {
        std::lock_guard<std::mutex> lock(job->callback_mutex);
        if (job->started && job->msg_cb && !job->msg_poller_started) {
            job->msg_poller_started = true;
            launch = true;
        }
    }
    if (!launch)
        return true;
    if (poll_ft_messages(job))
        return true;
    {
        std::lock_guard<std::mutex> lock(job->callback_mutex);
        job->msg_poller_started = false;
    }
    return false;
}

static bool ensure_ft_result_poller(Slic3r::FT_JobHandle* job)
{
    bool launch = false;
    {
        std::lock_guard<std::mutex> lock(job->callback_mutex);
        if (job->started && job->result_cb && !job->result_poller_started) {
            job->result_poller_started = true;
            launch = true;
        }
    }
    if (!launch)
        return true;
    if (poll_ft_result(job))
        return true;
    {
        std::lock_guard<std::mutex> lock(job->callback_mutex);
        job->result_poller_started = false;
    }
    return false;
}

static int register_remote_callback(const char* method, RuntimeAgent* a);

static void fill_user_cache(RuntimeAgent* a, const nlohmann::json& j)
{
    if (!a)
        return;
    std::lock_guard<std::mutex> lock(a->state_mutex);
    if (j.contains("user_id")) a->user_id = j["user_id"].get<std::string>();
    if (j.contains("user_name")) a->user_name = j["user_name"].get<std::string>();
    if (j.contains("user_avatar")) a->user_avatar = j["user_avatar"].get<std::string>();
    if (j.contains("user_nickname")) a->user_nickname = j["user_nickname"].get<std::string>();
    if (j.contains("login_cmd")) a->login_cmd = j["login_cmd"].get<std::string>();
    if (j.contains("logout_cmd")) a->logout_cmd = j["logout_cmd"].get<std::string>();
    if (j.contains("login_info")) a->login_info = j["login_info"].get<std::string>();
    if (j.contains("bambulab_host")) a->bambulab_host = j["bambulab_host"].get<std::string>();
}

template <typename Fn>
static int set_agent_callback(const char* method, RuntimeAgent* agent, Fn RuntimeAgent::* member, Fn callback)
{
    if (!agent)
        return invalid_handle();
    {
        std::lock_guard<std::mutex> lock(agent->state_mutex);
        agent->*member = std::move(callback);
    }
    return register_remote_callback(method, agent);
}

template <typename T>
static T set_agent_cache(RuntimeAgent* agent, T RuntimeAgent::* member, T value)
{
    if (!agent)
        return value;
    std::lock_guard<std::mutex> lock(agent->state_mutex);
    agent->*member = std::move(value);
    return agent->*member;
}

template <typename T>
static T get_agent_cache(RuntimeAgent* agent, T RuntimeAgent::* member, T fallback = T{})
{
    if (!agent)
        return fallback;
    std::lock_guard<std::mutex> lock(agent->state_mutex);
    return agent->*member;
}

static void ensure_event_pump()
{
    EventPump::instance().ensure_started();
}

static int register_remote_callback(const char* method, RuntimeAgent* a)
{
    if (!a)
        return invalid_handle();
    ensure_event_pump();
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}}));
    return j.value("value", j.value("ret", 0));
}

static std::atomic<std::int64_t> g_next_job_id{1};

static std::int64_t next_job_id()
{
    return g_next_job_id.fetch_add(1);
}


#if defined(_WIN32)
static std::wstring utf8_to_wide(const std::string& value)
{
    if (value.empty())
        return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len);
    return out;
}

static std::string wide_to_utf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out(static_cast<std::size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), out.data(), len, nullptr, nullptr);
    return out;
}

static bool is_windows_drive_path(const std::string& path)
{
    if (path.size() < 3)
        return false;
    const unsigned char c = static_cast<unsigned char>(path[0]);
    const bool drive = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return drive && path[1] == ':' && (path[2] == '\\' || path[2] == '/');
}

static std::string normalize_windows_runtime_path(std::string path)
{
    if (path.empty())
        return path;

    if (path.rfind("\\\\?\\", 0) == 0)
        path.erase(0, 4);

    if (!is_windows_drive_path(path))
        return path;

    std::wstring wide = utf8_to_wide(path);
    if (wide.empty())
        return path;

    DWORD full_needed = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (full_needed) {
        std::wstring full(static_cast<std::size_t>(full_needed), L'\0');
        DWORD written = GetFullPathNameW(wide.c_str(), full_needed, full.data(), nullptr);
        if (written && written < full_needed) {
            full.resize(written);
            wide = std::move(full);
        }
    }

    const DWORD long_needed = GetLongPathNameW(wide.c_str(), nullptr, 0);
    if (long_needed) {
        std::wstring long_path(static_cast<std::size_t>(long_needed), L'\0');
        const DWORD written = GetLongPathNameW(wide.c_str(), long_path.data(), long_needed);
        if (written && written < long_needed) {
            long_path.resize(written);
            std::string normalized = wide_to_utf8(long_path);
            if (!normalized.empty())
                return normalized;
        }
    }

    std::string normalized = wide_to_utf8(wide);
    return normalized.empty() ? path : normalized;
}
#else
static std::string normalize_windows_runtime_path(std::string path)
{
    return path;
}
#endif

static std::string runtime_component_cert_folder(const std::string& filename)
{
    if (filename.empty())
        return {};
    const char* dir = std::getenv("SLICER_LINUX_RUNTIME_COMPONENT_DIR");
    if (!dir || !*dir)
        return {};
    std::filesystem::path cert_path = std::filesystem::path(dir) / filename;
    if (!std::filesystem::exists(cert_path) || std::filesystem::is_directory(cert_path))
        return {};
    return cert_path.parent_path().string();
}

static BBL::PrintParams normalize_runtime_paths(BBL::PrintParams params)
{
    params.filename = normalize_windows_runtime_path(std::move(params.filename));
    params.config_filename = normalize_windows_runtime_path(std::move(params.config_filename));
    params.dst_file = normalize_windows_runtime_path(std::move(params.dst_file));
    return params;
}

static BBL::PublishParams normalize_runtime_paths(BBL::PublishParams params)
{
    params.project_3mf_file = normalize_windows_runtime_path(std::move(params.project_3mf_file));
    params.config_filename = normalize_windows_runtime_path(std::move(params.config_filename));
    return params;
}

static void start_cancel_watch(const std::shared_ptr<RuntimeJobState>& job)
{
    if (!job || !job->was_cancelled)
        return;
    job->cancel_watch = std::thread([job]() {
        using namespace std::chrono_literals;
        while (!job->stop_cancel_watch.load()) {
            bool cancel_now = false;
            try {
                cancel_now = job->was_cancelled();
            } catch (...) {
                cancel_now = false;
            }
            if (cancel_now) {
                RpcClient::instance().invoke_void("runtime.job_cancel", {{"job_id", job->job_id}, {"cancel", true}});
                break;
            }
            std::this_thread::sleep_for(120ms);
        }
    });
}

static int invoke_job_update_only(const char* method, const char* kind, RuntimeAgent* a, const nlohmann::json& payload, BBL::OnUpdateStatusFn update)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<RuntimeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_update_status = std::move(update);
    register_job_state(a, job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", payload}}));
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

static int invoke_job_with_wait(const char* method, const char* kind, RuntimeAgent* a, const nlohmann::json& params, BBL::OnUpdateStatusFn update, BBL::WasCancelledFn cancel, BBL::OnWaitFn wait, std::string* out)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<RuntimeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_update_status = std::move(update);
    job->was_cancelled = std::move(cancel);
    job->on_wait = std::move(wait);
    job->out_string = out;
    register_job_state(a, job);
    start_cancel_watch(job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", params}}));
    if (out && j.contains("out"))
        *out = j.value("out", std::string());
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

static std::map<std::string, std::string> clone_string_map(const std::map<std::string, std::string>* in)
{
    return in ? *in : std::map<std::string, std::string>();
}

static int invoke_string_callback(const char* method, RuntimeAgent* a, const nlohmann::json& payload, const std::function<void(std::string)>& fn)
{
    if (!a)
        return invalid_handle();
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, payload));
    const int ret = j.value("value", j.value("ret", 0));
    if (ret == 0 && fn && j.contains("result"))
        fn(j.value("result", std::string()));
    return ret;
}

static int invoke_string_int_callback(const char* method, RuntimeAgent* a, const nlohmann::json& payload, const std::function<void(std::string, int)>& fn)
{
    if (!a)
        return invalid_handle();
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, payload));
    const int ret = j.value("value", j.value("ret", 0));
    if (ret == 0 && fn)
        fn(j.value("result", std::string()), j.value("status", 0));
    return ret;
}

static void json_to_nested_string_map(const nlohmann::json& j, std::map<std::string, std::map<std::string, std::string>>& out)
{
    out.clear();
    if (!j.is_object())
        return;
    for (auto it = j.begin(); it != j.end(); ++it) {
        std::map<std::string, std::string> inner;
        if (it.value().is_object()) {
            for (auto it2 = it.value().begin(); it2 != it.value().end(); ++it2)
                inner[it2.key()] = it2.value().is_string() ? it2.value().get<std::string>() : it2.value().dump();
        }
        out[it.key()] = std::move(inner);
    }
}

static int invoke_progress_job(const char* method, const char* kind, RuntimeAgent* a, const nlohmann::json& payload, BBL::ProgressFn progress, BBL::WasCancelledFn cancel)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<RuntimeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_progress = std::move(progress);
    job->was_cancelled = std::move(cancel);
    register_job_state(a, job);
    start_cancel_watch(job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", payload}}));
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

static int invoke_progress_check_job(const char* method, const char* kind, RuntimeAgent* a, const nlohmann::json& payload, BBL::CheckFn check, BBL::ProgressFn progress, BBL::WasCancelledFn cancel)
{
    if (!a)
        return invalid_handle();
    auto job = std::make_shared<RuntimeJobState>();
    job->job_id = next_job_id();
    job->kind = kind;
    job->on_check = std::move(check);
    job->on_progress = std::move(progress);
    job->was_cancelled = std::move(cancel);
    register_job_state(a, job);
    start_cancel_watch(job);
    const auto j = ok_or_error(RpcClient::instance().invoke_json(method, {{"agent", agent_id(a)}, {"client_job_id", job->job_id}, {"params", payload}}));
    unregister_job_state(a, job->job_id);
    return j.value("value", j.value("ret", 0));
}

}

using namespace Slic3r::SlicerLinuxRuntime;
using namespace BBL;
using Slic3r::BBLModelTask;
using Slic3r::OnGetSubTaskFn;

SLICER_LINUX_RUNTIME_EXPORT bool bambu_network_check_debug_consistent(bool) { return true; }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_version() { return runtime_reported_version(); }

SLICER_LINUX_RUNTIME_EXPORT void* bambu_network_create_agent(std::string log_dir)
{
    log_dir = normalize_windows_runtime_path(std::move(log_dir));
    auto* a = as_agent(new_agent(log_dir));
    auto& rpc = RpcClient::instance();
    const auto j = ok_or_error(rpc.invoke_json("net.create_agent", {{"log_dir", log_dir}}));
    if (!j.value("ok", false)) {
        delete_agent(a);
        return nullptr;
    }
    a->remote_handle = j.value("value", 0LL);
    if (a->remote_handle == 0) {
        set_last_error("Linux runtime host returned invalid remote agent handle");
        delete_agent(a);
        return nullptr;
    }
    register_remote_agent(a);
    ensure_event_pump();
    return a;
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_destroy_agent(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    unregister_remote_agent(a);
    if (a->remote_handle)
        ok_or_error(RpcClient::instance().invoke_json("net.destroy_agent", {{"agent", a->remote_handle}}));
    return delete_agent(agent);
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_init_log(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    return RpcClient::instance().invoke_int("net.init_log", {{"agent", agent_id(a)}});
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_config_dir(void* agent, std::string config_dir)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    const auto cached = set_agent_cache(a, &RuntimeAgent::config_dir, normalize_windows_runtime_path(std::move(config_dir)));
    return RpcClient::instance().invoke_int("net.set_config_dir", {{"agent", agent_id(a)}, {"config_dir", cached}});
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_cert_file(void* agent, std::string folder, std::string filename)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    const std::string runtime_cert_dir = runtime_component_cert_folder(filename);
    const auto cert_dir = set_agent_cache(a, &RuntimeAgent::cert_dir,
        runtime_cert_dir.empty() ? normalize_windows_runtime_path(std::move(folder)) : runtime_cert_dir);
    const auto cert_file = set_agent_cache(a, &RuntimeAgent::cert_file, std::move(filename));
    return RpcClient::instance().invoke_int("net.set_cert_file", {{"agent", agent_id(a)}, {"folder", cert_dir}, {"filename", cert_file}});
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_country_code(void* agent, std::string country_code)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    const auto cached = set_agent_cache(a, &RuntimeAgent::country_code, std::move(country_code));
    return RpcClient::instance().invoke_int("net.set_country_code", {{"agent", agent_id(a)}, {"country_code", cached}});
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    set_agent_cache(a, &RuntimeAgent::started, true);
    return RpcClient::instance().invoke_int("net.start", {{"agent", agent_id(a)}});
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_ssdp_msg_fn(void* agent, OnMsgArrivedFn fn) { return set_agent_callback("net.set_on_ssdp_msg_fn", require_agent(agent), &RuntimeAgent::on_ssdp_msg, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_user_login_fn(void* agent, OnUserLoginFn fn) { return set_agent_callback("net.set_on_user_login_fn", require_agent(agent), &RuntimeAgent::on_user_login, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_printer_connected_fn(void* agent, OnPrinterConnectedFn fn) { return set_agent_callback("net.set_on_printer_connected_fn", require_agent(agent), &RuntimeAgent::on_printer_connected, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_server_connected_fn(void* agent, OnServerConnectedFn fn) { return set_agent_callback("net.set_on_server_connected_fn", require_agent(agent), &RuntimeAgent::on_server_connected, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_http_error_fn(void* agent, OnHttpErrorFn fn) { return set_agent_callback("net.set_on_http_error_fn", require_agent(agent), &RuntimeAgent::on_http_error, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_get_country_code_fn(void* agent, GetCountryCodeFn fn) { return set_agent_callback("net.set_get_country_code_fn", require_agent(agent), &RuntimeAgent::get_country_code, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_subscribe_failure_fn(void* agent, GetSubscribeFailureFn fn) { return set_agent_callback("net.set_on_subscribe_failure_fn", require_agent(agent), &RuntimeAgent::on_subscribe_failure, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_message_fn(void* agent, OnMessageFn fn) { return set_agent_callback("net.set_on_message_fn", require_agent(agent), &RuntimeAgent::on_message, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_user_message_fn(void* agent, OnMessageFn fn) { return set_agent_callback("net.set_on_user_message_fn", require_agent(agent), &RuntimeAgent::on_user_message, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_local_connect_fn(void* agent, OnLocalConnectedFn fn) { return set_agent_callback("net.set_on_local_connect_fn", require_agent(agent), &RuntimeAgent::on_local_connect, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_on_local_message_fn(void* agent, OnMessageFn fn) { return set_agent_callback("net.set_on_local_message_fn", require_agent(agent), &RuntimeAgent::on_local_message, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_queue_on_main_fn(void* agent, QueueOnMainFn fn) { return set_agent_callback("net.set_queue_on_main_fn", require_agent(agent), &RuntimeAgent::queue_on_main, std::move(fn)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_server_callback(void* agent, OnServerErrFn fn) { return set_agent_callback("net.set_server_callback", require_agent(agent), &RuntimeAgent::on_server_error, std::move(fn)); }

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_connect_server(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    const int ret = RpcClient::instance().invoke_int("net.connect_server", {{"agent", agent_id(a)}});
    set_agent_cache(a, &RuntimeAgent::server_connected, ret == 0);
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT bool bambu_network_is_server_connected(void* agent)
{
    auto* a = require_agent(agent);
    if (!a) return false;
    const bool ret = RpcClient::instance().invoke_bool("net.is_server_connected", {{"agent", agent_id(a)}});
    set_agent_cache(a, &RuntimeAgent::server_connected, ret);
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_refresh_connection(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.refresh_connection", {{"agent", agent_id(a)}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start_subscribe(void* agent, std::string module) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.start_subscribe", {{"agent", agent_id(a)}, {"module", module}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_stop_subscribe(void* agent, std::string module) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.stop_subscribe", {{"agent", agent_id(a)}, {"module", module}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_add_subscribe(void* agent, std::vector<std::string> devs) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.add_subscribe", {{"agent", agent_id(a)}, {"devs", devs}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_del_subscribe(void* agent, std::vector<std::string> devs) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.del_subscribe", {{"agent", agent_id(a)}, {"devs", devs}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT void bambu_network_enable_multi_machine(void* agent, bool enable) { auto* a = require_agent(agent); if (a) { set_agent_cache(a, &RuntimeAgent::multi_machine_enabled, enable); RpcClient::instance().invoke_void("net.enable_multi_machine", {{"agent", agent_id(a)}, {"enable", enable}}); } }

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_send_message(void* agent, std::string dev_id, std::string msg, int qos, int flag) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.send_message", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"msg", msg}, {"qos", qos}, {"flag", flag}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_connect_printer(void* agent, std::string dev_id, std::string dev_ip, std::string username, std::string password, bool use_ssl)
{
    auto* a = require_agent(agent);
    if (!a)
        return invalid_handle();

    return RpcClient::instance().invoke_int(
        "net.connect_printer",
        {{"agent", agent_id(a)},
         {"dev_id", dev_id},
         {"dev_ip", dev_ip},
         {"username", username},
         {"password", password},
         {"use_ssl", use_ssl}});
}
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_disconnect_printer(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.disconnect_printer", {{"agent", agent_id(a)}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_send_message_to_printer(void* agent, std::string dev_id, std::string msg, int qos, int flag) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.send_message_to_printer", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"msg", msg}, {"qos", qos}, {"flag", flag}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_update_cert(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.update_cert", {{"agent", agent_id(a)}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT void bambu_network_install_device_cert(void* agent, std::string dev_id, bool lan_only) { auto* a = require_agent(agent); if (a) RpcClient::instance().invoke_void("net.install_device_cert", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"lan_only", lan_only}}); }
SLICER_LINUX_RUNTIME_EXPORT bool bambu_network_start_discovery(void* agent, bool start, bool sending) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_bool("net.start_discovery", {{"agent", agent_id(a)}, {"start", start}, {"sending", sending}}) : false; }

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_change_user(void* agent, std::string user_info)
{
    auto* a = require_agent(agent);
    if (!a) return invalid_handle();
    const auto cached_user_info = set_agent_cache(a, &RuntimeAgent::user_info, std::move(user_info));
    const auto j = ok_or_error(RpcClient::instance().invoke_json("net.change_user", {{"agent", agent_id(a)}, {"user_info", cached_user_info}}));
    set_agent_cache(a, &RuntimeAgent::logged_in, j.value("logged_in", !cached_user_info.empty()));
    fill_user_cache(a, j);
    return j.value("value", 0);
}

SLICER_LINUX_RUNTIME_EXPORT bool bambu_network_is_user_login(void* agent) { auto* a = require_agent(agent); if (!a) return false; return set_agent_cache(a, &RuntimeAgent::logged_in, RpcClient::instance().invoke_bool("net.is_user_login", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_user_logout(void* agent, bool request) { auto* a = require_agent(agent); if (!a) return invalid_handle(); set_agent_cache(a, &RuntimeAgent::logged_in, false); return RpcClient::instance().invoke_int("net.user_logout", {{"agent", agent_id(a)}, {"request", request}}); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_user_id(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::user_id, RpcClient::instance().invoke_string("net.get_user_id", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_user_name(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::user_name, RpcClient::instance().invoke_string("net.get_user_name", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_user_avatar(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::user_avatar, RpcClient::instance().invoke_string("net.get_user_avatar", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_user_nickanme(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::user_nickname, RpcClient::instance().invoke_string("net.get_user_nickname", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_build_login_cmd(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::login_cmd, RpcClient::instance().invoke_string("net.build_login_cmd", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_build_logout_cmd(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::logout_cmd, RpcClient::instance().invoke_string("net.build_logout_cmd", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_build_login_info(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::login_info, RpcClient::instance().invoke_string("net.build_login_info", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_ping_bind(void* agent, std::string ping_code) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.ping_bind", {{"agent", agent_id(a)}, {"ping_code", ping_code}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_bind_detect(void* agent, std::string dev_ip, std::string sec_link, detectResult& out)
{
    auto* a = require_agent(agent);
    if (!a)
        return invalid_handle();

    out = detectResult{};
    const auto fail = [&](std::string message) {
        out = detectResult{};
        out.result_msg = message;
        set_last_error(std::move(message));
        return -1;
    };

    const auto j = RpcClient::instance().invoke_json(
        "net.bind_detect", {{"agent", agent_id(a)}, {"dev_ip", dev_ip}, {"sec_link", sec_link}});
    if (!j.is_object() || !j.contains("ok") || !j["ok"].is_boolean())
        return fail("net.bind_detect returned a malformed RPC reply");
    if (!j["ok"].get<bool>()) {
        if (j.contains("error") && j["error"].is_string())
            return fail(j["error"].get<std::string>());
        return fail("net.bind_detect RPC failed");
    }
    if (!j.contains("value") || !j["value"].is_number_integer())
        return fail("net.bind_detect returned no result code");

    int result = 0;
    try {
        if (j["value"].is_number_unsigned()) {
            const auto unsigned_result = j["value"].get<std::uint64_t>();
            if (unsigned_result > static_cast<std::uint64_t>(INT_MAX))
                return fail("net.bind_detect returned an out-of-range result code");
            result = static_cast<int>(unsigned_result);
        } else {
            const auto signed_result = j["value"].get<std::int64_t>();
            if (signed_result < INT_MIN || signed_result > INT_MAX)
                return fail("net.bind_detect returned an out-of-range result code");
            result = static_cast<int>(signed_result);
        }
    } catch (const nlohmann::json::exception&) {
        return fail("net.bind_detect returned an invalid result code");
    }
    if (!j.contains("detect") || !j["detect"].is_object())
        return fail("net.bind_detect returned no detectResult payload");

    const auto& d = j["detect"];
    const auto read_string = [&](const char* name, std::string& value) {
        if (!d.contains(name)) {
            value.clear();
            return true;
        }
        if (!d[name].is_string())
            return false;
        value = d[name].get<std::string>();
        return true;
    };
    if (!read_string("result_msg", out.result_msg) ||
        !read_string("command", out.command) ||
        !read_string("dev_id", out.dev_id) ||
        !read_string("model_id", out.model_id) ||
        !read_string("dev_name", out.dev_name) ||
        !read_string("version", out.version) ||
        !read_string("bind_state", out.bind_state) ||
        !read_string("connect_type", out.connect_type))
        return fail("net.bind_detect returned an invalid detectResult field");

    return result;
}
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_report_consent(void* agent, std::string expand) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.report_consent", {{"agent", agent_id(a)}, {"expand", expand}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_bind(void* agent, std::string dev_ip, std::string dev_id, std::string dev_model, std::string sec_link, std::string timezone, bool improved, OnUpdateStatusFn update) { auto* a = require_agent(agent); return invoke_job_update_only("net.bind", "bind", a, {{"dev_ip", dev_ip}, {"dev_id", dev_id}, {"dev_model", dev_model}, {"sec_link", sec_link}, {"timezone", timezone}, {"improved", improved}}, std::move(update)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_unbind(void* agent, std::string dev_id) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.unbind", {{"agent", agent_id(a)}, {"dev_id", dev_id}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_bambulab_host(void* agent)
{
    auto* a = require_agent(agent);
    if (!a)
        return {};
    auto value = RpcClient::instance().invoke_string("net.get_bambulab_host", {{"agent", agent_id(a)}});
    if (value.empty())
        value = get_agent_cache(a, &RuntimeAgent::bambulab_host, std::string("https://bambulab.com"));
    if (value.empty())
        value = "https://bambulab.com";
    return set_agent_cache(a, &RuntimeAgent::bambulab_host, std::move(value));
}
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_user_selected_machine(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::selected_machine, RpcClient::instance().invoke_string("net.get_user_selected_machine", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_user_selected_machine(void* agent, std::string dev_id) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto cached = set_agent_cache(a, &RuntimeAgent::selected_machine, std::move(dev_id)); return RpcClient::instance().invoke_int("net.set_user_selected_machine", {{"agent", agent_id(a)}, {"dev_id", cached}}); }

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start_print(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_print", "print", a, JsonRuntime::to_json(normalize_runtime_paths(std::move(params))), std::move(update), std::move(cancel), std::move(wait), nullptr); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start_local_print_with_record(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_local_print_with_record", "local_print_with_record", a, JsonRuntime::to_json(normalize_runtime_paths(std::move(params))), std::move(update), std::move(cancel), std::move(wait), nullptr); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start_send_gcode_to_sdcard(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel, OnWaitFn wait) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_send_gcode_to_sdcard", "send_gcode_to_sdcard", a, JsonRuntime::to_json(normalize_runtime_paths(std::move(params))), std::move(update), std::move(cancel), std::move(wait), nullptr); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start_local_print(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_local_print", "local_print", a, JsonRuntime::to_json(normalize_runtime_paths(std::move(params))), std::move(update), std::move(cancel), OnWaitFn(), nullptr); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start_sdcard_print(void* agent, PrintParams params, OnUpdateStatusFn update, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_sdcard_print", "sdcard_print", a, JsonRuntime::to_json(normalize_runtime_paths(std::move(params))), std::move(update), std::move(cancel), OnWaitFn(), nullptr); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_user_presets(void* agent, std::map<std::string, std::map<std::string, std::string>>* user_presets) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_presets", {{"agent", agent_id(a)}})); if (user_presets && j.contains("user_presets")) json_to_nested_string_map(j["user_presets"], *user_presets); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_request_setting_id(void* agent, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) { auto* a = require_agent(agent); if (!a) return {}; const auto values = clone_string_map(values_map); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.request_setting_id", {{"agent", agent_id(a)}, {"name", name}, {"values", values}})); if (http_code) *http_code = j.value("http_code", 0u); return j.value("setting_id", std::string()); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_put_setting(void* agent, std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto values = clone_string_map(values_map); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.put_setting", {{"agent", agent_id(a)}, {"setting_id", setting_id}, {"name", name}, {"values", values}})); if (http_code) *http_code = j.value("http_code", 0u); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_setting_list(void* agent, std::string bundle_version, ProgressFn progress, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_progress_job("net.get_setting_list", "get_setting_list", a, {{"bundle_version", bundle_version}}, std::move(progress), std::move(cancel)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_setting_list2(void* agent, std::string bundle_version, CheckFn check, ProgressFn progress, WasCancelledFn cancel) { auto* a = require_agent(agent); return invoke_progress_check_job("net.get_setting_list2", "get_setting_list2", a, {{"bundle_version", bundle_version}}, std::move(check), std::move(progress), std::move(cancel)); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_delete_setting(void* agent, std::string setting_id) { auto* a = require_agent(agent); if (!a) return invalid_handle(); return RpcClient::instance().invoke_int("net.delete_setting", {{"agent", agent_id(a)}, {"setting_id", setting_id}}); }
SLICER_LINUX_RUNTIME_EXPORT std::string bambu_network_get_studio_info_url(void* agent) { auto* a = require_agent(agent); if (!a) return {}; return set_agent_cache(a, &RuntimeAgent::studio_info_url, RpcClient::instance().invoke_string("net.get_studio_info_url", {{"agent", agent_id(a)}})); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_set_extra_http_header(void* agent, std::map<std::string, std::string> headers) { auto* a = require_agent(agent); if (!a) return invalid_handle(); return RpcClient::instance().invoke_int("net.set_extra_http_header", {{"agent", agent_id(a)}, {"headers", headers}}); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_my_message(void* agent, int type, int after, int limit, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_my_message", {{"agent", agent_id(a)}, {"type", type}, {"after", after}, {"limit", limit}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_check_user_task_report(void* agent, int* task_id, bool* printable) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.check_user_task_report", {{"agent", agent_id(a)}})); if (task_id) *task_id = j.value("task_id", 0); if (printable) *printable = j.value("printable", false); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_user_print_info(void* agent, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_print_info", {{"agent", agent_id(a)}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_user_tasks(void* agent, TaskQueryParams params, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_tasks", {{"agent", agent_id(a)}, {"params", {{"dev_id", params.dev_id}, {"status", params.status}, {"offset", params.offset}, {"limit", params.limit}}}})); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_filament_spools(void* agent, FilamentQueryParams params, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_filament_spools", {{"agent", agent_id(a)}, {"category", params.category}, {"status", params.status}, {"spool_id", params.spool_id}, {"rfid", params.rfid}, {"offset", params.offset}, {"limit", params.limit}})); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_create_filament_spool(void* agent, std::string request_body, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.create_filament_spool", {{"agent", agent_id(a)}, {"request_body", request_body}})); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_update_filament_spool(void* agent, std::string spool_id, std::string request_body, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.update_filament_spool", {{"agent", agent_id(a)}, {"spool_id", spool_id}, {"request_body", request_body}})); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_delete_filament_spools(void* agent, FilamentDeleteParams params, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.delete_filament_spools", {{"agent", agent_id(a)}, {"ids", params.ids}, {"rfids", params.rfids}})); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_filament_config(void* agent, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_filament_config", {{"agent", agent_id(a)}})); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_sync_ams_filaments(void* agent, AmsSyncParams params, std::string* http_body)
{
    auto* a = require_agent(agent);
    if (!a)
        return invalid_handle();

    nlohmann::json items = nlohmann::json::array();
    for (const auto& item : params.items) {
        items.push_back({
            {"RFID", item.RFID},
            {"filamentVendor", item.filamentVendor},
            {"filamentType", item.filamentType},
            {"filamentName", item.filamentName},
            {"filamentId", item.filamentId},
            {"isSupport", item.isSupport},
            {"color", item.color},
            {"colorType", item.colorType},
            {"colors", item.colors},
            {"netWeight", item.netWeight},
            {"totalNetWeight", item.totalNetWeight},
            {"trayIdName", item.trayIdName},
            {"note", item.note},
            {"amsSn", item.amsSn},
            {"slotId", item.slotId},
            {"amsId", item.amsId},
            {"amsType", item.amsType},
            {"createNew", item.createNew}
        });
    }

    const auto j = ok_or_error(RpcClient::instance().invoke_json("net.sync_ams_filaments", {
        {"agent", agent_id(a)},
        {"dev_id", params.devId},
        {"items", std::move(items)}
    }));
    if (http_body)
        *http_body = j.value("http_body", std::string());
    return j.value("value", 0);
}
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_printer_firmware(void* agent, std::string dev_id, unsigned* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_printer_firmware", {{"agent", agent_id(a)}, {"dev_id", dev_id}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_task_plate_index(void* agent, std::string task_id, int* plate_index) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_task_plate_index", {{"agent", agent_id(a)}, {"task_id", task_id}})); if (plate_index) *plate_index = j.value("plate_index", -1); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_user_info(void* agent, int* identifier) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_user_info", {{"agent", agent_id(a)}})); if (identifier) *identifier = j.value("identifier", 0); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_request_bind_ticket(void* agent, std::string* ticket) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.request_bind_ticket", {{"agent", agent_id(a)}})); if (ticket) *ticket = j.value("ticket", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_subtask_info(void* agent, std::string subtask_id, std::string* task_json, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_subtask_info", {{"agent", agent_id(a)}, {"subtask_id", subtask_id}})); if (task_json) *task_json = j.value("task_json", std::string()); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_slice_info(void* agent, std::string project_id, std::string profile_id, int plate_index, std::string* slice_json) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_slice_info", {{"agent", agent_id(a)}, {"project_id", project_id}, {"profile_id", profile_id}, {"plate_index", plate_index}})); if (slice_json) *slice_json = j.value("slice_json", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_query_bind_status(void* agent, std::vector<std::string> query_list, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.query_bind_status", {{"agent", agent_id(a)}, {"query_list", query_list}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_modify_printer_name(void* agent, std::string dev_id, std::string dev_name) { auto* a = require_agent(agent); if (!a) return invalid_handle(); return RpcClient::instance().invoke_int("net.modify_printer_name", {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"dev_name", dev_name}}); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_camera_url(void* agent, std::string dev_id, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_camera_url", a, {{"agent", agent_id(a)}, {"dev_id", dev_id}}, callback); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_camera_url_for_golive(void* agent, std::string dev_id, std::string sdev_id, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_camera_url_for_golive", a, {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"sdev_id", sdev_id}}, callback); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_design_staffpick(void* agent, int offset, int limit, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_design_staffpick", a, {{"agent", agent_id(a)}, {"offset", offset}, {"limit", limit}}, callback); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_start_publish(void* agent, PublishParams params, OnUpdateStatusFn update, WasCancelledFn cancel, std::string* out) { auto* a = require_agent(agent); return invoke_job_with_wait("net.start_publish", "publish", a, JsonRuntime::to_json(normalize_runtime_paths(std::move(params))), std::move(update), std::move(cancel), OnWaitFn(), out); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_model_publish_url(void* agent, std::string* url) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_publish_url", {{"agent", agent_id(a)}})); if (url) *url = j.value("url", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_model_mall_home_url(void* agent, std::string* url) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_mall_home_url", {{"agent", agent_id(a)}})); if (url) *url = j.value("url", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_model_mall_detail_url(void* agent, std::string* url, std::string id) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_mall_detail_url", {{"agent", agent_id(a)}, {"id", id}})); if (url) *url = j.value("url", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_subtask(void* agent, BBLModelTask* task, OnGetSubTaskFn callback) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_subtask", {{"agent", agent_id(a)}, {"task", model_task_to_json(task)}})); const int ret = j.value("value", 0); if (ret == 0 && callback && j.contains("subtask") && j["subtask"].is_object()) { BBLModelTask local_task{}; json_to_model_task(j["subtask"], local_task); if (task) *task = local_task; callback(&local_task); } return ret; }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_my_profile(void* agent, std::string token, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_my_profile", {{"agent", agent_id(a)}, {"token", token}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_my_token(void* agent, std::string ticket, unsigned int* http_code, std::string* http_body) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_my_token", {{"agent", agent_id(a)}, {"ticket", ticket}})); if (http_code) *http_code = j.value("http_code", 0u); if (http_body) *http_body = j.value("http_body", std::string()); return j.value("value", 0); }

SLICER_LINUX_RUNTIME_EXPORT int bambu_network_track_enable(void* agent, bool enable) { auto* a = require_agent(agent); if (!a) return invalid_handle(); set_agent_cache(a, &RuntimeAgent::tracking_enabled, enable); return RpcClient::instance().invoke_int("net.track_enable", {{"agent", agent_id(a)}, {"enable", enable}}); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_track_remove_files(void* agent) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.track_remove_files", {{"agent", agent_id(a)}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_track_event(void* agent, std::string evt_key, std::string content) { auto* a = require_agent(agent); return a ? RpcClient::instance().invoke_int("net.track_event", {{"agent", agent_id(a)}, {"evt_key", evt_key}, {"content", content}}) : invalid_handle(); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_track_header(void* agent, std::string header) { auto* a = require_agent(agent); if (!a) return invalid_handle(); set_agent_cache(a, &RuntimeAgent::track_header, header); return RpcClient::instance().invoke_int("net.track_header", {{"agent", agent_id(a)}, {"header", header}}); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_track_update_property(void* agent, std::string name, std::string value, std::string type) { auto* a = require_agent(agent); if (!a) return invalid_handle(); { std::lock_guard<std::mutex> lock(a->state_mutex); a->track_properties[name] = value; } return RpcClient::instance().invoke_int("net.track_update_property", {{"agent", agent_id(a)}, {"name", name}, {"value", value}, {"type", type}}); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_track_get_property(void* agent, std::string name, std::string& value, std::string type) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.track_get_property", {{"agent", agent_id(a)}, {"name", name}, {"type", type}})); if (j.contains("property_value")) value = j.value("property_value", std::string()); else { std::lock_guard<std::mutex> lock(a->state_mutex); auto it = a->track_properties.find(name); if (it != a->track_properties.end()) value = it->second; } return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_put_model_mall_rating(void* agent, int rating_id, int score, std::string content, std::vector<std::string> images, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.put_model_mall_rating", {{"agent", agent_id(a)}, {"rating_id", rating_id}, {"score", score}, {"content", content}, {"images", images}})); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_oss_config(void* agent, std::string& config, std::string country_code, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_oss_config", {{"agent", agent_id(a)}, {"country_code", country_code}})); config = j.value("config", std::string()); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_put_rating_picture_oss(void* agent, std::string& config, std::string& pic_oss_path, std::string model_id, int profile_id, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.put_rating_picture_oss", {{"agent", agent_id(a)}, {"config", config}, {"pic_oss_path", pic_oss_path}, {"model_id", model_id}, {"profile_id", profile_id}})); config = j.value("config", config); pic_oss_path = j.value("pic_oss_path", pic_oss_path); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_model_mall_rating(void* agent, int job_id, std::string& rating_result, unsigned int& http_code, std::string& http_error) { auto* a = require_agent(agent); if (!a) return invalid_handle(); const auto j = ok_or_error(RpcClient::instance().invoke_json("net.get_model_mall_rating", {{"agent", agent_id(a)}, {"job_id", job_id}})); rating_result = j.value("rating_result", std::string()); http_code = j.value("http_code", 0u); http_error = j.value("http_error", std::string()); return j.value("value", 0); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_mw_user_preference(void* agent, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_mw_user_preference", a, {{"agent", agent_id(a)}}, callback); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_mw_user_4ulist(void* agent, int seed, int limit, std::function<void(std::string)> callback) { auto* a = require_agent(agent); return invoke_string_callback("net.get_mw_user_4ulist", a, {{"agent", agent_id(a)}, {"seed", seed}, {"limit", limit}}, callback); }
SLICER_LINUX_RUNTIME_EXPORT int bambu_network_get_hms_snapshot(void* agent, std::string& dev_id, std::string& file_name, std::function<void(std::string, int)> callback) { auto* a = require_agent(agent); return invoke_string_int_callback("net.get_hms_snapshot", a, {{"agent", agent_id(a)}, {"dev_id", dev_id}, {"file_name", file_name}}, callback); }
SLICER_LINUX_RUNTIME_EXPORT int slicer_linux_runtime_forwarder_active_tunnels()
{
    const auto count = active_runtime_tunnel_count();
    return count > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(count);
}

SLICER_LINUX_RUNTIME_EXPORT int slicer_linux_runtime_forwarder_active_callbacks()
{
    const auto count = active_queued_main_callback_count();
    return count > static_cast<std::size_t>(INT_MAX) ? INT_MAX : static_cast<int>(count);
}

SLICER_LINUX_RUNTIME_EXPORT void slicer_linux_runtime_forwarder_shutdown()
{
    bool expected = false;
    if (!g_forwarder_shutting_down.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return;

    EventPump::instance().request_stop();
    RpcClient::instance().shutdown();
    EventPump::instance().stop();
    join_forwarder_workers();
}

SLICER_LINUX_RUNTIME_EXPORT const char* bambu_network_get_last_error_msg() { return last_error_c_str(); }

SLICER_LINUX_RUNTIME_EXPORT int ft_abi_version()
{
    return 1;
}

SLICER_LINUX_RUNTIME_EXPORT void ft_free(void* p)
{
    std::free(p);
}

SLICER_LINUX_RUNTIME_EXPORT void ft_job_result_destroy(Slic3r::ft_job_result* result)
{
    if (!result)
        return;
    std::free(const_cast<char*>(result->json));
    std::free(const_cast<void*>(result->bin));
    result->ec = 0;
    result->resp_ec = 0;
    result->json = nullptr;
    result->bin = nullptr;
    result->bin_size = 0;
}

SLICER_LINUX_RUNTIME_EXPORT void ft_job_msg_destroy(Slic3r::ft_job_msg* msg)
{
    if (!msg)
        return;
    std::free(const_cast<char*>(msg->json));
    msg->kind = 0;
    msg->json = nullptr;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_tunnel_create(const char* url, Slic3r::FT_TunnelHandle** out)
{
    if (!out)
        return Slic3r::FT_EINVAL;
    *out = nullptr;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.tunnel_create", {{"url", std::string(url ? url : "")}}));
    const auto ret = ft_value(j);
    const auto remote = j.value("tunnel", 0LL);
    if (ret != Slic3r::FT_OK || remote == 0)
        return ret;
    auto* tunnel = new Slic3r::FT_TunnelHandle();
    tunnel->remote = remote;
    *out = tunnel;
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT void ft_tunnel_retain(Slic3r::FT_TunnelHandle* tunnel)
{
    retain_tunnel_handle(tunnel);
}

SLICER_LINUX_RUNTIME_EXPORT void ft_tunnel_release(Slic3r::FT_TunnelHandle* tunnel)
{
    release_tunnel_handle(tunnel);
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_tunnel_sync_connect(Slic3r::FT_TunnelHandle* tunnel);

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_tunnel_start_connect(Slic3r::FT_TunnelHandle* tunnel, void (*cb)(void*, int, int, const char*), void* user)
{
    if (!tunnel)
        return Slic3r::FT_EINVAL;
    retain_tunnel_handle(tunnel);
    {
        std::lock_guard<std::mutex> lock(tunnel->callback_mutex);
        tunnel->conn_cb = cb;
        tunnel->conn_user = user;
    }
    const auto ret = ft_tunnel_sync_connect(tunnel);
    void (*callback)(void*, int, int, const char*) = nullptr;
    void* callback_user = nullptr;
    if (!g_forwarder_shutting_down.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(tunnel->callback_mutex);
        callback = tunnel->conn_cb;
        callback_user = tunnel->conn_user;
        if (callback)
            ++tunnel->active_conn_callbacks;
    }
    if (callback) {
        callback(callback_user, ret == Slic3r::FT_OK ? 0 : 1, static_cast<int>(ret), ret == Slic3r::FT_OK ? "" : last_error_c_str());
        {
            std::lock_guard<std::mutex> lock(tunnel->callback_mutex);
            if (tunnel->active_conn_callbacks > 0)
                --tunnel->active_conn_callbacks;
        }
        tunnel->callback_cv.notify_all();
    }
    release_tunnel_handle(tunnel);
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_tunnel_sync_connect(Slic3r::FT_TunnelHandle* tunnel)
{
    if (!tunnel)
        return Slic3r::FT_EINVAL;
    notify_tunnel_status(tunnel, 0, 1, 0, "connecting");
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.tunnel_sync_connect", {{"tunnel", tunnel->remote}}));
    const auto ret = ft_value(j);
    notify_tunnel_status(tunnel, 1, ret == Slic3r::FT_OK ? 2 : -1, static_cast<int>(ret), ret == Slic3r::FT_OK ? "connected" : last_error_c_str());
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_tunnel_set_status_cb(Slic3r::FT_TunnelHandle* tunnel, void (*cb)(void*, int, int, int, const char*), void* user)
{
    if (!tunnel)
        return Slic3r::FT_EINVAL;
    std::unique_lock<std::mutex> lock(tunnel->callback_mutex);
    tunnel->status_cb = cb;
    tunnel->status_user = user;
    tunnel->callback_cv.wait(lock, [tunnel] { return tunnel->active_status_callbacks == 0; });
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_tunnel_shutdown(Slic3r::FT_TunnelHandle* tunnel)
{
    if (!tunnel)
        return Slic3r::FT_EINVAL;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.tunnel_shutdown", {{"tunnel", tunnel->remote}}));
    return ft_value(j);
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_job_create(const char* params_json, Slic3r::FT_JobHandle** out)
{
    if (!out)
        return Slic3r::FT_EINVAL;
    *out = nullptr;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.job_create", {{"params_json", std::string(params_json ? params_json : "")}}));
    const auto ret = ft_value(j);
    const auto remote = j.value("job", 0LL);
    if (ret != Slic3r::FT_OK || remote == 0)
        return ret;
    auto* job = new Slic3r::FT_JobHandle();
    job->remote = remote;
    *out = job;
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT void ft_job_retain(Slic3r::FT_JobHandle* job)
{
    retain_job_handle(job);
}

SLICER_LINUX_RUNTIME_EXPORT void ft_job_release(Slic3r::FT_JobHandle* job)
{
    release_job_handle(job);
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_job_set_result_cb(Slic3r::FT_JobHandle* job, void (*cb)(void*, Slic3r::ft_job_result), void* user)
{
    if (!job)
        return Slic3r::FT_EINVAL;
    {
        std::unique_lock<std::mutex> lock(job->callback_mutex);
        job->result_cb = cb;
        job->result_user = user;
        job->callback_cv.wait(lock, [job] { return job->active_result_callbacks == 0; });
    }
    if (cb && !ensure_ft_result_poller(job))
        return Slic3r::FT_EIO;
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_job_get_result(Slic3r::FT_JobHandle* job, uint32_t timeout_ms, Slic3r::ft_job_result* out_result)
{
    if (!job || !out_result)
        return Slic3r::FT_EINVAL;
    *out_result = {};
    const auto reply = RpcClient::instance().invoke_binary("ft.job_get_result", {{"job", job->remote}, {"timeout_ms", timeout_ms}});
    const auto j = ok_or_error(reply.payload);
    const auto ret = ft_value(j);
    if (ret != Slic3r::FT_OK)
        return ret;
    out_result->ec = j.value("ec", 0);
    out_result->resp_ec = j.value("resp_ec", 0);
    out_result->json = copy_c_string(j.value("json", std::string()));
    out_result->bin = copy_binary_buffer(reply.binary);
    out_result->bin_size = static_cast<uint32_t>(reply.binary.size());
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_tunnel_start_job(Slic3r::FT_TunnelHandle* tunnel, Slic3r::FT_JobHandle* job)
{
    if (!tunnel || !job)
        return Slic3r::FT_EINVAL;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.job_start", {{"tunnel", tunnel->remote}, {"job", job->remote}}));
    const auto ret = ft_value(j);
    if (ret == Slic3r::FT_OK) {
        {
            std::lock_guard<std::mutex> lock(job->callback_mutex);
            job->started = true;
        }
        if (!ensure_ft_message_poller(job) || !ensure_ft_result_poller(job))
            return Slic3r::FT_EIO;
    }
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_job_cancel(Slic3r::FT_JobHandle* job)
{
    if (!job)
        return Slic3r::FT_EINVAL;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.job_cancel", {{"job", job->remote}}));
    return ft_value(j);
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_job_set_msg_cb(Slic3r::FT_JobHandle* job, void (*cb)(void*, Slic3r::ft_job_msg), void* user)
{
    if (!job)
        return Slic3r::FT_EINVAL;
    {
        std::unique_lock<std::mutex> lock(job->callback_mutex);
        job->msg_cb = cb;
        job->msg_user = user;
        job->callback_cv.wait(lock, [job] { return job->active_msg_callbacks == 0; });
    }
    if (cb && !ensure_ft_message_poller(job))
        return Slic3r::FT_EIO;
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_job_try_get_msg(Slic3r::FT_JobHandle* job, Slic3r::ft_job_msg* out_msg)
{
    if (!job || !out_msg)
        return Slic3r::FT_EINVAL;
    *out_msg = {};
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.job_try_get_msg", {{"job", job->remote}}));
    const auto ret = ft_value(j);
    if (ret != Slic3r::FT_OK)
        return ret;
    out_msg->kind = j.value("kind", 0);
    out_msg->json = copy_c_string(j.value("json", std::string()));
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT Slic3r::ft_err ft_job_get_msg(Slic3r::FT_JobHandle* job, uint32_t timeout_ms, Slic3r::ft_job_msg* out_msg)
{
    if (!job || !out_msg)
        return Slic3r::FT_EINVAL;
    *out_msg = {};
    const auto j = ok_or_error(RpcClient::instance().invoke_json("ft.job_get_msg", {{"job", job->remote}, {"timeout_ms", timeout_ms}}));
    const auto ret = ft_value(j);
    if (ret != Slic3r::FT_OK)
        return ret;
    out_msg->kind = j.value("kind", 0);
    out_msg->json = copy_c_string(j.value("json", std::string()));
    return Slic3r::FT_OK;
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_Create(Bambu_Tunnel* tunnel, char const* path)
{
    if (!tunnel) return -1;
    const std::string path_value(path ? path : "");
    const bool is_bambu = path_value.rfind("bambu:///", 0) == 0;
    const bool is_tutk = path_value.rfind("bambu:///tutk", 0) == 0;
    BOOST_LOG_TRIVIAL(info) << "slicer-linux-runtime-forwarder: src.create.begin path_len=" << path_value.size()
                            << ", path_is_bambu=" << (is_bambu ? 1 : 0)
                            << ", path_is_tutk=" << (is_tutk ? 1 : 0);
    const auto j = ok_or_error(RpcClient::instance().invoke_json("src.create", {{"path", path_value}}));
    const int ret = j.value("value", -1);
    const auto remote = j.value("tunnel", 0LL);
    BOOST_LOG_TRIVIAL(info) << "slicer-linux-runtime-forwarder: src.create.end value=" << ret << ", tunnel=" << remote;
    if (ret != 0 || remote == 0) {
        *tunnel = nullptr;
        return ret;
    }
    auto* t = new RuntimeTunnel();
    t->remote_handle = remote;
    register_remote_tunnel(t);
    *tunnel = reinterpret_cast<Bambu_Tunnel>(t);
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT void Bambu_SetLogger(Bambu_Tunnel tunnel, Logger logger, void* context)
{
    auto lease = require_tunnel(tunnel);
    if (!lease)
        return;
    RuntimeTunnel* t = lease.get();
    {
        std::lock_guard<std::mutex> lock(t->state_mutex);
        t->logger = logger;
        t->logger_ctx = context;
    }
    RpcClient::instance().invoke_void("src.set_logger", {{"tunnel", t->remote_handle}, {"enabled", logger != nullptr}});
}

SLICER_LINUX_RUNTIME_EXPORT void Bambu_SetStreamInfoCallback(Bambu_Tunnel tunnel, StreamInfoCallback callback, void* context)
{
    auto lease = require_tunnel(tunnel);
    if (!lease)
        return;
    RuntimeTunnel* t = lease.get();
    {
        std::lock_guard<std::mutex> lock(t->state_mutex);
        t->stream_info_callback = callback;
        t->stream_info_ctx = context;
    }
    RpcClient::instance().invoke_void("src.set_stream_info_callback", {{"tunnel", t->remote_handle}, {"enabled", callback != nullptr}});
}

SLICER_LINUX_RUNTIME_EXPORT void Bambu_SetTrackReporter(Bambu_Tunnel tunnel, TrackReporter reporter, void* context)
{
    auto lease = require_tunnel(tunnel);
    if (!lease)
        return;
    RuntimeTunnel* t = lease.get();
    {
        std::lock_guard<std::mutex> lock(t->state_mutex);
        t->track_reporter = reporter;
        t->track_reporter_ctx = context;
    }
    RpcClient::instance().invoke_void("src.set_track_reporter", {{"tunnel", t->remote_handle}, {"enabled", reporter != nullptr}});
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_Open(Bambu_Tunnel tunnel)
{
    auto lease = require_tunnel(tunnel);
    if (!lease)
        return -1;
    RuntimeTunnel* t = lease.get();
    const int ret = RpcClient::instance().invoke_int("src.open", {{"tunnel", t->remote_handle}});
    {
        std::lock_guard<std::mutex> lock(t->state_mutex);
        t->opened = ret == 0;
    }
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_StartStream(Bambu_Tunnel tunnel, bool video)
{
    auto lease = require_tunnel(tunnel);
    return lease ? RpcClient::instance().invoke_int("src.start_stream", {{"tunnel", lease->remote_handle}, {"video", video}}) : -1;
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_StartStreamEx(Bambu_Tunnel tunnel, int type)
{
    auto lease = require_tunnel(tunnel);
    return lease ? RpcClient::instance().invoke_int("src.start_stream_ex", {{"tunnel", lease->remote_handle}, {"type", type}}) : -1;
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_GetStreamCount(Bambu_Tunnel tunnel)
{
    auto lease = require_tunnel(tunnel);
    return lease ? RpcClient::instance().invoke_int("src.get_stream_count", {{"tunnel", lease->remote_handle}}) : -1;
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_GetStreamInfo(Bambu_Tunnel tunnel, int index, Bambu_StreamInfo* info)
{
    auto lease = require_tunnel(tunnel);
    if (!lease || !info || index < 0)
        return -1;
    RuntimeTunnel* t = lease.get();
    const auto reply = RpcClient::instance().invoke_binary("src.get_stream_info", {{"tunnel", t->remote_handle}, {"index", index}});
    const auto j = ok_or_error(reply.payload);
    const int ret = j.value("value", -1);
    if (ret != 0 || !j.contains("info"))
        return ret;
    const auto& src = j["info"];
    info->type = static_cast<Bambu_StreamType>(src.value("type", 0));
    info->sub_type = src.value("sub_type", 0);
    info->format_type = src.value("format_type", 0);
    info->format_size = src.value("format_size", 0);
    info->max_frame_size = src.value("max_frame_size", 0);
    if (info->type == VIDE) {
        info->format.video.width = src.value("width", 0);
        info->format.video.height = src.value("height", 0);
        info->format.video.frame_rate = src.value("frame_rate", 0);
    } else {
        info->format.audio.sample_rate = src.value("sample_rate", 0);
        info->format.audio.channel_count = src.value("channel_count", 0);
        info->format.audio.sample_size = src.value("sample_size", 0);
    }
    {
        std::lock_guard<std::mutex> lock(t->state_mutex);
        while (static_cast<int>(t->stream_format_buffers.size()) <= index)
            t->stream_format_buffers.emplace_back();
        auto& storage = t->stream_format_buffers[static_cast<std::size_t>(index)];
        storage = reply.binary;
        info->format_buffer = storage.empty() ? nullptr : storage.data();
        info->format_size = static_cast<int>(storage.size());
    }
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT unsigned long Bambu_GetDuration(Bambu_Tunnel tunnel)
{
    auto lease = require_tunnel(tunnel);
    if (!lease)
        return 0;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("src.get_duration", {{"tunnel", lease->remote_handle}}));
    return j.value("value", 0UL);
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_Seek(Bambu_Tunnel tunnel, unsigned long time)
{
    auto lease = require_tunnel(tunnel);
    return lease ? RpcClient::instance().invoke_int("src.seek", {{"tunnel", lease->remote_handle}, {"time", time}}) : -1;
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_ReadSample(Bambu_Tunnel tunnel, Bambu_Sample* sample)
{
    auto lease = require_tunnel(tunnel);
    if (!lease || !sample)
        return -1;
    RuntimeTunnel* t = lease.get();
    const auto reply = RpcClient::instance().invoke_binary("src.read_sample", {{"tunnel", t->remote_handle}});
    const auto j = ok_or_error(reply.payload);
    const int ret = j.value("value", -1);
    if (ret != 0 || !j.contains("sample"))
        return ret;

    const auto& src = j["sample"];
    CachedSample cached;
    cached.buffer = reply.binary;
    cached.itrack = src.value("itrack", 0);
    cached.size = src.value("size", 0);
    cached.flags = src.value("flags", 0);
    cached.decode_time = src.value("decode_time", 0ULL);
    {
        std::lock_guard<std::mutex> lock(t->state_mutex);
        t->sample_queue.clear();
        t->sample_queue.push_back(std::move(cached));
        auto& front = t->sample_queue.front();
        sample->itrack = front.itrack;
        sample->size = front.size;
        sample->flags = front.flags;
        sample->decode_time = front.decode_time;
        sample->buffer = front.buffer.empty() ? nullptr : front.buffer.data();
    }
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_SendMessage(Bambu_Tunnel tunnel, int ctrl, char const* data, int len)
{
    auto lease = require_tunnel(tunnel);
    if (!lease || len < 0)
        return -1;
    std::vector<unsigned char> binary;
    if (data && len > 0)
        binary.assign(reinterpret_cast<const unsigned char*>(data), reinterpret_cast<const unsigned char*>(data) + static_cast<std::size_t>(len));
    const auto reply = RpcClient::instance().invoke_binary("src.send_message", {{"tunnel", lease->remote_handle}, {"ctrl", ctrl}}, binary);
    return reply.payload.value("value", -1);
}

SLICER_LINUX_RUNTIME_EXPORT int Bambu_RecvMessage(Bambu_Tunnel tunnel, int* ctrl, char* data, int* len)
{
    auto lease = require_tunnel(tunnel);
    if (!lease || !len || *len < 0)
        return -1;
    const int caller_buffer_size = *len;
    const auto reply = RpcClient::instance().invoke_binary("src.recv_message", {{"tunnel", lease->remote_handle}, {"buffer_size", caller_buffer_size}});
    const auto j = ok_or_error(reply.payload);
    const int ret = j.value("value", -1);
    if (ctrl)
        *ctrl = j.value("ctrl", 0);
    if (ret != 0) {
        if (j.contains("required_len"))
            *len = j.value("required_len", caller_buffer_size);
        return ret;
    }
    const int needed = j.contains("message_len") ? j.value("message_len", static_cast<int>(reply.binary.size()))
                                                  : static_cast<int>(reply.binary.size());
    if (needed < 0 || static_cast<std::size_t>(needed) > reply.binary.size()) {
        *len = 0;
        return -1;
    }
    if (!data || caller_buffer_size < needed) {
        *len = needed;
        return -1;
    }
    if (needed > 0)
        std::memcpy(data, reply.binary.data(), static_cast<std::size_t>(needed));
    *len = needed;
    return ret;
}

SLICER_LINUX_RUNTIME_EXPORT int slicer_linux_runtime_http_request(
    std::string method,
    std::string url,
    std::string headers_json,
    std::string request_body,
    std::string multipart_json,
    std::string range,
    unsigned long long max_bytes,
    long connect_timeout_ms,
    long timeout_ms,
    int (*progress_cb)(void*, unsigned long long, unsigned long long, unsigned long long, unsigned long long, double),
    bool (*cancel_cb)(void*),
    void* callback_user,
    unsigned int* http_status,
    std::string* response_body,
    std::string* response_headers,
    std::string* primary_ip,
    std::string* error)
{
    nlohmann::json headers = nlohmann::json::array();
    nlohmann::json multipart_parts = nlohmann::json::array();
    try {
        if (!headers_json.empty())
            headers = nlohmann::json::parse(headers_json);
        if (!multipart_json.empty())
            multipart_parts = nlohmann::json::parse(multipart_json);
    } catch (const std::exception& e) {
        if (error)
            *error = std::string("invalid Linux HTTP metadata JSON: ") + e.what();
        return -1;
    }

    std::vector<unsigned char> binary(request_body.begin(), request_body.end());
    auto start = RpcClient::instance().invoke_binary("http.start", {
        {"method", std::move(method)},
        {"url", std::move(url)},
        {"headers", std::move(headers)},
        {"multipart_parts", std::move(multipart_parts)},
        {"range", std::move(range)},
        {"max_bytes", max_bytes},
        {"connect_timeout_ms", connect_timeout_ms},
        {"timeout_ms", timeout_ms},
        {"user_agent", std::string(SLIC3R_APP_NAME) + "/" + SLIC3R_VERSION}
    }, binary);
    if (!start.payload.value("ok", false)) {
        if (error)
            *error = start.payload.value("error", std::string("failed to start Linux HTTP request"));
        return -1;
    }

    const std::int64_t job = start.payload.value("job", 0LL);
    if (job == 0) {
        if (error)
            *error = "Linux HTTP request returned an invalid job ID";
        return -1;
    }

    std::string accumulated_body;
    bool cancel_sent = false;
    nlohmann::json final_status;
    for (;;) {
        if (g_forwarder_shutting_down.load(std::memory_order_acquire)) {
            if (error)
                *error = "Linux runtime forwarder is shutting down";
            break;
        }

        const auto status_reply = RpcClient::instance().invoke_binary("http.status", {{"job", job}});
        const auto& status = status_reply.payload;
        if (!status.value("ok", false)) {
            if (error)
                *error = status.value("error", std::string("Linux HTTP status failed"));
            break;
        }

        if (!status_reply.binary.empty()) {
            const std::size_t expected_offset = status.value("chunk_offset", accumulated_body.size());
            if (expected_offset != accumulated_body.size()) {
                if (error)
                    *error = "Linux HTTP response stream offset mismatch";
                break;
            }
            if (status_reply.binary.size() > max_bytes || accumulated_body.size() > max_bytes - status_reply.binary.size()) {
                if (error)
                    *error = "Linux HTTP response exceeds limit";
                break;
            }
            accumulated_body.append(reinterpret_cast<const char*>(status_reply.binary.data()), status_reply.binary.size());
            if (response_body)
                *response_body = accumulated_body;
        }

        const auto download_total = status.value("download_total", 0ULL);
        const auto download_now = status.value("download_now", 0ULL);
        const auto upload_total = status.value("upload_total", 0ULL);
        const auto upload_now = status.value("upload_now", 0ULL);
        const double upload_speed = status.value("upload_speed", 0.0);
        bool should_cancel = false;
        try {
            should_cancel = cancel_cb && cancel_cb(callback_user);
            if (progress_cb && progress_cb(callback_user, download_total, download_now, upload_total, upload_now, upload_speed) != 0)
                should_cancel = true;
        } catch (const std::exception& e) {
            should_cancel = true;
            if (error)
                *error = std::string("Linux HTTP callback failed: ") + e.what();
        } catch (...) {
            should_cancel = true;
            if (error)
                *error = "Linux HTTP callback failed";
        }
        if (should_cancel && !cancel_sent) {
            RpcClient::instance().invoke_void("http.cancel", {{"job", job}});
            cancel_sent = true;
        }

        if (status.value("done", false)) {
            final_status = status;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    RpcClient::instance().invoke_void("http.release", {{"job", job}});

    if (http_status)
        *http_status = final_status.value("http_status", 0U);
    if (response_body)
        *response_body = accumulated_body;
    if (response_headers)
        *response_headers = final_status.value("response_headers", std::string());
    if (primary_ip)
        *primary_ip = final_status.value("primary_ip", std::string());
    if (error && error->empty())
        *error = final_status.value("error", std::string());
    return final_status.value("transport_ok", false) ? 0 : -1;
}

SLICER_LINUX_RUNTIME_EXPORT int slicer_linux_runtime_http_get(
    std::string url,
    std::string headers_json,
    unsigned int* http_status,
    std::string* body,
    std::string* error)
{
    std::string response_headers;
    std::string primary_ip;
    const int rc = slicer_linux_runtime_http_request("GET", std::move(url), std::move(headers_json), {}, {}, {},
        512ULL * 1024ULL * 1024ULL, 15000, 600000,
        nullptr, nullptr, nullptr,
        http_status, body, &response_headers, &primary_ip, error);
    if (rc == 0 && http_status && (*http_status < 200 || *http_status >= 300)) {
        if (error)
            *error = "HTTP status " + std::to_string(*http_status);
        return -1;
    }
    return rc;
}

SLICER_LINUX_RUNTIME_EXPORT std::string slicer_linux_runtime_auth_start(void* agent, std::string login_url)
{
    auto* a = require_agent(agent);
    if (!a)
        return nlohmann::json({{"ok", false}, {"state", "error"}, {"error", "invalid runtime agent"}}).dump();
    return RpcClient::instance().invoke_json("auth.start", {
        {"agent", agent_id(a)},
        {"login_url", std::move(login_url)},
        {"client_version", SLIC3R_VERSION},
        {"language", "en"},
        {"theme", "light"}
    }).dump();
}

SLICER_LINUX_RUNTIME_EXPORT std::string slicer_linux_runtime_auth_start_v2(
    void* agent,
    std::string login_url,
    std::string client_version,
    std::string language,
    bool dark_mode)
{
    auto* a = require_agent(agent);
    if (!a)
        return nlohmann::json({{"ok", false}, {"state", "error"}, {"error", "invalid runtime agent"}}).dump();
    return RpcClient::instance().invoke_json("auth.start", {
        {"agent", agent_id(a)},
        {"login_url", std::move(login_url)},
        {"client_version", std::move(client_version)},
        {"language", std::move(language)},
        {"theme", dark_mode ? "dark" : "light"}
    }).dump();
}

SLICER_LINUX_RUNTIME_EXPORT std::string slicer_linux_runtime_auth_status(void* agent)
{
    auto* a = require_agent(agent);
    if (!a)
        return nlohmann::json({{"ok", false}, {"state", "error"}, {"error", "invalid runtime agent"}}).dump();
    return RpcClient::instance().invoke_json("auth.status", {{"agent", agent_id(a)}}).dump();
}

SLICER_LINUX_RUNTIME_EXPORT int slicer_linux_runtime_auth_cancel(void* agent)
{
    auto* a = require_agent(agent);
    if (!a)
        return -1;
    const auto reply = RpcClient::instance().invoke_json("auth.cancel", {{"agent", agent_id(a)}, {"reason", "cancelled"}});
    return reply.value("ok", false) ? 0 : -1;
}

SLICER_LINUX_RUNTIME_EXPORT std::string slicer_linux_runtime_auth_capabilities()
{
    return RpcClient::instance().invoke_json("auth.capabilities").dump();
}

SLICER_LINUX_RUNTIME_EXPORT std::string slicer_linux_runtime_browser_start(void* agent, std::string url, bool bind_ticket)
{
    std::int64_t remote_agent = 0;
    if (bind_ticket) {
        auto* a = require_agent(agent);
        if (!a)
            return nlohmann::json({{"ok", false}, {"state", "error"}, {"error", "invalid runtime agent"}}).dump();
        remote_agent = agent_id(a);
    }
    return RpcClient::instance().invoke_json("browser.start", {
        {"agent", remote_agent},
        {"url", std::move(url)},
        {"bind_ticket", bind_ticket}
    }).dump();
}

SLICER_LINUX_RUNTIME_EXPORT std::string slicer_linux_runtime_browser_status()
{
    return RpcClient::instance().invoke_json("browser.status").dump();
}

SLICER_LINUX_RUNTIME_EXPORT std::string slicer_linux_runtime_browser_command(std::string command_json)
{
    try {
        auto payload = nlohmann::json::parse(command_json);
        if (!payload.is_object())
            throw std::runtime_error("browser command must be an object");
        return RpcClient::instance().invoke_json("browser.command", payload).dump();
    } catch (const std::exception& e) {
        return nlohmann::json({{"ok", false}, {"error", e.what()}}).dump();
    }
}

SLICER_LINUX_RUNTIME_EXPORT int slicer_linux_runtime_browser_cancel()
{
    const auto reply = RpcClient::instance().invoke_json("browser.cancel", {{"reason", "cancelled"}});
    return reply.value("ok", false) ? 0 : -1;
}

SLICER_LINUX_RUNTIME_EXPORT void Bambu_Close(Bambu_Tunnel tunnel)
{
    auto lease = require_tunnel(tunnel);
    if (!lease)
        return;
    RpcClient::instance().invoke_void("src.close", {{"tunnel", lease->remote_handle}});
    std::lock_guard<std::mutex> lock(lease->state_mutex);
    lease->opened = false;
}

SLICER_LINUX_RUNTIME_EXPORT void Bambu_Destroy(Bambu_Tunnel tunnel)
{
    RuntimeTunnel* t = begin_destroy_tunnel_handle(tunnel);
    if (!t)
        return;
    const std::int64_t remote = t->remote_handle;
    {
        std::lock_guard<std::mutex> lock(t->state_mutex);
        t->logger = nullptr;
        t->logger_ctx = nullptr;
        t->stream_info_callback = nullptr;
        t->stream_info_ctx = nullptr;
        t->track_reporter = nullptr;
        t->track_reporter_ctx = nullptr;
        t->opened = false;
    }
    if (!g_forwarder_shutting_down.load(std::memory_order_acquire))
        RpcClient::instance().invoke_void("src.destroy", {{"tunnel", remote}});
    delete t;
}
SLICER_LINUX_RUNTIME_EXPORT int Bambu_Init() { return RpcClient::instance().invoke_int("src.init"); }
SLICER_LINUX_RUNTIME_EXPORT void Bambu_GetSessionStat(Bambu_Tunnel tunnel, Bambu_SessionStat* stat)
{
    if (!stat)
        return;
    *stat = {};
    auto lease = require_tunnel(tunnel);
    if (!lease)
        return;
    const auto j = ok_or_error(RpcClient::instance().invoke_json("src.get_session_stat", {{"tunnel", lease->remote_handle}}));
    if (!j.value("ok", false))
        return;
    stat->session_duration_ms = j.value("session_duration_ms", 0LL);
    stat->freeze_total_duration_ms = j.value("freeze_total_duration_ms", 0LL);
    stat->freeze_count = j.value("freeze_count", 0);
    stat->avg_fps = j.value("avg_fps", 0.0f);
    stat->avg_bitrate_kbps = j.value("avg_bitrate_kbps", 0.0f);
    stat->avg_jitter_ms = j.value("avg_jitter_ms", 0.0f);
    stat->max_jitter_ms = j.value("max_jitter_ms", 0.0f);
}
SLICER_LINUX_RUNTIME_EXPORT void Bambu_Deinit() { RpcClient::instance().invoke_void("src.deinit"); }
SLICER_LINUX_RUNTIME_EXPORT char const* Bambu_GetLastErrorMsg() { const auto j = RpcClient::instance().invoke_json("src.get_last_error_msg"); if (j.value("ok", false) && j.contains("message")) set_last_error(j.value("message", std::string())); return last_error_c_str(); }
SLICER_LINUX_RUNTIME_EXPORT void Bambu_FreeLogMsg(tchar const* msg) { (void)msg; }

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
