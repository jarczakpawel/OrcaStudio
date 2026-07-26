// SPDX-License-Identifier: AGPL-3.0-only

#include <gtk/gtk.h>
#include <jsc/jsc.h>
#include <nlohmann/json.hpp>
#include <webkit2/webkit2.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <optional>
#include <string>
#include <mutex>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <thread>
#include <vector>
#include <cstring>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Options {
    std::string mode{"auth"};
    std::string login_url;
    fs::path login_cmd_file;
    fs::path result_file;
    fs::path profile_dir;
    fs::path command_file;
    fs::path event_file;
    fs::path ready_file;
    std::string client_version{"0.0.0.0"};
    std::string language{"en"};
    std::string theme{"light"};
    int loopback_port{0};
    bool probe{false};
    bool self_test{false};
};

struct App {
    Options opts;
    GtkWidget* window{nullptr};
    WebKitWebView* web_view{nullptr};
    std::string login_cmd;
    std::string autotest_token;
    std::atomic<bool> completed{false};
    std::atomic<bool> callback_pending{false};
    std::atomic<bool> ready{false};
    std::atomic<bool> load_finished{false};
    int configured_loopback_port{0};
    std::vector<int> callback_sockets;
    std::thread callback_thread;
    fs::path auth_reply_file;
    fs::path callback_complete_file;
    std::string callback_redirect_url;
    std::mutex external_browser_mutex;
    GPid external_browser_pid{0};
    std::uint64_t event_sequence{0};
    bool quit_main_loop_on_finish{true};
};

std::string read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read " + path.string());
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_atomic_private(const fs::path& path, const std::string& value)
{
    fs::create_directories(path.parent_path());
    const fs::path tmp = path.string() + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("cannot write " + tmp.string());
        out.write(value.data(), static_cast<std::streamsize>(value.size()));
        out.flush();
        if (!out)
            throw std::runtime_error("short write " + tmp.string());
    }
    ::chmod(tmp.c_str(), S_IRUSR | S_IWUSR);
    fs::rename(tmp, path);
}


void append_event(App* app, const json& event)
{
    if (!app || app->opts.event_file.empty())
        return;
    json out = event;
    out["sequence"] = ++app->event_sequence;
    out["schema"] = 1;
    out["platform"] = "linux";
    std::ofstream stream(app->opts.event_file, std::ios::binary | std::ios::app);
    if (!stream)
        throw std::runtime_error("cannot append " + app->opts.event_file.string());
    stream << out.dump() << '\n';
    stream.flush();
    if (!stream)
        throw std::runtime_error("short append " + app->opts.event_file.string());
    ::chmod(app->opts.event_file.c_str(), S_IRUSR | S_IWUSR);
}

bool auth_mode(const App* app)
{
    return app && app->opts.mode == "auth";
}

std::string linux_machine()
{
    utsname info{};
    if (::uname(&info) != 0 || info.machine[0] == '\0')
        return "unknown";
    const std::string machine = info.machine;
    if (machine == "arm64")
        return "aarch64";
    if (machine == "amd64")
        return "x86_64";
    return machine;
}

std::string browser_metadata_token(const std::string& value,
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

std::string build_bbl_user_agent(const Options& opts, const std::string& default_user_agent = {})
{
    const std::string version = browser_metadata_token(opts.client_version, "0.0.0.0", 32, true);
    const std::string language = browser_metadata_token(opts.language, "en", 16, false);
    const std::string theme = opts.theme == "dark" ? "dark" : "light";
    const std::string base = default_user_agent.find("Linux") != std::string::npos
        ? default_user_agent
        : "Mozilla/5.0 (X11; Linux " + linux_machine() + ") AppleWebKit/537.36 (KHTML, like Gecko)";
    return base + " BBL-Slicer/v" + version + " (" + theme + ") BBL-Language/" + language;
}

std::string url_decode(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '+') {
            out.push_back(' ');
        } else if (value[i] == '%' && i + 2 < value.size()) {
            const auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(value[i + 1]);
            const int lo = hex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(value[i]);
            }
        } else {
            out.push_back(value[i]);
        }
    }
    return out;
}

std::string param_from_section(const std::string& section, const std::string& key)
{
    std::size_t pos = 0;
    while (pos <= section.size()) {
        const auto end = section.find('&', pos);
        const std::string part = section.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        const auto eq = part.find('=');
        if (url_decode(part.substr(0, eq)) == key)
            return eq == std::string::npos ? std::string() : url_decode(part.substr(eq + 1));
        if (end == std::string::npos)
            break;
        pos = end + 1;
    }
    return {};
}

std::string url_param(const std::string& uri, const std::string& key)
{
    for (const char separator : {'?', '#'}) {
        const auto start = uri.find(separator);
        if (start == std::string::npos)
            continue;
        const auto section_end = uri.find(separator == '?' ? '#' : '?', start + 1);
        const std::string section = uri.substr(start + 1,
            section_end == std::string::npos ? std::string::npos : section_end - start - 1);
        const std::string value = param_from_section(section, key);
        if (!value.empty())
            return value;
    }
    return {};
}

