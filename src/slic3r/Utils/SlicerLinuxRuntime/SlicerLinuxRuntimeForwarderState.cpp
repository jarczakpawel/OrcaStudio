#include "SlicerLinuxRuntimeForwarderState.hpp"
#include "SlicerLinuxRuntimeRpcClient.hpp"

#include <functional>
#if defined(_WIN32)
#include <codecvt>
#include <locale>
#endif

namespace Slic3r::SlicerLinuxRuntime {
namespace {
std::mutex g_remote_agents_mutex;
std::map<std::int64_t, RuntimeAgent*> g_remote_agents;
std::mutex g_remote_tunnels_mutex;
std::map<std::int64_t, RuntimeTunnel*> g_remote_tunnels;
std::map<RuntimeTunnel*, RuntimeTunnel*> g_tunnel_handles;
std::atomic<std::size_t> g_queued_main_callbacks{0};

class QueuedMainCallback {
public:
    explicit QueuedMainCallback(std::function<void()> fn) : m_fn(std::move(fn))
    {
        ++g_queued_main_callbacks;
    }

    ~QueuedMainCallback()
    {
        --g_queued_main_callbacks;
    }

    void invoke()
    {
        if (m_fn)
            m_fn();
    }

private:
    std::function<void()> m_fn;
};

void run_or_queue(const BBL::QueueOnMainFn& queue_on_main, std::function<void()> fn)
{
    if (!fn)
        return;
    if (queue_on_main) {
        auto pending = std::make_shared<QueuedMainCallback>(std::move(fn));
        queue_on_main([pending] { pending->invoke(); });
    } else {
        fn();
    }
}

#if defined(_WIN32)
std::wstring utf8_to_wstring(const std::string& s)
{
    try {
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        return conv.from_bytes(s);
    } catch (...) {
        return std::wstring(s.begin(), s.end());
    }
}
#endif
}

RuntimeAgent* as_agent(void* handle)
{
    return reinterpret_cast<RuntimeAgent*>(handle);
}

void* new_agent(const std::string& log_dir)
{
    auto* agent = new RuntimeAgent();
    agent->log_dir = log_dir;
    return agent;
}

RuntimeAgentLease& RuntimeAgentLease::operator=(RuntimeAgentLease&& other) noexcept
{
    if (this != &other) {
        if (m_agent) {
            {
                std::lock_guard<std::mutex> lock(m_agent->state_mutex);
                if (m_agent->active_dispatches > 0)
                    --m_agent->active_dispatches;
            }
            m_agent->dispatch_cv.notify_all();
        }
        m_agent = other.m_agent;
        other.m_agent = nullptr;
    }
    return *this;
}

RuntimeAgentLease::~RuntimeAgentLease()
{
    if (!m_agent)
        return;
    {
        std::lock_guard<std::mutex> lock(m_agent->state_mutex);
        if (m_agent->active_dispatches > 0)
            --m_agent->active_dispatches;
    }
    m_agent->dispatch_cv.notify_all();
}

RuntimeTunnelLease& RuntimeTunnelLease::operator=(RuntimeTunnelLease&& other) noexcept
{
    if (this != &other) {
        if (m_tunnel) {
            {
                std::lock_guard<std::mutex> lock(m_tunnel->state_mutex);
                if (m_tunnel->active_dispatches > 0)
                    --m_tunnel->active_dispatches;
            }
            m_tunnel->dispatch_cv.notify_all();
        }
        m_tunnel = other.m_tunnel;
        other.m_tunnel = nullptr;
    }
    return *this;
}

RuntimeTunnelLease::~RuntimeTunnelLease()
{
    if (!m_tunnel)
        return;
    {
        std::lock_guard<std::mutex> lock(m_tunnel->state_mutex);
        if (m_tunnel->active_dispatches > 0)
            --m_tunnel->active_dispatches;
    }
    m_tunnel->dispatch_cv.notify_all();
}

int delete_agent(void* handle)
{
    auto* agent = as_agent(handle);
    if (!agent)
        return 0;
    unregister_remote_agent(agent);

    std::vector<std::shared_ptr<RuntimeJobState>> jobs;
    {
        std::lock_guard<std::mutex> lock(agent->jobs_mutex);
        for (auto& [id, job] : agent->jobs)
            jobs.push_back(job);
        agent->jobs.clear();
    }
    for (auto& job : jobs) {
        if (!job)
            continue;
        job->stop_cancel_watch = true;
        if (job->cancel_watch.joinable())
            job->cancel_watch.join();
    }
    delete agent;
    return 0;
}

RuntimeTunnel* as_tunnel_handle(void* handle)
{
    return reinterpret_cast<RuntimeTunnel*>(handle);
}

void register_remote_agent(RuntimeAgent* agent)
{
    if (!agent || !agent->remote_handle)
        return;
    std::lock_guard<std::mutex> map_lock(g_remote_agents_mutex);
    std::lock_guard<std::mutex> state_lock(agent->state_mutex);
    agent->destroying = false;
    g_remote_agents[agent->remote_handle] = agent;
}

void unregister_remote_agent(RuntimeAgent* agent)
{
    if (!agent)
        return;
    std::unique_lock<std::mutex> state_lock(agent->state_mutex, std::defer_lock);
    {
        std::lock_guard<std::mutex> map_lock(g_remote_agents_mutex);
        state_lock.lock();
        agent->destroying = true;
        auto it = g_remote_agents.find(agent->remote_handle);
        if (it != g_remote_agents.end() && it->second == agent)
            g_remote_agents.erase(it);
    }
    agent->dispatch_cv.wait(state_lock, [agent] { return agent->active_dispatches == 0; });
}

RuntimeAgentLease acquire_remote_agent(std::int64_t remote_handle)
{
    std::lock_guard<std::mutex> map_lock(g_remote_agents_mutex);
    auto it = g_remote_agents.find(remote_handle);
    if (it == g_remote_agents.end() || !it->second)
        return {};
    RuntimeAgent* agent = it->second;
    std::lock_guard<std::mutex> state_lock(agent->state_mutex);
    if (agent->destroying)
        return {};
    ++agent->active_dispatches;
    return RuntimeAgentLease(agent);
}

void register_remote_tunnel(RuntimeTunnel* tunnel)
{
    if (!tunnel || !tunnel->remote_handle)
        return;
    std::lock_guard<std::mutex> map_lock(g_remote_tunnels_mutex);
    std::lock_guard<std::mutex> state_lock(tunnel->state_mutex);
    tunnel->destroying = false;
    g_remote_tunnels[tunnel->remote_handle] = tunnel;
    g_tunnel_handles[tunnel] = tunnel;
}

void unregister_remote_tunnel(RuntimeTunnel* tunnel)
{
    if (!tunnel)
        return;
    std::unique_lock<std::mutex> state_lock(tunnel->state_mutex, std::defer_lock);
    {
        std::lock_guard<std::mutex> map_lock(g_remote_tunnels_mutex);
        state_lock.lock();
        tunnel->destroying = true;
        auto it = g_remote_tunnels.find(tunnel->remote_handle);
        if (it != g_remote_tunnels.end() && it->second == tunnel)
            g_remote_tunnels.erase(it);
        g_tunnel_handles.erase(tunnel);
    }
    tunnel->dispatch_cv.wait(state_lock, [tunnel] { return tunnel->active_dispatches == 0; });
}

RuntimeTunnelLease acquire_remote_tunnel(std::int64_t remote_handle)
{
    std::lock_guard<std::mutex> map_lock(g_remote_tunnels_mutex);
    auto it = g_remote_tunnels.find(remote_handle);
    if (it == g_remote_tunnels.end() || !it->second)
        return {};
    RuntimeTunnel* tunnel = it->second;
    std::lock_guard<std::mutex> state_lock(tunnel->state_mutex);
    if (tunnel->destroying)
        return {};
    ++tunnel->active_dispatches;
    return RuntimeTunnelLease(tunnel);
}

RuntimeTunnelLease acquire_tunnel_handle(void* handle)
{
    auto* requested = as_tunnel_handle(handle);
    if (!requested)
        return {};
    std::lock_guard<std::mutex> map_lock(g_remote_tunnels_mutex);
    auto it = g_tunnel_handles.find(requested);
    if (it == g_tunnel_handles.end() || !it->second)
        return {};
    RuntimeTunnel* tunnel = it->second;
    std::lock_guard<std::mutex> state_lock(tunnel->state_mutex);
    if (tunnel->destroying)
        return {};
    ++tunnel->active_dispatches;
    return RuntimeTunnelLease(tunnel);
}

RuntimeTunnel* begin_destroy_tunnel_handle(void* handle)
{
    auto* requested = as_tunnel_handle(handle);
    if (!requested)
        return nullptr;
    std::unique_lock<std::mutex> state_lock(requested->state_mutex, std::defer_lock);
    {
        std::lock_guard<std::mutex> map_lock(g_remote_tunnels_mutex);
        auto handle_it = g_tunnel_handles.find(requested);
        if (handle_it == g_tunnel_handles.end() || handle_it->second != requested)
            return nullptr;
        state_lock.lock();
        if (requested->destroying)
            return nullptr;
        requested->destroying = true;
        g_tunnel_handles.erase(handle_it);
        auto remote_it = g_remote_tunnels.find(requested->remote_handle);
        if (remote_it != g_remote_tunnels.end() && remote_it->second == requested)
            g_remote_tunnels.erase(remote_it);
    }
    requested->dispatch_cv.wait(state_lock, [requested] { return requested->active_dispatches == 0; });
    return requested;
}

std::size_t active_runtime_tunnel_count()
{
    std::lock_guard<std::mutex> lock(g_remote_tunnels_mutex);
    return g_tunnel_handles.size();
}

std::size_t active_queued_main_callback_count()
{
    return g_queued_main_callbacks.load(std::memory_order_acquire);
}

void dispatch_agent_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload)
{
    auto lease = acquire_remote_agent(remote_handle);
    if (!lease)
        return;
    RuntimeAgent* agent = lease.get();

    if (name == "on_ssdp_msg") {
        BBL::OnMsgArrivedFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = agent->on_ssdp_msg;
            queue = agent->queue_on_main;
        }
        const auto msg = payload.value("dev_info_json_str", std::string());
        if (cb) run_or_queue(queue, [cb, msg] { cb(msg); });
        return;
    }
    if (name == "on_user_login") {
        const int online_login = payload.value("online_login", 0);
        const bool login = payload.value("login", false);
        BBL::OnUserLoginFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            agent->logged_in = login;
            cb = agent->on_user_login;
            queue = agent->queue_on_main;
        }
        if (cb) run_or_queue(queue, [cb, online_login, login] { cb(online_login, login); });
        return;
    }
    if (name == "on_printer_connected") {
        BBL::OnPrinterConnectedFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = agent->on_printer_connected;
            queue = agent->queue_on_main;
        }
        const auto topic = payload.value("topic_str", std::string());
        if (cb) run_or_queue(queue, [cb, topic] { cb(topic); });
        return;
    }
    if (name == "on_server_connected") {
        const int return_code = payload.value("return_code", 0);
        const int reason_code = payload.value("reason_code", 0);
        BBL::OnServerConnectedFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            agent->server_connected = return_code == 0;
            cb = agent->on_server_connected;
            queue = agent->queue_on_main;
        }
        if (cb) run_or_queue(queue, [cb, return_code, reason_code] { cb(return_code, reason_code); });
        return;
    }
    if (name == "on_http_error") {
        BBL::OnHttpErrorFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = agent->on_http_error;
            queue = agent->queue_on_main;
        }
        const unsigned http_code = payload.value("http_code", 0u);
        const auto body = payload.value("http_body", std::string());
        if (cb) run_or_queue(queue, [cb, http_code, body] { cb(http_code, body); });
        return;
    }
    if (name == "on_subscribe_failure") {
        BBL::GetSubscribeFailureFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = agent->on_subscribe_failure;
            queue = agent->queue_on_main;
        }
        const auto topic = payload.value("topic", std::string());
        if (cb) run_or_queue(queue, [cb, topic] { cb(topic); });
        return;
    }
    if (name == "callback.get_country_code") {
        BBL::GetCountryCodeFn cb;
        std::string fallback;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = agent->get_country_code;
            fallback = agent->country_code;
        }
        const std::string value = cb ? cb() : fallback;
        RpcClient::instance().invoke_void("runtime.callback_reply", {{"request_id", payload.value("request_id", 0LL)}, {"value", value}});
        return;
    }
    if (name == "on_message" || name == "on_user_message" || name == "on_local_message") {
        BBL::OnMessageFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = name == "on_message" ? agent->on_message :
                 name == "on_user_message" ? agent->on_user_message : agent->on_local_message;
            queue = agent->queue_on_main;
        }
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg = payload.value("msg", std::string());
        if (cb) run_or_queue(queue, [cb, dev_id, msg] { cb(dev_id, msg); });
        return;
    }
    if (name == "on_local_connect") {
        BBL::OnLocalConnectedFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = agent->on_local_connect;
            queue = agent->queue_on_main;
        }
        const int status = payload.value("status", 0);
        const auto dev_id = payload.value("dev_id", std::string());
        const auto msg = payload.value("msg", std::string());
        if (cb) run_or_queue(queue, [cb, status, dev_id, msg] { cb(status, dev_id, msg); });
        return;
    }
    if (name == "on_server_error") {
        BBL::OnServerErrFn cb;
        BBL::QueueOnMainFn queue;
        {
            std::lock_guard<std::mutex> lock(agent->state_mutex);
            cb = agent->on_server_error;
            queue = agent->queue_on_main;
        }
        const auto url = payload.value("url", std::string());
        const int status = payload.value("status", 0);
        if (cb) run_or_queue(queue, [cb, url, status] { cb(url, status); });
        return;
    }

    BBL::QueueOnMainFn queue;
    {
        std::lock_guard<std::mutex> lock(agent->state_mutex);
        queue = agent->queue_on_main;
    }
    if (name == "job.update_status") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        if (job && job->on_update_status) {
            const int status = payload.value("status", 0);
            const int code = payload.value("code", 0);
            const auto msg = payload.value("msg", std::string());
            auto cb = job->on_update_status;
            run_or_queue(queue, [cb, status, code, msg] { cb(status, code, msg); });
        }
        return;
    }
    if (name == "job.progress") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        if (job && job->on_progress) {
            const int progress = payload.value("progress", 0);
            auto cb = job->on_progress;
            run_or_queue(queue, [cb, progress] { cb(progress); });
        }
        return;
    }
    if (name == "job.check") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        bool reply = true;
        if (job && job->on_check && payload.contains("info") && payload["info"].is_object()) {
            std::map<std::string, std::string> info;
            for (auto it = payload["info"].begin(); it != payload["info"].end(); ++it)
                info[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
            reply = job->on_check(info);
        }
        RpcClient::instance().invoke_void("runtime.job_wait_reply", {{"job_id", payload.value("job_id", 0LL)}, {"request_id", payload.value("request_id", 0LL)}, {"reply", reply}});
        return;
    }
    if (name == "job.wait") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        bool reply = true;
        if (job && job->on_wait)
            reply = job->on_wait(payload.value("status", 0), payload.value("job_info", std::string()));
        RpcClient::instance().invoke_void("runtime.job_wait_reply", {{"job_id", payload.value("job_id", 0LL)}, {"request_id", payload.value("request_id", 0LL)}, {"reply", reply}});
        return;
    }
    if (name == "job.complete") {
        auto job = find_job_state(agent, payload.value("job_id", 0LL));
        if (job && job->out_string && payload.contains("out"))
            *job->out_string = payload.value("out", std::string());
    }
}

