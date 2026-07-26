#include "FileTransferUtils.hpp"

namespace Slic3r {

FileTransferModule::FileTransferModule(ModuleHandle networking_module, int required_abi_version) : networking_(networking_module)
{
    // basic
    ft_abi_version        = sym_lookup<fn_ft_abi_version>(networking_, "ft_abi_version");
    ft_free               = sym_lookup<fn_ft_free>(networking_, "ft_free");
    ft_job_result_destroy = sym_lookup<fn_ft_job_result_destroy>(networking_, "ft_job_result_destroy");
    ft_job_msg_destroy    = sym_lookup<fn_ft_job_msg_destroy>(networking_, "ft_job_msg_destroy");

    // tunnel
    ft_tunnel_create        = sym_lookup<fn_ft_tunnel_create>(networking_, "ft_tunnel_create");
    ft_tunnel_retain        = sym_lookup<fn_ft_tunnel_retain>(networking_, "ft_tunnel_retain");
    ft_tunnel_release       = sym_lookup<fn_ft_tunnel_release>(networking_, "ft_tunnel_release");
    ft_tunnel_start_connect = sym_lookup<fn_ft_tunnel_start_connect>(networking_, "ft_tunnel_start_connect");
    ft_tunnel_sync_connect  = sym_lookup<fn_ft_tunnel_sync_connect>(networking_, "ft_tunnel_sync_connect");
    ft_tunnel_set_status_cb = sym_lookup<fn_ft_tunnel_set_status_cb>(networking_, "ft_tunnel_set_status_cb");
    ft_tunnel_shutdown      = sym_lookup<fn_ft_tunnel_shutdown>(networking_, "ft_tunnel_shutdown");

    // job
    ft_job_create        = sym_lookup<fn_ft_job_create>(networking_, "ft_job_create");
    ft_job_retain        = sym_lookup<fn_ft_job_retain>(networking_, "ft_job_retain");
    ft_job_release       = sym_lookup<fn_ft_job_release>(networking_, "ft_job_release");
    ft_job_set_result_cb = sym_lookup<fn_ft_job_set_result_cb>(networking_, "ft_job_set_result_cb");
    ft_job_get_result    = sym_lookup<fn_ft_job_get_result>(networking_, "ft_job_get_result");
    ft_tunnel_start_job  = sym_lookup<fn_ft_tunnel_start_job>(networking_, "ft_tunnel_start_job");
    ft_job_cancel        = sym_lookup<fn_ft_job_cancel>(networking_, "ft_job_cancel");

    ft_job_set_msg_cb  = sym_lookup<fn_ft_job_set_msg_cb>(networking_, "ft_job_set_msg_cb");
    ft_job_try_get_msg = sym_lookup<fn_ft_job_try_get_msg>(networking_, "ft_job_try_get_msg");
    ft_job_get_msg     = sym_lookup<fn_ft_job_get_msg>(networking_, "ft_job_get_msg");

    if (!ft_abi_version)
        throw std::runtime_error("Bambu networking plugin does not export ft_abi_version");
    const int actual_abi_version = ft_abi_version();
    if (actual_abi_version < required_abi_version)
        throw std::runtime_error("Bambu networking plugin file-transfer ABI is too old");
}

FileTransferTunnel::FileTransferTunnel(std::shared_ptr<FileTransferModule> m, const std::string &url) : m_(std::move(m))
{
    if (!m_ || !m_->ft_tunnel_create || !m_->ft_tunnel_set_status_cb) {
        throw std::runtime_error("Bambu networking plugin is too old: missing ft_tunnel_* symbols. Please update the networking plugin.");
    }
    FT_TunnelHandle *handle{};
    if (m_->ft_tunnel_create(url.c_str(), &handle) != FT_OK || !handle)
        throw std::runtime_error("ft_tunnel_create failed");
    h_ = handle;

    auto callback = [](void *user, int old_status, int new_status, int err_code, const char *msg) noexcept {
        auto *self = static_cast<FileTransferTunnel *>(user);
        if (!self)
            return;
        const bool deliver = self->enter_callback();
        TunnelStatusCb fn;
        if (deliver) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->status_ = new_status;
            fn = self->status_cb_;
        }
        try {
            if (fn)
                fn(old_status, new_status, err_code, std::string(msg ? msg : ""));
        } catch (...) {}
        self->leave_callback();
    };
    if (m_->ft_tunnel_set_status_cb(h_, callback, this) != FT_OK) {
        m_->ft_tunnel_release(h_);
        h_ = nullptr;
        throw std::runtime_error("ft_tunnel_set_status_cb failed");
    }
}

