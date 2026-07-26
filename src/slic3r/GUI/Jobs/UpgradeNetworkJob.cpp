#include "UpgradeNetworkJob.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/SlicerLinuxRuntime/SlicerLinuxRuntimeConfig.hpp"

#include <boost/process.hpp>
#if defined(_WIN32)
#include <boost/process/windows.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <functional>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace Slic3r {
namespace GUI {

wxDEFINE_EVENT(EVT_UPGRADE_UPDATE_MESSAGE, wxCommandEvent);
wxDEFINE_EVENT(EVT_UPGRADE_NETWORK_SUCCESS, wxCommandEvent);
wxDEFINE_EVENT(EVT_DOWNLOAD_NETWORK_FAILED, wxCommandEvent);
wxDEFINE_EVENT(EVT_INSTALL_NETWORK_FAILED, wxCommandEvent);

namespace {

namespace bp = boost::process;

struct RuntimeCommandResult {
    int exit_code {-1};
    bool canceled {false};
    std::string output;
};

std::string read_text_file(const fs::path& path)
{
    std::ifstream stream(path.string(), std::ios::in | std::ios::binary);
    if (!stream)
        return {};
    std::ostringstream out;
    out << stream.rdbuf();
    return out.str();
}

RuntimeCommandResult run_runtime_command(const std::string& executable,
                                         const std::vector<std::string>& arguments,
                                         const std::function<bool()>& cancel_requested)
{
    RuntimeCommandResult result;
    const std::string suffix = std::to_string(get_current_pid()) + "-" +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const fs::path stdout_path = fs::temp_directory_path() / ("bsos-runtime-" + suffix + ".out");
    const fs::path stderr_path = fs::temp_directory_path() / ("bsos-runtime-" + suffix + ".err");

    boost::system::error_code cleanup_error;
    fs::remove(stdout_path, cleanup_error);
    cleanup_error.clear();
    fs::remove(stderr_path, cleanup_error);

    try {
#if defined(_WIN32)
        bp::child child(executable, bp::args(arguments),
                        bp::std_out > stdout_path.string(), bp::std_err > stderr_path.string(),
                        bp::windows::create_no_window, bp::windows::hide);
#else
        bp::child child(executable, bp::args(arguments),
                        bp::std_out > stdout_path.string(), bp::std_err > stderr_path.string());
#endif
        while (child.running()) {
            if (cancel_requested && cancel_requested()) {
                result.canceled = true;
                child.terminate();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        child.wait();
        result.exit_code = result.canceled ? -1 : child.exit_code();
    } catch (const std::exception& e) {
        result.output = e.what();
    }

    const std::string stdout_text = read_text_file(stdout_path);
    const std::string stderr_text = read_text_file(stderr_path);
    if (!stderr_text.empty()) {
        if (!result.output.empty())
            result.output += "\n";
        result.output += stderr_text;
    }
    if (!stdout_text.empty()) {
        if (!result.output.empty())
            result.output += "\n";
        result.output += stdout_text;
    }
    if (result.output.size() > 16000)
        result.output.erase(0, result.output.size() - 16000);

    cleanup_error.clear();
    fs::remove(stdout_path, cleanup_error);
    cleanup_error.clear();
    fs::remove(stderr_path, cleanup_error);
    return result;
}

void log_runtime_result(const char* phase, const RuntimeCommandResult& result)
{
    BOOST_LOG_TRIVIAL(info) << phase << ": exit_code=" << result.exit_code
                            << ", canceled=" << result.canceled;
    if (!result.output.empty()) {
        if (result.exit_code == 0)
            BOOST_LOG_TRIVIAL(info) << phase << ": " << result.output;
        else
            BOOST_LOG_TRIVIAL(error) << phase << ": " << result.output;
    }
}

bool install_requested_network_runtime(const std::function<bool()>& cancel_requested,
                                       std::string& error)
{
    error.clear();
    if (!Slic3r::SlicerLinuxRuntime::enabled())
        return true;

    const fs::path component_dir = fs::path(Slic3r::data_dir()) / "plugins";
    const fs::path component_cache_dir = fs::path(Slic3r::data_dir()) / "ota" / "plugins";
    boost::system::error_code directory_error;
    fs::create_directories(component_cache_dir, directory_error);
    if (directory_error) {
        error = "cannot create runtime cache directory: " + directory_error.message();
        return false;
    }

    std::string executable;
    std::vector<std::string> install_arguments;

#if defined(_WIN32)
    const fs::path install_script = component_dir / Slic3r::SlicerLinuxRuntime::windows_wsl_import_script_file_name();
    const auto powershell = bp::search_path("powershell.exe");
    if (powershell.empty()) {
        error = "powershell.exe not found";
        return false;
    }
    if (!fs::is_regular_file(install_script)) {
        error = "Windows WSL runtime install script missing";
        return false;
    }
    executable = powershell.string();
    install_arguments = {
        "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", install_script.string(),
        "-PackageDir", component_dir.string(),
        "-ComponentDir", component_dir.string(),
        "-ComponentCacheDir", component_cache_dir.string(),
        "-SkipCopyToComponentDir"
    };
#elif defined(__APPLE__)
    const fs::path install_script = component_dir / Slic3r::SlicerLinuxRuntime::mac_runtime_install_script_file_name();
    if (!fs::is_regular_file(install_script)) {
        error = "macOS Lima runtime install script missing";
        return false;
    }
    executable = "/bin/bash";
    install_arguments = {
        install_script.string(),
        "-PackageDir", component_dir.string(),
        "-ComponentDir", component_dir.string(),
        "-ComponentCacheDir", component_cache_dir.string()
    };
#else
    return true;
#endif

    RuntimeCommandResult install = run_runtime_command(executable, install_arguments, cancel_requested);
    log_runtime_result("[network_plugin_runtime_install]", install);
    if (install.canceled) {
        error = "runtime installation canceled";
        return false;
    }
    if (install.exit_code != 0) {
        error = install.output.empty() ? "runtime installation failed" : install.output;
        return false;
    }
    return true;
}

void commit_network_plugin_configuration(AppConfig* app_config, const std::string& version)
{
    app_config->set_network_plugin_version(version);
    app_config->set_bool("installed_networking", true);
    app_config->add_cloud_provider(BBL_CLOUD_PROVIDER);
    wxGetApp().CallAfter([app_config] { app_config->save(); });
}

void clear_failed_first_install(AppConfig* app_config, bool was_installed, const std::string& previous_version)
{
    if (was_installed)
        return;
    app_config->set_bool("installed_networking", false);
    app_config->set_network_plugin_version(previous_version);
    wxGetApp().CallAfter([app_config] { app_config->save(); });
}

} // namespace

UpgradeNetworkJob::UpgradeNetworkJob()
{
    name         = "plugins";
    package_name = "networking_plugins.zip";
}

void UpgradeNetworkJob::on_success(std::function<void()> success)
{
    m_success_fun = success;
}

void UpgradeNetworkJob::update_status(Ctl &ctl, int st, const std::string &msg)
{
    BOOST_LOG_TRIVIAL(info) << "UpgradeNetworkJob: percent = " << st << "msg = " << msg;
    ctl.update_status(st, msg);
    wxCommandEvent event(EVT_UPGRADE_UPDATE_MESSAGE);
    event.SetString(msg);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
}

void UpgradeNetworkJob::process(Ctl &ctl)
{
    int result = 0;

    AppConfig* app_config = wxGetApp().app_config;
    if (!app_config)
        return;

    const bool was_installed = app_config->get_bool("installed_networking");
    const std::string previous_version = app_config->get_network_plugin_version();

    BOOST_LOG_TRIVIAL(info) << "[UpgradeNetworkJob process]: enter";

    auto cancel_fn = [&ctl]() { return ctl.was_canceled(); };
    std::string downloaded_version;
    result = wxGetApp().download_plugin(name, package_name,
        [this, &ctl](int state, int percent, bool&) {
            if (state == InstallStatusDownloadFailed)
                update_status(ctl, 0, _u8L("Download failed"));
            else
                update_status(ctl, std::min(percent, 50), _u8L("Downloading"));
        }, cancel_fn, &downloaded_version);

    if (ctl.was_canceled()) {
        clear_failed_first_install(app_config, was_installed, previous_version);
        update_status(ctl, 0, _u8L("Canceled"));
        wxCloseEvent event(wxEVT_CLOSE_WINDOW);
        event.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, event);
        return;
    }

    if (result < 0) {
        clear_failed_first_install(app_config, was_installed, previous_version);
        update_status(ctl, 0, _u8L("Download failed"));
        wxCommandEvent event(EVT_DOWNLOAD_NETWORK_FAILED);
        event.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, event);
        return;
    }

