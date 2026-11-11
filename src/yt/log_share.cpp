// youtube-tv - yt.log to a phone: a QR code and a one-file web server.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/log_share.hpp"

#include "yt/log.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <pthread.h>
#include <random>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace yt
{

struct LogShare::State
{
    std::atomic<bool> stop{false};
    std::atomic<int> downloads{0};
    int listener = -1;
    std::string token;
};

namespace
{

constexpr std::size_t kMaxLog = 8u << 20;

std::string header(int status, const char *reason, const char *type, std::size_t length,
                   const char *extra)
{
    char text[512];
    std::snprintf(text, sizeof text,
                  "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                  "Cache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                  status, reason, type, length, extra);
    return text;
}

// The console's address on the home network: the one a UDP socket would
// send from (nothing is sent).
std::string local_address()
{
    const int probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe < 0)
        return {};
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    remote.sin_addr.s_addr = htonl(0x08080808); // 8.8.8.8, only to pick the route
    std::string out;
    if (connect(probe, reinterpret_cast<sockaddr *>(&remote), sizeof remote) == 0)
    {
        sockaddr_in local{};
        socklen_t size = sizeof local;
        if (getsockname(probe, reinterpret_cast<sockaddr *>(&local), &size) == 0)
        {
            char text[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof text) != nullptr)
                out = text;
        }
    }
    close(probe);
    return out == "0.0.0.0" ? std::string() : out;
}

void send_all(int fd, const std::string &data)
{
    std::size_t at = 0;
    while (at < data.size())
    {
        const ssize_t sent = send(fd, data.data() + at, data.size() - at, 0);
        if (sent <= 0)
            return;
        at += static_cast<std::size_t>(sent);
    }
}

void serve(const std::shared_ptr<LogShare::State> &state, int client)
{
    timeval timeout{5, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    std::string request;
    char buffer[2048];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192)
    {
        const ssize_t got = recv(client, buffer, sizeof buffer, 0);
        if (got <= 0)
            break;
        request.append(buffer, static_cast<std::size_t>(got));
    }
    std::string path;
    if (request.rfind("GET ", 0) == 0)
    {
        const std::size_t end = request.find(' ', 4);
        if (end != std::string::npos)
            path = request.substr(4, end - 4);
    }
    bool download = false;
    const bool wants_file = path.find("/yt.log") != std::string::npos;
    const std::string answer = log_share_answer(
        path, state->token, wants_file ? log::read_all(kMaxLog) : std::string(), &download);
    send_all(client, answer);
    if (download)
        state->downloads++;
    close(client);
}

void run(std::shared_ptr<LogShare::State> state)
{
    while (!state->stop)
    {
        sockaddr_in peer{};
        socklen_t size = sizeof peer;
        const int client = accept(state->listener, reinterpret_cast<sockaddr *>(&peer), &size);
        if (client < 0)
        {
            if (state->stop)
                break;
            usleep(100 * 1000); // timeout or a passing error
            continue;
        }
        serve(state, client);
    }
    close(state->listener);
}

void *entry(void *argument)
{
    auto *state = static_cast<std::shared_ptr<LogShare::State> *>(argument);
    run(*state);
    delete state;
    return nullptr;
}

} // namespace

std::string log_share_answer(const std::string &request_path, const std::string &token,
                             const std::string &log_text, bool *was_download)
{
    *was_download = false;
    const std::string base = "/" + token;
    if (token.empty() || request_path.rfind(base, 0) != 0 ||
        (request_path.size() > base.size() && request_path[base.size()] != '/' &&
         request_path[base.size()] != '?'))
    {
        const std::string body = "Not found\n";
        return header(404, "Not Found", "text/plain; charset=utf-8", body.size(), "") + body;
    }
    const std::string rest = request_path.substr(base.size());
    if (rest.rfind("/yt.log", 0) == 0)
    {
        *was_download = true;
        const bool view = rest.find("view") != std::string::npos;
        return header(200, "OK", "text/plain; charset=utf-8", log_text.size(),
                      view ? "" : "Content-Disposition: attachment; filename=\"yt.log\"\r\n") +
               log_text;
    }
    // The page: a download button, and a view link for a phone that will not
    // save a file.
    const std::string page =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>TubeBlock log</title><style>"
        "body{font-family:system-ui,sans-serif;background:#0f0f0f;color:#f1f1f1;margin:0;"
        "padding:32px 20px;text-align:center}h1{font-size:22px}p{color:#aaa;line-height:1.5}"
        "a.b{display:block;margin:18px auto;max-width:320px;padding:16px;border-radius:12px;"
        "background:#3ea6ff;color:#0f0f0f;font-weight:600;text-decoration:none;font-size:18px}"
        "a.v{color:#3ea6ff}</style></head><body><h1>TubeBlock log</h1>"
        "<p>Save yt.log to this phone, then attach it to your report.</p>"
        "<a class=\"b\" href=\"" +
        base +
        "/yt.log\" download=\"yt.log\">Download yt.log</a>"
        "<p><a class=\"v\" href=\"" +
        base + "/yt.log?view\">Open it as text</a></p></body></html>";
    return header(200, "OK", "text/html; charset=utf-8", page.size(), "") + page;
}

LogShare::LogShare() = default;

LogShare::~LogShare()
{
    stop();
}

bool LogShare::start(std::string *error)
{
    stop();
    const std::string address = local_address();
    if (address.empty())
    {
        *error = "no network address (is the console online?)";
        return false;
    }
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
    {
        *error = std::string("socket: ") + std::strerror(errno);
        return false;
    }
    const int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    int port = 0;
    for (int candidate = 8090; candidate < 8100 && port == 0; ++candidate)
    {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(static_cast<std::uint16_t>(candidate));
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(listener, reinterpret_cast<sockaddr *>(&local), sizeof local) == 0)
            port = candidate;
    }
    if (port == 0 || listen(listener, 4) != 0)
    {
        *error = std::string("listen: ") + std::strerror(errno);
        close(listener);
        return false;
    }
    // accept() wakes every second so a stop is seen.
    timeval timeout{1, 0};
    setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    state_ = std::make_shared<State>();
    state_->listener = listener;
    std::random_device random;
    char token[16];
    std::snprintf(token, sizeof token, "%08x", static_cast<unsigned>(random()));
    state_->token = token;
    auto *argument = new std::shared_ptr<State>(state_);
    pthread_attr_t attributes;
    pthread_t thread;
    bool created = false;
    if (pthread_attr_init(&attributes) == 0)
    {
        pthread_attr_setstacksize(&attributes, 256u << 10);
        pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        created = pthread_create(&thread, &attributes, entry, argument) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (!created)
    {
        delete argument;
        close(listener);
        state_.reset();
        *error = "no thread";
        return false;
    }
    char url[96];
    std::snprintf(url, sizeof url, "http://%s:%d/%s", address.c_str(), port, token);
    url_ = url;
    log::line("logshare: serving yt.log at %s", url_.c_str());
    return true;
}

void LogShare::stop()
{
    if (state_ != nullptr)
    {
        state_->stop = true;
        log::line("logshare: stopped (%d downloads)", state_->downloads.load());
    }
    state_.reset();
    url_.clear();
}

bool LogShare::running() const
{
    return state_ != nullptr;
}

const std::string &LogShare::url() const
{
    return url_;
}

int LogShare::downloads() const
{
    return state_ != nullptr ? state_->downloads.load() : 0;
}

} // namespace yt