bool FileTransferTunnel::enter_callback() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_callbacks_;
    return !closing_;
}

void FileTransferTunnel::leave_callback() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_callbacks_ > 0)
        --active_callbacks_;
    if (closing_ && active_callbacks_ == 0)
        callback_cv_.notify_all();
}

void FileTransferTunnel::reset() noexcept
{
    FT_TunnelHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_)
            return;
        closing_ = true;
        handle = h_;
        h_ = nullptr;
        mod = m_;
    }
    if (handle && mod) {
        try {
            if (mod->ft_tunnel_set_status_cb)
                (void) mod->ft_tunnel_set_status_cb(handle, nullptr, nullptr);
            if (mod->ft_tunnel_shutdown)
                (void) mod->ft_tunnel_shutdown(handle);
            if (mod->ft_tunnel_release)
                mod->ft_tunnel_release(handle);
        } catch (...) {}
    }
    std::unique_lock<std::mutex> lock(mutex_);
    callback_cv_.wait(lock, [this] { return active_callbacks_ == 0; });
    conn_cb_ = {};
    status_cb_ = {};
    m_.reset();
}

void FileTransferTunnel::start_connect()
{
    FT_TunnelHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            throw std::runtime_error("tunnel handle invalid");
        handle = h_;
        mod = m_;
    }
    auto callback = [](void *user, int ok, int ec, const char *msg) noexcept {
        auto *self = static_cast<FileTransferTunnel *>(user);
        if (!self)
            return;
        const bool deliver = self->enter_callback();
        ConnectionCb fn;
        if (deliver) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            fn = self->conn_cb_;
        }
        try {
            if (fn)
                fn(ok == 0, ec, std::string(msg ? msg : ""));
        } catch (...) {}
        self->leave_callback();
    };
    if (!mod || !mod->ft_tunnel_start_connect || mod->ft_tunnel_start_connect(handle, callback, this) != FT_OK)
        throw std::runtime_error("ft_tunnel_start_connect failed");
}

bool FileTransferTunnel::sync_start_connect()
{
    FT_TunnelHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            return false;
        handle = h_;
        mod = m_;
    }
    return mod && mod->ft_tunnel_sync_connect && mod->ft_tunnel_sync_connect(handle) == FT_OK;
}

void FileTransferTunnel::on_connection(ConnectionCb cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closing_)
        conn_cb_ = std::move(cb);
}

void FileTransferTunnel::on_status(TunnelStatusCb cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closing_)
        status_cb_ = std::move(cb);
}

void FileTransferTunnel::shutdown()
{
    FT_TunnelHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            return;
        handle = h_;
        mod = m_;
    }
    if (mod && mod->ft_tunnel_shutdown)
        (void) mod->ft_tunnel_shutdown(handle);
}

int FileTransferTunnel::get_status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

bool FileTransferTunnel::check_valid() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !closing_ && h_ != nullptr;
}

FT_TunnelHandle *FileTransferTunnel::native() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_ ? nullptr : h_;
}