    result = wxGetApp().install_plugin(
        name, package_name,
        [this, &ctl](int, int percent, bool&) {
            const int mapped_percent = 50 + std::min(percent, 100) * 30 / 100;
            update_status(ctl, mapped_percent, _u8L("Installing"));
        }, cancel_fn, downloaded_version);

    if (ctl.was_canceled()) {
        std::string rollback_error;
        if (!wxGetApp().rollback_network_plugin_payload(&rollback_error) && !rollback_error.empty())
            BOOST_LOG_TRIVIAL(error) << "[UpgradeNetworkJob process]: rollback after cancellation failed: " << rollback_error;
        clear_failed_first_install(app_config, was_installed, previous_version);
        update_status(ctl, 0, _u8L("Canceled"));
        wxCloseEvent event(wxEVT_CLOSE_WINDOW);
        event.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, event);
        return;
    }

    if (result != 0) {
        clear_failed_first_install(app_config, was_installed, previous_version);
        update_status(ctl, 0, _u8L("Install failed"));
        wxCommandEvent event(EVT_INSTALL_NETWORK_FAILED);
        event.SetEventObject(m_event_handle);
        wxPostEvent(m_event_handle, event);
        return;
    }

    update_status(ctl, 85, _u8L("Preparing Linux runtime"));
    std::string runtime_error;
    if (!install_requested_network_runtime(cancel_fn, runtime_error)) {
        std::string rollback_error;
        if (!wxGetApp().rollback_network_plugin_payload(&rollback_error) && !rollback_error.empty()) {
            if (!runtime_error.empty())
                runtime_error += "\n";
            runtime_error += "plug-in rollback failed: " + rollback_error;
        }
        clear_failed_first_install(app_config, was_installed, previous_version);
        BOOST_LOG_TRIVIAL(error) << "[UpgradeNetworkJob process]: runtime installation failed: " << runtime_error;
        update_status(ctl, 0, ctl.was_canceled() ? _u8L("Canceled") : _u8L("Install failed"));
        if (ctl.was_canceled()) {
            wxCloseEvent event(wxEVT_CLOSE_WINDOW);
            event.SetEventObject(m_event_handle);
            wxPostEvent(m_event_handle, event);
        } else {
            wxCommandEvent event(EVT_INSTALL_NETWORK_FAILED);
            event.SetString(runtime_error);
            event.SetEventObject(m_event_handle);
            wxPostEvent(m_event_handle, event);
        }
        return;
    }

    update_status(ctl, 95, _u8L("Activating network plug-in"));
    commit_network_plugin_configuration(app_config, downloaded_version);
    update_status(ctl, 100, _u8L("Installed successfully"));

    wxCommandEvent event(EVT_UPGRADE_NETWORK_SUCCESS);
    event.SetEventObject(m_event_handle);
    wxPostEvent(m_event_handle, event);
    BOOST_LOG_TRIVIAL(info) << "[UpgradeNetworkJob process]: exit";
}

void UpgradeNetworkJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    try {
        if (eptr)
            std::rethrow_exception(eptr);
        eptr = nullptr;
    } catch (...) {
        eptr = std::current_exception();
    }

    if (canceled || eptr)
        return;
}

void UpgradeNetworkJob::set_event_handle(wxWindow *hanle)
{
    m_event_handle = hanle;
}

}} // namespace Slic3r::GUI
