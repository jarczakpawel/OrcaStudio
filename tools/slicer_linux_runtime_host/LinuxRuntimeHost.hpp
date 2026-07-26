#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <functional>
#include <filesystem>

namespace Slic3r::SlicerLinuxRuntime {


struct HostResource {
    void* handle{nullptr};
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t active_calls{0};
    bool closing{false};
};

template <typename T>
class HostResourceLease {
public:
    HostResourceLease() = default;
    explicit HostResourceLease(std::shared_ptr<HostResource> resource)
        : m_resource(std::move(resource))
    {
    }
    HostResourceLease(const HostResourceLease&) = delete;
    HostResourceLease& operator=(const HostResourceLease&) = delete;
    HostResourceLease(HostResourceLease&& other) noexcept
        : m_resource(std::move(other.m_resource))
    {
    }
    HostResourceLease& operator=(HostResourceLease&& other) noexcept
    {
        if (this != &other) {
            release();
            m_resource = std::move(other.m_resource);
        }
        return *this;
    }
    ~HostResourceLease() { release(); }

    explicit operator bool() const { return m_resource && m_resource->handle; }
    operator T() const { return m_resource ? static_cast<T>(m_resource->handle) : T{}; }
    T get() const { return m_resource ? static_cast<T>(m_resource->handle) : T{}; }

private:
    void release()
    {
        if (!m_resource)
            return;
        {
            std::lock_guard<std::mutex> lock(m_resource->mutex);
            if (m_resource->active_calls > 0)
                --m_resource->active_calls;
        }
        m_resource->cv.notify_all();
        m_resource.reset();
    }

    std::shared_ptr<HostResource> m_resource;
};

struct HostRetiredCallbackContext {
    std::int64_t tunnel_id{0};
    void* pointer{nullptr};
    void (*destroy)(void*){nullptr};
};

struct HostJobState {
    std::int64_t job_id{0};
    std::int64_t agent_handle{0};
    std::string kind;
    std::atomic<bool> cancel_requested{false};
    std::mutex wait_mutex;
    std::condition_variable wait_cv;
    std::int64_t wait_request_id{0};
    bool wait_reply_ready{false};
    bool wait_reply_value{true};
};



struct HostHttpJobState {
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> cancel_requested{false};
    bool done{false};
    std::uint64_t download_total{0};
    std::uint64_t download_now{0};
    std::uint64_t upload_total{0};
    std::uint64_t upload_now{0};
    double upload_speed{0.0};
    std::size_t reported_response_bytes{0};
    nlohmann::json result;
    std::vector<unsigned char> response_binary;
};

struct HostAuthSession {
    std::string mode{"auth"};
    long long agent_handle{0};
    int process_group{0};
    std::string session_id;
    std::string password;
    int host_novnc_port{0};
    int guest_novnc_port{0};
    int guest_vnc_port{0};
    std::filesystem::path state_dir;
    std::filesystem::path result_file;
    std::filesystem::path command_file;
    std::filesystem::path event_file;
    std::uintmax_t event_offset{0};
    std::string state{"idle"};
    std::string error;
    bool processed{false};
};

struct HostCallbackReplyState {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready{false};
    std::string string_value;
};


struct HostFtJobState {
    void* handle{nullptr};
    std::mutex mutex;
    std::condition_variable cv;
    bool result_ready{false};
    bool shutting_down{false};
    bool result_callback_enabled{false};
    bool msg_callback_enabled{false};
    std::size_t active_callbacks{0};
    int result_ec{0};
    int result_resp_ec{0};
    std::string result_json;
    std::vector<unsigned char> result_bin;
    std::deque<std::pair<int, std::string>> messages;
    void* result_destroy{nullptr};
    void* msg_destroy{nullptr};
    void* free_mem{nullptr};
};

class LinuxRuntimeHost {
public:
    LinuxRuntimeHost();
    ~LinuxRuntimeHost();
    void begin_shutdown();
    nlohmann::json handle(const std::string& method, const nlohmann::json& payload);
    static void set_thread_request_binary(std::vector<unsigned char> data);
    static bool consume_thread_reply_binary(std::vector<unsigned char>& out);
    void dispatch_logger_event(std::int64_t tunnel_handle, int level, const std::string& message);
    void dispatch_stream_info_event(std::int64_t tunnel_handle, const nlohmann::json& payload);
    void dispatch_track_event(std::int64_t tunnel_handle, const nlohmann::json& payload);
    std::string refresh_camera_url_for_ft(const std::string& device, const std::string& dev_ver, const std::string& channel);
    std::string refresh_agora_url_ptr_string() const;

private:
    void load_modules();
    void* resolve_component(const char* name);
    void* resolve_source(const char* name);
    bool has_component_symbol(const char* name);
    nlohmann::json auth_capabilities() const;
    nlohmann::json auth_browser_capabilities() const;
    nlohmann::json start_auth_browser(const nlohmann::json& payload);
    nlohmann::json start_generic_browser(const nlohmann::json& payload);
    nlohmann::json start_browser_session(const std::string& mode,
                                        std::int64_t agent_id,
                                        const std::string& url,
                                        const std::string& client_version,
                                        const std::string& language,
                                        const std::string& theme);
    nlohmann::json auth_browser_status();
    nlohmann::json generic_browser_status();
    nlohmann::json browser_command(const nlohmann::json& payload);
    nlohmann::json drain_browser_events();
    nlohmann::json cancel_auth_browser(const std::string& reason);
    nlohmann::json process_auth_browser_result(const nlohmann::json& result);
    nlohmann::json perform_http_request(const nlohmann::json& payload);
    nlohmann::json perform_http_request_impl(const nlohmann::json& payload,
                                             std::vector<unsigned char> request_body,
                                             const std::shared_ptr<HostHttpJobState>& job);
    nlohmann::json start_http_request(const nlohmann::json& payload);
    nlohmann::json http_request_status(const nlohmann::json& payload);
    nlohmann::json cancel_http_request(const nlohmann::json& payload);
    nlohmann::json release_http_request(const nlohmann::json& payload);
    void stop_auth_browser_process();
    void clear_auth_profile();
    nlohmann::json not_supported(const std::string& method) const;
    void queue_event(std::int64_t agent_handle, const std::string& name, const nlohmann::json& payload);
    void queue_tunnel_event(std::int64_t tunnel_handle, const std::string& name, const nlohmann::json& payload);
    nlohmann::json drain_events(std::size_t limit);
    std::shared_ptr<HostJobState> get_job(std::int64_t job_id);
    void register_job(const std::shared_ptr<HostJobState>& job);
    void unregister_job(std::int64_t job_id);
    void set_job_cancel(std::int64_t job_id, bool value);
    void set_job_wait_reply(std::int64_t job_id, std::int64_t request_id, bool value);
    std::shared_ptr<HostCallbackReplyState> register_callback_request(std::int64_t request_id);
    void unregister_callback_request(std::int64_t request_id);
    void set_callback_reply(std::int64_t request_id, const std::string& value);
    void queue_main_task(std::function<void()> fn);
    void ensure_main_dispatcher();
    void stop_main_dispatcher();
    void cleanup_resources() noexcept;
    void main_dispatch_loop();