void dispatch_tunnel_event(std::int64_t remote_handle, const std::string& name, const nlohmann::json& payload)
{
    auto lease = acquire_remote_tunnel(remote_handle);
    if (!lease)
        return;
    RuntimeTunnel* tunnel = lease.get();

    if (name == "logger") {
        Logger logger = nullptr;
        void* logger_ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(tunnel->state_mutex);
            logger = tunnel->logger;
            logger_ctx = tunnel->logger_ctx;
        }
        if (!logger)
            return;
        const int level = payload.value("level", 0);
        const auto message = payload.value("message", std::string());
#if defined(_WIN32)
        const std::wstring wide = utf8_to_wstring(message);
        logger(logger_ctx, level, wide.c_str());
#else
        logger(logger_ctx, level, message.c_str());
#endif
        return;
    }

    if (name == "stream_info") {
        StreamInfoCallback callback = nullptr;
        void* callback_ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(tunnel->state_mutex);
            callback = tunnel->stream_info_callback;
            callback_ctx = tunnel->stream_info_ctx;
        }
        if (!callback)
            return;

        Bambu_StreamInfo info{};
        info.type = static_cast<Bambu_StreamType>(payload.value("type", 0));
        info.sub_type = payload.value("sub_type", 0);
        info.format_type = payload.value("format_type", 0);
        info.format_size = payload.value("format_size", 0);
        info.max_frame_size = payload.value("max_frame_size", 0);
        if (info.type == VIDE) {
            info.format.video.width = payload.value("width", 0);
            info.format.video.height = payload.value("height", 0);
            info.format.video.frame_rate = payload.value("frame_rate", 0);
        } else {
            info.format.audio.sample_rate = payload.value("sample_rate", 0);
            info.format.audio.channel_count = payload.value("channel_count", 0);
            info.format.audio.sample_size = payload.value("sample_size", 0);
        }
        std::vector<unsigned char> format_buffer;
        auto format_it = payload.find("format_buffer");
        if (format_it != payload.end() && format_it->is_array()) {
            format_buffer.reserve(format_it->size());
            for (const auto& byte : *format_it) {
                if (byte.is_number_unsigned())
                    format_buffer.push_back(static_cast<unsigned char>(byte.get<unsigned int>() & 0xffU));
                else if (byte.is_number_integer())
                    format_buffer.push_back(static_cast<unsigned char>(byte.get<int>() & 0xff));
            }
        }
        info.format_buffer = format_buffer.empty() ? nullptr : format_buffer.data();
        info.format_size = static_cast<int>(format_buffer.size());
        callback(callback_ctx, &info);
        return;
    }

    if (name == "track_event") {
        TrackReporter reporter = nullptr;
        void* reporter_ctx = nullptr;
        {
            std::lock_guard<std::mutex> lock(tunnel->state_mutex);
            reporter = tunnel->track_reporter;
            reporter_ctx = tunnel->track_reporter_ctx;
        }
        if (!reporter)
            return;

        const std::string event_name = payload.value("event_name", std::string());
        const std::string module = payload.value("module", std::string());
        const std::string phase = payload.value("phase", std::string());
        const std::string result = payload.value("result", std::string());
        const std::string error_code = payload.value("error_code", std::string());
        const std::string error_message = payload.value("error_message", std::string());
        const std::string event_data_body = payload.value("event_data_body", std::string());
        PlayerEventC event{
            event_name.empty() ? nullptr : event_name.c_str(),
            module.empty() ? nullptr : module.c_str(),
            phase.empty() ? nullptr : phase.c_str(),
            result.empty() ? nullptr : result.c_str(),
            error_code.empty() ? nullptr : error_code.c_str(),
            error_message.empty() ? nullptr : error_message.c_str(),
            event_data_body.empty() ? nullptr : event_data_body.c_str()
        };
        reporter(reporter_ctx, &event);
    }
}

std::shared_ptr<RuntimeJobState> register_job_state(RuntimeAgent* agent, const std::shared_ptr<RuntimeJobState>& job)
{
    if (!agent || !job)
        return nullptr;
    std::lock_guard<std::mutex> lock(agent->jobs_mutex);
    agent->jobs[job->job_id] = job;
    return job;
}

std::shared_ptr<RuntimeJobState> find_job_state(RuntimeAgent* agent, std::int64_t job_id)
{
    if (!agent)
        return nullptr;
    std::lock_guard<std::mutex> lock(agent->jobs_mutex);
    auto it = agent->jobs.find(job_id);
    return it == agent->jobs.end() ? nullptr : it->second;
}

void unregister_job_state(RuntimeAgent* agent, std::int64_t job_id)
{
    if (!agent)
        return;
    std::shared_ptr<RuntimeJobState> job;
    {
        std::lock_guard<std::mutex> lock(agent->jobs_mutex);
        auto it = agent->jobs.find(job_id);
        if (it != agent->jobs.end()) {
            job = it->second;
            agent->jobs.erase(it);
        }
    }
    if (job) {
        job->stop_cancel_watch = true;
        if (job->cancel_watch.joinable())
            job->cancel_watch.join();
    }
}

}