bool is_expected_loopback_uri(const std::string& uri, int port)
{
    const std::string port_text = port > 0 ? ":" + std::to_string(port) : std::string();
    const std::string prefixes[] = {
        "http://localhost" + port_text,
        "http://127.0.0.1" + port_text,
        "http://[::1]" + port_text,
        "https://localhost" + port_text,
        "https://127.0.0.1" + port_text,
        "https://[::1]" + port_text
    };
    for (const auto& prefix : prefixes) {
        if (uri.rfind(prefix, 0) != 0)
            continue;
        if (uri.size() == prefix.size())
            return true;
        const char next = uri[prefix.size()];
        if (next == '/' || next == '?' || next == '#')
            return true;
    }
    return false;
}


void finish(App* app, json result);
bool capture_callback(App* app, const std::string& uri);

void replace_all(std::string& value, const std::string& from, const std::string& to)
{
    if (from.empty() || from == to)
        return;
    std::size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::string::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string rewrite_loopback_port(std::string value, int old_port, int new_port)
{
    if (old_port <= 0 || new_port <= 0 || old_port == new_port)
        return value;
    const std::string old_text = std::to_string(old_port);
    const std::string new_text = std::to_string(new_port);
    for (const std::string& host : {"localhost", "127.0.0.1", "[::1]"}) {
        replace_all(value, "http://" + host + ":" + old_text, "http://" + host + ":" + new_text);
        replace_all(value, "https://" + host + ":" + old_text, "https://" + host + ":" + new_text);
    }
    for (const std::string& host : {"localhost", "127.0.0.1", "%5B%3A%3A1%5D", "%5b%3a%3a1%5d"}) {
        replace_all(value, "http%3A%2F%2F" + host + "%3A" + old_text, "http%3A%2F%2F" + host + "%3A" + new_text);
        replace_all(value, "https%3A%2F%2F" + host + "%3A" + old_text, "https%3A%2F%2F" + host + "%3A" + new_text);
        replace_all(value, "http%3a%2f%2f" + host + "%3a" + old_text, "http%3a%2f%2f" + host + "%3a" + new_text);
        replace_all(value, "https%3a%2f%2f" + host + "%3a" + old_text, "https%3a%2f%2f" + host + "%3a" + new_text);
    }
    return value;
}

void rewrite_json_strings(json& value, int old_port, int new_port)
{
    if (value.is_string()) {
        value = rewrite_loopback_port(value.get<std::string>(), old_port, new_port);
    } else if (value.is_array()) {
        for (auto& item : value)
            rewrite_json_strings(item, old_port, new_port);
    } else if (value.is_object()) {
        for (auto& item : value.items())
            rewrite_json_strings(item.value(), old_port, new_port);
    }
}

int bind_loopback_v4(int requested_port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(requested_port));
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

int socket_port(int fd)
{
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0)
        return 0;
    return ntohs(addr.sin_port);
}

int bind_loopback_v6(int port)
{
    const int fd = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = htons(static_cast<uint16_t>(port));
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 8) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

std::string http_target_from_request(const std::string& request)
{
    const auto line_end = request.find("\r\n");
    const std::string first = request.substr(0, line_end);
    const auto first_space = first.find(' ');
    if (first_space == std::string::npos)
        return {};
    const auto second_space = first.find(' ', first_space + 1);
    if (second_space == std::string::npos)
        return {};
    if (first.substr(0, first_space) != "GET")
        return {};
    return first.substr(first_space + 1, second_space - first_space - 1);
}

