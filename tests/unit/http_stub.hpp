// youtube-tv - Test transport: canned answers for yt::http, no sockets.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The unit binary links no HTTP backend (host/http_host.cpp is left out of
// sources.txt the way the update transport is): this header supplies
// yt::http::perform instead, so a test can drive the auth and feed paths
// against canned answers and read back the requests the code sent - the
// Authorization header of a feed request, for example.

#pragma once

#include "yt/http.hpp"

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace hui::testing
{

// One recorded call: everything the code sent and the answer it got.
struct HttpExchange
{
    yt::http::Request request;
    yt::http::Response response;
};

// The one place every yt::http call in the unit binary lands. A test installs
// a handler per case and reads exchanges() afterwards; a request no handler
// answers is recorded and answered with a transport error, so an unexpected
// call fails the test's expectations instead of reaching the network.
class HttpStub
{
  public:
    using Handler = std::function<yt::http::Response(const yt::http::Request &)>;

    static HttpStub &instance()
    {
        static HttpStub stub;
        return stub;
    }

    void set_handler(Handler handler)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::move(handler);
        log_.clear();
    }

    void clear()
    {
        set_handler(Handler{});
    }

    yt::http::Response perform(const yt::http::Request &request)
    {
        Handler handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler = handler_;
        }
        yt::http::Response response = handler ? handler(request) : yt::http::Response{};
        if (!handler)
            response.error = "http_stub: no handler";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            log_.push_back({request, response});
        }
        return response;
    }

    std::vector<HttpExchange> exchanges() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return log_;
    }

  private:
    mutable std::mutex mutex_;
    Handler handler_;
    std::vector<HttpExchange> log_;
};

} // namespace hui::testing