    template <typename T>
    T net(const char* name)
    {
        return reinterpret_cast<T>(resolve_component(name));
    }

    template <typename T>
    T src(const char* name)
    {
        return reinterpret_cast<T>(resolve_source(name));
    }

    void* m_component{nullptr};
    void* m_source{nullptr};
    bool m_module_load_attempted{false};
    std::atomic<std::int64_t> m_next_agent{1};
    std::atomic<std::int64_t> m_next_tunnel{1};
    std::atomic<std::int64_t> m_next_ft_tunnel{1};
    std::atomic<std::int64_t> m_next_ft_job{1};
    std::atomic<std::int64_t> m_next_http_job{1};
    std::map<std::int64_t, std::shared_ptr<HostResource>> m_agents;
    std::map<std::int64_t, std::shared_ptr<HostResource>> m_tunnels;
    std::map<std::int64_t, std::shared_ptr<HostResource>> m_ft_tunnels;
    std::map<std::int64_t, std::shared_ptr<HostResource>> m_ft_jobs;
    std::map<std::int64_t, std::shared_ptr<HostFtJobState>> m_ft_job_states;
    std::map<std::int64_t, std::shared_ptr<HostHttpJobState>> m_http_jobs;
    std::mutex m_http_jobs_mutex;
    std::map<std::int64_t, std::string> m_country_codes;
    mutable std::mutex m_state_mutex;
    mutable std::recursive_mutex m_module_mutex;
    std::mutex m_events_mutex;
    std::deque<nlohmann::json> m_events;
    std::map<std::int64_t, std::shared_ptr<HostJobState>> m_jobs;
    std::map<std::int64_t, std::shared_ptr<HostCallbackReplyState>> m_callback_replies;
    std::atomic<std::int64_t> m_next_wait_request{1};
    std::atomic<std::int64_t> m_next_callback_request{1};
    std::map<std::int64_t, void*> m_logger_contexts;
    std::map<std::int64_t, void*> m_stream_info_contexts;
    std::map<std::int64_t, void*> m_track_reporter_contexts;
    std::vector<HostRetiredCallbackContext> m_retired_callback_contexts;
    std::mutex m_main_tasks_mutex;
    std::condition_variable m_main_tasks_cv;
    std::deque<std::function<void()>> m_main_tasks;
    std::thread m_main_dispatcher;
    std::atomic<bool> m_stop_main_dispatcher{false};
    std::string m_component_status{"not_loaded"};
    std::string m_component_actual_abi_version;
    std::string m_source_status{"not_loaded"};
    std::unique_ptr<HostAuthSession> m_auth_session;
    std::mutex m_auth_mutex;
    std::atomic<bool> m_shutting_down{false};
};

}