bool valid_redirect_url(const std::string& url)
{
    return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

std::string redirect_with_result(const std::string& redirect_url, bool success)
{
    if (!valid_redirect_url(redirect_url))
        return {};
    return redirect_url + "?result=" + (success ? "success" : "fail");
}

void send_http_response(int client, const std::string& response)
{
    std::size_t off = 0;
    while (off < response.size()) {
        const ssize_t n = ::send(client, response.data() + off, response.size() - off, MSG_NOSIGNAL);
        if (n <= 0)
            break;
        off += static_cast<std::size_t>(n);
    }
}

void send_callback_not_found(int client)
{
    const std::string body = "<!doctype html><meta charset=utf-8><title>Invalid callback</title><h2>Invalid authentication callback</h2>";
    const std::string response =
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Security-Policy: default-src 'none'\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    send_http_response(client, response);
}

void send_callback_redirect(int client, const std::string& redirect_url, bool success)
{
    const std::string destination = redirect_with_result(redirect_url, success);
    if (destination.empty()) {
        send_callback_not_found(client);
        return;
    }
    const std::string body = "<!doctype html><meta charset=utf-8><title>Authentication complete</title>";
    const std::string response =
        "HTTP/1.1 302 Found\r\n"
        "Location: " + destination + "\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Security-Policy: default-src 'none'\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\n\r\n" + body;
    send_http_response(client, response);
}

bool publish_external_ticket(App* app, const std::string& uri)
{
    if (!app || app->completed.load())
        return false;
    const std::string ticket = url_param(uri, "ticket");
    const std::string redirect_url = url_param(uri, "redirect_url");
    if (ticket.empty() || !valid_redirect_url(redirect_url))
        return false;
    bool expected = false;
    if (!app->callback_pending.compare_exchange_strong(expected, true))
        return false;

    app->callback_redirect_url = redirect_url;
    const json result = {
        {"kind", "ticket"}, {"ticket", ticket}, {"redirect_url", redirect_url},
        {"external_callback", true}, {"autotest_token", app->autotest_token},
        {"schema", 1}, {"browser", "webkitgtk"}, {"platform", "linux"}
    };
    try {
        fs::remove(app->auth_reply_file);
        fs::remove(app->callback_complete_file);
        write_atomic_private(app->opts.result_file, result.dump());
        return true;
    } catch (const std::exception& e) {
        app->callback_pending.store(false);
        std::cerr << "auth callback result write failed: " << e.what() << '\n';
        return false;
    }
}

std::optional<bool> wait_for_auth_reply(App* app)
{
    if (!app)
        return std::nullopt;
    for (int i = 0; i < 1200 && !app->completed.load(); ++i) {
        if (fs::exists(app->auth_reply_file)) {
            try {
                const json reply = json::parse(read_file(app->auth_reply_file));
                return reply.value("success", false);
            } catch (const std::exception& e) {
                std::cerr << "invalid Linux auth reply: " << e.what() << '\n';
                return false;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return std::nullopt;
}

void complete_external_callback(App* app)
{
    if (!app)
        return;
    try {
        write_atomic_private(app->callback_complete_file, "complete\n");
    } catch (const std::exception& e) {
        std::cerr << "callback completion marker failed: " << e.what() << '\n';
    }
    app->completed.store(true);
    if (app->quit_main_loop_on_finish) {
        g_idle_add([](gpointer) -> gboolean {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }, nullptr);
    }
}

void callback_server_loop(App* app)
{
    while (app && !app->completed.load()) {
        std::vector<pollfd> fds;
        fds.reserve(app->callback_sockets.size());
        for (int fd : app->callback_sockets)
            fds.push_back({fd, POLLIN, 0});
        const int rc = ::poll(fds.data(), fds.size(), 200);
        if (rc <= 0)
            continue;
        for (const auto& item : fds) {
            if (!(item.revents & POLLIN))
                continue;
            const int client = ::accept4(item.fd, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0)
                continue;
            const timeval socket_timeout{125, 0};
            (void) ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout));
            (void) ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout, sizeof(socket_timeout));
            std::string request;
            char buffer[4096];
            while (request.size() < 32768) {
                const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
                if (n <= 0)
                    break;
                request.append(buffer, static_cast<std::size_t>(n));
                if (request.find("\r\n\r\n") != std::string::npos)
                    break;
            }
            const std::string target = http_target_from_request(request);
            std::string uri;
            if (target.rfind("http://", 0) == 0 || target.rfind("https://", 0) == 0)
                uri = target;
            else if (!target.empty())
                uri = "http://localhost:" + std::to_string(app->opts.loopback_port) + target;

            const bool accepted = !uri.empty() &&
                is_expected_loopback_uri(uri, app->opts.loopback_port) &&
                publish_external_ticket(app, uri);
            if (!accepted) {
                send_callback_not_found(client);
            } else {
                const auto success = wait_for_auth_reply(app);
                send_callback_redirect(client, app->callback_redirect_url, success.value_or(false));
                complete_external_callback(app);
            }
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        }
    }
}

void start_callback_server(App* app)
{
    if (!app || !auth_mode(app))
        return;
    int fd4 = bind_loopback_v4(app->opts.loopback_port);
    if (fd4 < 0 && app->opts.loopback_port > 0)
        fd4 = bind_loopback_v4(0);
    if (fd4 < 0)
        throw std::runtime_error("cannot bind Linux authentication callback server");
    const int actual_port = socket_port(fd4);
    if (actual_port <= 0) {
        ::close(fd4);
        throw std::runtime_error("cannot determine Linux authentication callback port");
    }
    app->callback_sockets.push_back(fd4);
    app->auth_reply_file = app->opts.result_file.parent_path() / "auth-reply.json";
    app->callback_complete_file = app->opts.result_file.parent_path() / "callback-complete";
    fs::remove(app->auth_reply_file);
    fs::remove(app->callback_complete_file);
    const int fd6 = bind_loopback_v6(actual_port);
    if (fd6 >= 0)
        app->callback_sockets.push_back(fd6);

    const int old_port = app->opts.loopback_port > 0 ? app->opts.loopback_port : 13618;
    app->configured_loopback_port = old_port;
    app->opts.loopback_port = actual_port;
    if (!app->login_cmd.empty()) {
        json login = json::parse(app->login_cmd);
        rewrite_json_strings(login, old_port, actual_port);
        if (login.contains("pkce") && login["pkce"].is_object())
            login["pkce"]["loopback_port"] = actual_port;
        app->login_cmd = login.dump();
    }
    app->callback_thread = std::thread(callback_server_loop, app);
}

void stop_callback_server(App* app)
{
    if (!app)
        return;
    for (int fd : app->callback_sockets) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (app->callback_thread.joinable())
        app->callback_thread.join();
    app->callback_sockets.clear();
}


bool run_callback_self_test(const fs::path& root)
{
    App callback_app;
    callback_app.opts.mode = "auth";
    callback_app.opts.loopback_port = 13618;
    callback_app.opts.result_file = root / "callback-result.json";
    callback_app.login_cmd = R"({"pkce":{"loopback_port":13618,"redirect_uri":"http://localhost:13618/callback"}})";
    callback_app.quit_main_loop_on_finish = false;

    start_callback_server(&callback_app);
    bool client_ok = false;
    std::string response;
    std::thread client([&] {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0)
            return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(static_cast<uint16_t>(callback_app.opts.loopback_port));
        bool ok = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0;
        if (ok) {
            const std::string request =
                "GET /callback?ticket=callback-self-test&redirect_url=https%3A%2F%2Fexample.invalid%2Fauth-complete HTTP/1.1\r\n"
                "Host: localhost\r\nConnection: close\r\n\r\n";
            std::size_t sent = 0;
            while (sent < request.size()) {
                const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
                if (n <= 0) {
                    ok = false;
                    break;
                }
                sent += static_cast<std::size_t>(n);
            }
            char buffer[1024];
            while (ok) {
                const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
                if (n <= 0)
                    break;
                response.append(buffer, static_cast<std::size_t>(n));
            }
        }
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
        client_ok = ok;
    });

    bool result_ready = false;
    for (int i = 0; i < 500; ++i) {
        if (fs::exists(callback_app.opts.result_file)) {
            result_ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (result_ready)
        write_atomic_private(callback_app.auth_reply_file, json({{"success", true}}).dump());

    client.join();
    callback_app.completed.store(true);
    stop_callback_server(&callback_app);

    if (!client_ok || !result_ready ||
        response.find("HTTP/1.1 302 Found") == std::string::npos ||
        response.find("Location: https://example.invalid/auth-complete?result=success") == std::string::npos ||
        !fs::exists(callback_app.callback_complete_file))
        return false;
    const json result = json::parse(read_file(callback_app.opts.result_file));
    return result.value("kind", std::string()) == "ticket" &&
           result.value("ticket", std::string()) == "callback-self-test" &&
           result.value("external_callback", false) &&
           result.value("platform", std::string()) == "linux";
}

bool launch_external_linux_browser(App* app, const std::string& url)
{
    if (!app || url.empty())
        return false;
    const char* configured = std::getenv("SLICER_LINUX_RUNTIME_EXTERNAL_BROWSER");
    std::string executable = configured ? configured : "";
    if (executable.empty()) {
        for (const char* candidate : {"epiphany", "epiphany-browser"}) {
            gchar* found = g_find_program_in_path(candidate);
            if (found) {
                executable = found;
                g_free(found);
                break;
            }
        }
    }
    if (executable.empty())
        return false;

    gchar* argv[] = {
        const_cast<gchar*>(executable.c_str()),
        const_cast<gchar*>("--new-window"),
        const_cast<gchar*>(url.c_str()),
        nullptr
    };
    GPid pid = 0;
    GError* error = nullptr;
    const gboolean ok = g_spawn_async(nullptr, argv, nullptr,
        static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD),
        nullptr, nullptr, &pid, &error);
    if (!ok) {
        if (error) {
            std::cerr << "external Linux browser failed: " << error->message << '\n';
            g_error_free(error);
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(app->external_browser_mutex);
        app->external_browser_pid = pid;
    }
    return true;
}

void stop_external_linux_browser(App* app)
{
    if (!app)
        return;
    GPid pid = 0;
    {
        std::lock_guard<std::mutex> lock(app->external_browser_mutex);
        pid = app->external_browser_pid;
        app->external_browser_pid = 0;
    }
    if (pid <= 0)
        return;
    (void) ::kill(pid, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int status = 0;
        const pid_t ret = ::waitpid(pid, &status, WNOHANG);
        if (ret == pid) {
            g_spawn_close_pid(pid);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    (void) ::kill(pid, SIGKILL);
    int status = 0;
    (void) ::waitpid(pid, &status, 0);
    g_spawn_close_pid(pid);
}

void finish(App* app, json result)
{
    if (!app || app->completed.exchange(true))
        return;
    result["schema"] = 1;
    result["browser"] = "webkitgtk";
    result["platform"] = "linux";
    try {
        write_atomic_private(app->opts.result_file, result.dump());
    } catch (const std::exception& e) {
        std::cerr << "auth result write failed: " << e.what() << '\n';
    }
    if (app->quit_main_loop_on_finish) {
        g_idle_add([](gpointer) -> gboolean {
            gtk_main_quit();
            return G_SOURCE_REMOVE;
        }, nullptr);
    }
}

bool capture_callback(App* app, const std::string& uri)
{
    if (!auth_mode(app) || !is_expected_loopback_uri(uri, app->opts.loopback_port))
        return false;
    const std::string ticket = url_param(uri, "ticket");
    if (ticket.empty())
        return false;
    finish(app, {{"kind", "ticket"}, {"ticket", ticket}, {"autotest_token", app->autotest_token}});
    return true;
}

void post_json(WebKitWebView* view, const json& value)
{
    const std::string script = "window.postMessage(" + value.dump() + ", '*');";
#if WEBKIT_CHECK_VERSION(2, 40, 0)
    webkit_web_view_evaluate_javascript(view, script.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
#else
    webkit_web_view_run_javascript(view, script.c_str(), nullptr, nullptr, nullptr);
#endif
}

std::string javascript_result_string(WebKitJavascriptResult* result)
{
    JSCValue* value = webkit_javascript_result_get_js_value(result);
    if (!value)
        return {};
    if (jsc_value_is_string(value)) {
        gchar* text = jsc_value_to_string(value);
        std::string out = text ? text : "";
        g_free(text);
        return out;
    }
    gchar* text = jsc_value_to_json(value, 0);
    std::string out = text ? text : "";
    g_free(text);
    return out;
}

std::string message_url(const json& message)
{
    if (!message.contains("data"))
        return {};
    if (message["data"].is_string())
        return message["data"].get<std::string>();
    if (message["data"].is_object())
        return message["data"].value("url", std::string());
    return {};
}

std::string sequence_id(const json& message)
{
    if (!message.contains("sequence_id"))
        return {};
    if (message["sequence_id"].is_string())
        return message["sequence_id"].get<std::string>();
    return message["sequence_id"].dump();
}

void on_script_message(WebKitUserContentManager*, WebKitJavascriptResult* result, gpointer user_data)
{
    auto* app = static_cast<App*>(user_data);
    try {
        const std::string raw = javascript_result_string(result);
        if (raw.empty())
            return;
        const json message = json::parse(raw);
        const std::string command = message.value("command", std::string());

        if (!auth_mode(app)) {
            append_event(app, {{"kind", "script_message"}, {"message", message}});
            return;
        }

        if (command == "get_login_cmd") {
            if (app->login_cmd.empty())
                finish(app, {{"kind", "error"}, {"error", "Bambu login page requested an unavailable pre-login command"}});
            else
                post_json(app->web_view, json::parse(app->login_cmd));
            return;
        }
        if (command == "autotest_token") {
            if (message.contains("data") && message["data"].is_object())
                app->autotest_token = message["data"].value("token", std::string());
            return;
        }
        if (command == "get_localhost_url") {
            const json ack = {
                {"command", "get_localhost_url"},
                {"response", {{"base_url", "http://localhost:" + std::to_string(app->opts.loopback_port)}, {"result", "success"}}},
                {"sequence_id", sequence_id(message)}
            };
            post_json(app->web_view, ack);
            return;
        }
        if (command == "thirdparty_login" || command == "new_webpage") {
            std::string url = message_url(message);
            url = rewrite_loopback_port(url, app->configured_loopback_port, app->opts.loopback_port);
            if (!url.empty() && !launch_external_linux_browser(app, url))
                webkit_web_view_load_uri(app->web_view, url.c_str());
            return;
        }
        if (command == "user_ticket_login") {
            const std::string ticket = message.contains("data") && message["data"].is_object()
                ? message["data"].value("ticket", std::string()) : std::string();
            if (!ticket.empty()) {
                json out = {{"kind", "ticket"}, {"ticket", ticket}, {"autotest_token", app->autotest_token}};
                if (message["data"].contains("identity"))
                    out["browser_identity"] = message["data"]["identity"];
                finish(app, std::move(out));
            }
            return;
        }
        if (command == "user_login") {
            json normalized = message;
            if (!normalized.contains("data") || !normalized["data"].is_object())
                normalized["data"] = json::object();
            normalized["data"]["autotest_token"] = app->autotest_token;
            finish(app, {{"kind", "user_login"}, {"message", normalized}});
        }
    } catch (const std::exception& e) {
        std::cerr << "auth script message ignored: " << e.what() << '\n';
    }
}

gboolean poll_command_file(gpointer user_data)
{
    auto* app = static_cast<App*>(user_data);
    if (!app || app->completed.load() || app->opts.command_file.empty())
        return G_SOURCE_CONTINUE;
    if (!fs::exists(app->opts.command_file))
        return G_SOURCE_CONTINUE;

    try {
        const std::string raw = read_file(app->opts.command_file);
        fs::remove(app->opts.command_file);
        if (raw.empty())
            return G_SOURCE_CONTINUE;
        const json request = json::parse(raw);
        const std::string command = request.value("command", std::string());
        if (command == "back") {
            if (webkit_web_view_can_go_back(app->web_view))
                webkit_web_view_go_back(app->web_view);
        } else if (command == "forward") {
            if (webkit_web_view_can_go_forward(app->web_view))
                webkit_web_view_go_forward(app->web_view);
        } else if (command == "reload") {
            webkit_web_view_reload(app->web_view);
        } else if (command == "load_url") {
            const std::string url = request.value("url", std::string());
            if (!url.empty())
                webkit_web_view_load_uri(app->web_view, url.c_str());
        } else if (command == "post_message") {
            if (request.contains("message"))
                post_json(app->web_view, request["message"]);
        } else if (command == "close") {
            finish(app, {{"kind", "closed"}});
        } else {
            append_event(app, {{"kind", "command_error"}, {"error", "unsupported command"}, {"command", command}});
        }
    } catch (const std::exception& e) {
        try { append_event(app, {{"kind", "command_error"}, {"error", e.what()}}); } catch (...) {}
    }
    return G_SOURCE_CONTINUE;
}

bool publish_ready(App* app)
{
    if (!app || app->ready.exchange(true))
        return true;
    try {
        if (!app->opts.ready_file.empty())
            write_atomic_private(app->opts.ready_file, "ready\n");
        append_event(app, {
            {"kind", "render_ready"},
            {"width", gtk_widget_get_allocated_width(GTK_WIDGET(app->web_view))},
            {"height", gtk_widget_get_allocated_height(GTK_WIDGET(app->web_view))}
        });
        return true;
    } catch (const std::exception& e) {
        app->ready.store(false);
        if (auth_mode(app))
            finish(app, {{"kind", "error"}, {"error", std::string("failed to publish browser readiness: ") + e.what()}});
        else
            std::cerr << "browser readiness failed: " << e.what() << '\n';
        return false;
    }
}

gboolean on_draw(GtkWidget* widget, cairo_t*, gpointer user_data)
{
    auto* app = static_cast<App*>(user_data);
    if (!app || !app->load_finished.load() || app->completed.load())
        return FALSE;
    if (!gtk_widget_get_mapped(widget) ||
        gtk_widget_get_allocated_width(widget) <= 0 ||
        gtk_widget_get_allocated_height(widget) <= 0)
        return FALSE;
    publish_ready(app);
    return FALSE;
}

void on_load_changed(WebKitWebView* view, WebKitLoadEvent event, gpointer user_data)
{
    auto* app = static_cast<App*>(user_data);
    if (event != WEBKIT_LOAD_FINISHED)
        return;

    const gchar* uri = webkit_web_view_get_uri(view);
    const std::string current_uri = uri ? uri : "";
    if (current_uri.empty() || current_uri == "about:blank") {
        if (auth_mode(app))
            finish(app, {{"kind", "error"}, {"error", "authentication page finished on an empty URL"}});
        return;
    }

    app->load_finished.store(true);
    gtk_widget_queue_draw(GTK_WIDGET(view));
    if (auto* display = gdk_display_get_default())
        gdk_display_flush(display);

    if (auth_mode(app))
        return;
    const gchar* title = webkit_web_view_get_title(view);
    try {
        append_event(app, {
            {"kind", "navigation"},
            {"url", current_uri},
            {"title", std::string(title ? title : "")},
            {"can_go_back", static_cast<bool>(webkit_web_view_can_go_back(view))},
            {"can_go_forward", static_cast<bool>(webkit_web_view_can_go_forward(view))}
        });
    } catch (const std::exception& e) {
        std::cerr << "browser navigation event failed: " << e.what() << '\n';
    }
}

gboolean on_decide_policy(WebKitWebView* view, WebKitPolicyDecision* decision,
                          WebKitPolicyDecisionType type, gpointer user_data)
{
    auto* app = static_cast<App*>(user_data);
    WebKitURIRequest* request = nullptr;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION ||
        type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        auto* nav = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        request = webkit_navigation_action_get_request(webkit_navigation_policy_decision_get_navigation_action(nav));
    }
    if (!request)
        return FALSE;

    const gchar* raw_uri = webkit_uri_request_get_uri(request);
    const std::string uri = raw_uri ? raw_uri : "";
    if (capture_callback(app, uri)) {
        webkit_policy_decision_ignore(decision);
        return TRUE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        webkit_policy_decision_ignore(decision);
        webkit_web_view_load_uri(view, uri.c_str());
        return TRUE;
    }
    return FALSE;
}

WebKitWebView* on_create(WebKitWebView* view, WebKitNavigationAction* action, gpointer user_data)
{
    auto* app = static_cast<App*>(user_data);
    WebKitURIRequest* request = webkit_navigation_action_get_request(action);
    const gchar* raw_uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    const std::string uri = raw_uri ? raw_uri : "";
    if (!uri.empty()) {
        if (!capture_callback(app, uri))
            webkit_web_view_load_uri(view, uri.c_str());
    }
    return nullptr;
}

gboolean on_load_failed(WebKitWebView*, WebKitLoadEvent, const gchar* failing_uri, GError* error, gpointer user_data)
{
    auto* app = static_cast<App*>(user_data);
    const std::string uri = failing_uri ? failing_uri : "";
    if (capture_callback(app, uri))
        return TRUE;
    const std::string message = error && error->message ? error->message : "WebKit load failed";
    std::cerr << "webkit load failed: " << message << '\n';
    if (auth_mode(app) && !app->ready.load()) {
        finish(app, {{"kind", "error"}, {"error", message}});
        return TRUE;
    }
    return FALSE;
}

int parse_loopback_port(const json& login_cmd)
{
    if (!login_cmd.contains("pkce") || !login_cmd["pkce"].is_object())
        return 0;
    const auto& pkce = login_cmd["pkce"];
    if (pkce.contains("loopback_port")) {
        try {
            if (pkce["loopback_port"].is_number_integer())
                return pkce["loopback_port"].get<int>();
            if (pkce["loopback_port"].is_string())
                return std::stoi(pkce["loopback_port"].get<std::string>());
        } catch (...) {
        }
    }
    if (pkce.contains("redirect_uri") && pkce["redirect_uri"].is_string()) {
        const std::string uri = pkce["redirect_uri"].get<std::string>();
        for (const std::string prefix : {"localhost:", "127.0.0.1:", "[::1]:"}) {
            auto start = uri.find(prefix);
            if (start == std::string::npos)
                continue;
            start += prefix.size();
            const auto end = uri.find_first_not_of("0123456789", start);
            try {
                return std::stoi(uri.substr(start, end - start));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

Options parse_args(int argc, char** argv)
{
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string {
            if (++i >= argc)
                throw std::runtime_error("missing value for " + arg);
            return argv[i];
        };
        if (arg == "--mode") opts.mode = next();
        else if (arg == "--login-url") opts.login_url = next();
        else if (arg == "--login-cmd-file") opts.login_cmd_file = next();
        else if (arg == "--result-file") opts.result_file = next();
        else if (arg == "--profile-dir") opts.profile_dir = next();
        else if (arg == "--command-file") opts.command_file = next();
        else if (arg == "--event-file") opts.event_file = next();
        else if (arg == "--ready-file") opts.ready_file = next();
        else if (arg == "--client-version") opts.client_version = next();
        else if (arg == "--language") opts.language = next();
        else if (arg == "--theme") opts.theme = next();
        else if (arg == "--loopback-port") opts.loopback_port = std::stoi(next());
        else if (arg == "--probe") opts.probe = true;
        else if (arg == "--self-test") opts.self_test = true;
        else throw std::runtime_error("unknown argument: " + arg);
    }
    return opts;
}

} // namespace

int main(int argc, char** argv)
{
    fs::path self_test_root;
    try {
        App app;
        app.opts = parse_args(argc, argv);
        if (app.opts.probe) {
            std::cout << json({
                {"ok", true},
                {"platform", "linux"},
                {"engine", "webkitgtk"},
                {"webkit", std::to_string(webkit_get_major_version()) + "." + std::to_string(webkit_get_minor_version()) + "." + std::to_string(webkit_get_micro_version())},
                {"user_agent", build_bbl_user_agent(app.opts)}
            }).dump() << '\n';
            return 0;
        }

        if (app.opts.self_test) {
            self_test_root = fs::temp_directory_path() / ("slicer-linux-auth-self-test-" + std::to_string(::getpid()));
            fs::remove_all(self_test_root);
            fs::create_directories(self_test_root);
            if (app.opts.result_file.empty()) app.opts.result_file = self_test_root / "result.json";
            if (app.opts.profile_dir.empty()) app.opts.profile_dir = self_test_root / "profile";
            app.opts.loopback_port = 13618;
            app.login_cmd = R"({"command":"self_test"})";
        } else {
            if (app.opts.mode != "auth" && app.opts.mode != "browse")
                throw std::runtime_error("mode must be auth or browse");
            if (app.opts.login_url.empty() || app.opts.result_file.empty() || app.opts.profile_dir.empty())
                throw std::runtime_error("required Linux browser arguments are missing");
            if (app.opts.mode == "auth") {
                if (!app.opts.login_cmd_file.empty() && app.opts.login_cmd_file != "-") {
                    app.login_cmd = read_file(app.opts.login_cmd_file);
                    const json parsed_login_cmd = json::parse(app.login_cmd);
                    if (app.opts.loopback_port <= 0)
                        app.opts.loopback_port = parse_loopback_port(parsed_login_cmd);
                }
            } else if (app.opts.command_file.empty() || app.opts.event_file.empty()) {
                throw std::runtime_error("browse mode requires command and event files");
            }
        }
        if (app.opts.loopback_port <= 0)
            app.opts.loopback_port = 13618;

        fs::create_directories(app.opts.profile_dir);
        ::chmod(app.opts.profile_dir.c_str(), S_IRWXU);
        setenv("XDG_DATA_HOME", (app.opts.profile_dir / "data").c_str(), 1);
        setenv("XDG_CACHE_HOME", (app.opts.profile_dir / "cache").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (app.opts.profile_dir / "config").c_str(), 1);

        const bool callback_self_test_ok = !app.opts.self_test || run_callback_self_test(self_test_root);
        if (!callback_self_test_ok)
            throw std::runtime_error("Linux callback server self-test failed");

        gtk_init(&argc, &argv);
        auto* content_manager = webkit_user_content_manager_new();
        if (!webkit_user_content_manager_register_script_message_handler(content_manager, "wx"))
            throw std::runtime_error("failed to register WebKit script handler");
        const char* bridge =
            "(() => {"
            "if (!window.wx) Object.defineProperty(window, 'wx', {configurable:false, value:{postMessage:(value)=>{"
            "const payload = typeof value === 'string' ? value : JSON.stringify(value);"
            "window.webkit.messageHandlers.wx.postMessage(payload);"
            "}}});"
            "})();";
        auto* bridge_script = webkit_user_script_new(bridge,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            nullptr, nullptr);
        webkit_user_content_manager_add_script(content_manager, bridge_script);
        webkit_user_script_unref(bridge_script);
        g_signal_connect(content_manager, "script-message-received::wx", G_CALLBACK(on_script_message), &app);

        const fs::path data_dir = app.opts.profile_dir / "webkit-data";
        const fs::path cache_dir = app.opts.profile_dir / "webkit-cache";
        fs::create_directories(data_dir);
        fs::create_directories(cache_dir);
        auto* data_manager = webkit_website_data_manager_new(
            "base-data-directory", data_dir.c_str(),
            "base-cache-directory", cache_dir.c_str(),
            nullptr);
        auto* context = webkit_web_context_new_with_website_data_manager(data_manager);
        auto* cookie_manager = webkit_web_context_get_cookie_manager(context);
        const fs::path cookie_db = app.opts.profile_dir / "cookies.sqlite";
        webkit_cookie_manager_set_persistent_storage(cookie_manager, cookie_db.c_str(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_cookie_manager_set_accept_policy(cookie_manager, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);

        app.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(app.window), auth_mode(&app) ? "OrcaStudio - Bambu Login (Linux)" : "OrcaStudio - Bambu Browser (Linux)");
        gtk_window_set_default_size(GTK_WINDOW(app.window), 656, 840);
        g_signal_connect(app.window, "destroy", G_CALLBACK(+[](GtkWidget*, gpointer data) {
            auto* current = static_cast<App*>(data);
            if (!current->completed.load())
                finish(current, {{"kind", auth_mode(current) ? "cancelled" : "closed"}});
        }), &app);

        app.web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
            "web-context", context,
            "user-content-manager", content_manager,
            nullptr));
        auto* settings = webkit_web_view_get_settings(app.web_view);
        const gchar* default_user_agent = webkit_settings_get_user_agent(settings);
        const std::string user_agent = build_bbl_user_agent(
            app.opts, default_user_agent ? default_user_agent : "");
        webkit_settings_set_user_agent(settings, user_agent.c_str());
        webkit_settings_set_enable_javascript(settings, TRUE);
        webkit_settings_set_enable_webgl(settings, FALSE);

        append_event(&app, {
            {"kind", "browser_identity"},
            {"user_agent", user_agent},
            {"machine", linux_machine()},
            {"language", browser_metadata_token(app.opts.language, "en", 16, false)},
            {"theme", app.opts.theme == "dark" ? "dark" : "light"}
        });

        g_signal_connect(app.web_view, "decide-policy", G_CALLBACK(on_decide_policy), &app);
        g_signal_connect(app.web_view, "create", G_CALLBACK(on_create), &app);
        g_signal_connect(app.web_view, "load-failed", G_CALLBACK(on_load_failed), &app);
        g_signal_connect(app.web_view, "load-changed", G_CALLBACK(on_load_changed), &app);
        g_signal_connect(app.web_view, "draw", G_CALLBACK(on_draw), &app);
        if (!app.opts.command_file.empty())
            g_timeout_add(100, poll_command_file, &app);
        gtk_container_add(GTK_CONTAINER(app.window), GTK_WIDGET(app.web_view));
        gtk_widget_show_all(app.window);

        if (auth_mode(&app) && !app.opts.self_test)
            start_callback_server(&app);

        if (app.opts.self_test) {
            const char* html =
                "<!doctype html><meta charset=utf-8><title>self-test</title>"
                "<script>addEventListener('DOMContentLoaded',()=>window.wx.postMessage(JSON.stringify({command:'user_ticket_login',data:{ticket:'self-test-ticket',identity:{userAgent:navigator.userAgent,platform:navigator.platform}}})));</script>";
            webkit_web_view_load_html(app.web_view, html, "https://self-test.invalid/");
        } else {
            webkit_web_view_load_uri(app.web_view, app.opts.login_url.c_str());
        }

        gtk_main();
        app.completed.store(true);
        stop_callback_server(&app);
        stop_external_linux_browser(&app);
        if (!fs::exists(app.opts.result_file)) {
            if (!self_test_root.empty())
                fs::remove_all(self_test_root);
            return 2;
        }
        if (app.opts.self_test) {
            const json result = json::parse(read_file(app.opts.result_file));
            const auto identity = result.value("browser_identity", json::object());
            const std::string ua = identity.value("userAgent", std::string());
            const std::string platform = identity.value("platform", std::string());
            const bool ok = result.value("kind", std::string()) == "ticket" &&
                            result.value("ticket", std::string()) == "self-test-ticket" &&
                            ua.find("Linux") != std::string::npos &&
                            ua.find("BBL-Slicer/v") != std::string::npos &&
                            ua.find("BBL-Language/") != std::string::npos &&
                            platform.find("Linux") != std::string::npos;
            std::cout << json({{"ok", ok && callback_self_test_ok}, {"platform", "linux"}, {"engine", "webkitgtk"}, {"bridge", "window.wx"}, {"callback_server", callback_self_test_ok}, {"user_agent", ua}, {"navigator_platform", platform}}).dump() << '\n';
            fs::remove_all(self_test_root);
            return ok && callback_self_test_ok ? 0 : 3;
        }
        return 0;
    } catch (const std::exception& e) {
        if (!self_test_root.empty())
            fs::remove_all(self_test_root);
        std::cerr << "slicer_linux_auth_browser: " << e.what() << '\n';
        return 1;
    }
}