FileTransferJob::FileTransferJob(std::shared_ptr<FileTransferModule> m, const std::string &params_json) : m_(std::move(m))
{
    if (!m_ || !m_->ft_job_create || !m_->ft_job_set_result_cb)
        throw std::runtime_error("Bambu networking plugin is too old: missing ft_job_* symbols. Please update the networking plugin.");

    FT_JobHandle *handle{};
    if (m_->ft_job_create(params_json.c_str(), &handle) != FT_OK || !handle)
        throw std::runtime_error("ft_job_create failed");
    h_ = handle;

    auto callback = [](void *user, ft_job_result result) noexcept {
        auto *self = static_cast<FileTransferJob *>(user);
        if (!self)
            return;
        const bool deliver = self->enter_callback();
        ResultCb fn;
        int res = 0;
        int resp_ec = 0;
        std::string json;
        std::vector<std::byte> bin;
        std::shared_ptr<FileTransferModule> mod;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (deliver) {
                self->finished_ = true;
                self->solve_result_locked(result);
                res = self->res_;
                resp_ec = self->resp_ec_;
                json = self->res_json_;
                bin = self->res_bin_;
                fn = self->result_cb_;
            }
            mod = self->m_;
        }
        try {
            if (fn)
                fn(res, resp_ec, std::move(json), std::move(bin));
        } catch (...) {}
        try {
            if (mod && mod->ft_job_result_destroy)
                mod->ft_job_result_destroy(&result);
            else if (mod && mod->ft_free) {
                if (result.json)
                    mod->ft_free(const_cast<char *>(result.json));
                if (result.bin)
                    mod->ft_free(const_cast<void *>(result.bin));
            }
        } catch (...) {}
        self->leave_callback();
    };

    if (m_->ft_job_set_result_cb(h_, callback, this) != FT_OK) {
        m_->ft_job_release(h_);
        h_ = nullptr;
        throw std::runtime_error("ft_job_set_result_cb failed");
    }
}

bool FileTransferJob::enter_callback() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++active_callbacks_;
    return !closing_;
}

void FileTransferJob::leave_callback() noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_callbacks_ > 0)
        --active_callbacks_;
    if (closing_ && active_callbacks_ == 0)
        callback_cv_.notify_all();
}

void FileTransferJob::reset() noexcept
{
    FT_JobHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_)
            return;
        closing_ = true;
        handle = h_;
        h_ = nullptr;
        mod = m_;
    }
    if (handle && mod) {
        try {
            if (mod->ft_job_set_result_cb)
                (void) mod->ft_job_set_result_cb(handle, nullptr, nullptr);
            if (mod->ft_job_set_msg_cb)
                (void) mod->ft_job_set_msg_cb(handle, nullptr, nullptr);
            if (mod->ft_job_cancel)
                (void) mod->ft_job_cancel(handle);
            if (mod->ft_job_release)
                mod->ft_job_release(handle);
        } catch (...) {}
    }
    std::unique_lock<std::mutex> lock(mutex_);
    callback_cv_.wait(lock, [this] { return active_callbacks_ == 0; });
    result_cb_ = {};
    msg_cb_ = {};
    m_.reset();
}

void FileTransferJob::on_result(ResultCb cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!closing_)
        result_cb_ = std::move(cb);
}

bool FileTransferJob::get_result(int &ec, int &resp_ec, std::string &json, std::vector<std::byte> &bin, uint32_t timeout_ms)
{
    FT_JobHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            return false;
        handle = h_;
        mod = m_;
    }
    if (!mod || !mod->ft_job_get_result)
        return false;
    ft_job_result result{};
    if (mod->ft_job_get_result(handle, timeout_ms, &result) != FT_OK)
        return false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        solve_result_locked(result);
        finished_ = true;
        ec = res_;
        resp_ec = resp_ec_;
        json = res_json_;
        bin = res_bin_;
    }
    if (mod->ft_job_result_destroy)
        mod->ft_job_result_destroy(&result);
    else if (mod->ft_free) {
        if (result.json)
            mod->ft_free(const_cast<char *>(result.json));
        if (result.bin)
            mod->ft_free(const_cast<void *>(result.bin));
    }
    return true;
}

void FileTransferJob::start_on(FileTransferTunnel &t)
{
    FT_JobHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            throw std::runtime_error("job handle invalid");
        handle = h_;
        mod = m_;
    }
    FT_TunnelHandle *tunnel = t.native();
    if (!tunnel || !mod || !mod->ft_tunnel_start_job || mod->ft_tunnel_start_job(tunnel, handle) != FT_OK)
        throw std::runtime_error("ft_tunnel_start_job failed");
}

void FileTransferJob::on_msg(MsgCb cb)
{
    FT_JobHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            return;
        msg_cb_ = std::move(cb);
        handle = h_;
        mod = m_;
    }
    auto callback = [](void *user, ft_job_msg msg) noexcept {
        auto *self = static_cast<FileTransferJob *>(user);
        if (!self)
            return;
        const bool deliver = self->enter_callback();
        MsgCb fn;
        std::shared_ptr<FileTransferModule> callback_mod;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (deliver)
                fn = self->msg_cb_;
            callback_mod = self->m_;
        }
        try {
            if (fn)
                fn(msg.kind, std::string(msg.json ? msg.json : ""));
        } catch (...) {}
        try {
            if (callback_mod && callback_mod->ft_job_msg_destroy)
                callback_mod->ft_job_msg_destroy(&msg);
            else if (callback_mod && callback_mod->ft_free && msg.json)
                callback_mod->ft_free(const_cast<char *>(msg.json));
        } catch (...) {}
        self->leave_callback();
    };
    if (!mod || !mod->ft_job_set_msg_cb || mod->ft_job_set_msg_cb(handle, callback, this) != FT_OK)
        throw std::runtime_error("ft_job_set_msg_cb failed");
}

bool FileTransferJob::try_get_msg(int &kind, std::string &json)
{
    FT_JobHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            return false;
        handle = h_;
        mod = m_;
    }
    if (!mod || !mod->ft_job_try_get_msg)
        return false;
    ft_job_msg msg{};
    if (mod->ft_job_try_get_msg(handle, &msg) != FT_OK)
        return false;
    kind = msg.kind;
    json.assign(msg.json ? msg.json : "");
    if (mod->ft_job_msg_destroy)
        mod->ft_job_msg_destroy(&msg);
    else if (mod->ft_free && msg.json)
        mod->ft_free(const_cast<char *>(msg.json));
    return true;
}

bool FileTransferJob::get_msg(uint32_t timeout_ms, int &kind, std::string &json)
{
    FT_JobHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            return false;
        handle = h_;
        mod = m_;
    }
    if (!mod || !mod->ft_job_get_msg)
        return false;
    ft_job_msg msg{};
    if (mod->ft_job_get_msg(handle, timeout_ms, &msg) != FT_OK)
        return false;
    kind = msg.kind;
    json.assign(msg.json ? msg.json : "");
    if (mod->ft_job_msg_destroy)
        mod->ft_job_msg_destroy(&msg);
    else if (mod->ft_free && msg.json)
        mod->ft_free(const_cast<char *>(msg.json));
    return true;
}

FT_JobHandle *FileTransferJob::native() const noexcept
{
    std::lock_guard<std::mutex> lock(mutex_);
    return closing_ ? nullptr : h_;
}

bool FileTransferJob::check_valid() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return !closing_ && h_ != nullptr;
}

bool FileTransferJob::finished() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return finished_;
}

void FileTransferJob::cancel()
{
    FT_JobHandle *handle = nullptr;
    std::shared_ptr<FileTransferModule> mod;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closing_ || !h_)
            return;
        handle = h_;
        mod = m_;
    }
    if (mod && mod->ft_job_cancel)
        (void) mod->ft_job_cancel(handle);
}

void FileTransferJob::solve_result_locked(const ft_job_result &result)
{
    res_ = result.ec;
    resp_ec_ = result.resp_ec;
    res_bin_.clear();
    if (result.bin && result.bin_size) {
        const auto *first = static_cast<const std::byte *>(result.bin);
        res_bin_.assign(first, first + result.bin_size);
    }
    res_json_.assign(result.json ? result.json : "");
}

} // namespace Slic3r