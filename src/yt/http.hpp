// youtube-tv - HTTP transport used by the YouTube layer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One function, two backends:
//   console  src/platform/ps5/http_curl.cpp  - PacBrew libcurl + console_curl.c
//   PC       host/http_host.cpp              - the host's libcurl
// Requests are synchronous; call them from a worker thread, never from the
// frame. Responses are bounded by max_bytes.

#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace yt::http
{

struct Header
{
    std::string name;
    std::string value;
};

struct Response
{
    long status = 0;   // HTTP status, 0 when the request never completed
    std::string body;  // response body, truncated at max_bytes
    std::string error; // transport-level failure ("timeout", "connect", ...)

    bool ok() const
    {
        return error.empty();
    }
    bool http_ok() const
    {
        return error.empty() && status >= 200 && status < 300;
    }
};

struct Request
{
    std::string method = "GET";
    std::string url;
    std::vector<Header> headers;
    std::string body;
    long timeout_s = 20;
    std::size_t max_bytes = 8u << 20;
    // Streaming: each piece of the body as it arrives (a long poll), instead
    // of collecting it; returning false ends the transfer.
    std::function<bool(std::string_view)> on_body;
};

// Defined by the platform backend. Sets CURLOPT_NOSIGNAL / CAINFO / SO_NBIO
// on the console (console_curl_setup), then performs the request and follows
// redirects.
Response perform(const Request &request);

// Convenience wrappers over perform().
Response get(const std::string &url, const std::vector<Header> &headers = {}, long timeout_s = 20);
Response post_json(const std::string &url, const std::vector<Header> &headers,
                   const std::string &body, long timeout_s = 20);

// For the network test: how the name of `host` resolves on this platform
// ("DNS own 1.2.3.4 (1.1.1.1), PS5 5.6.7.8" on the console).
std::string describe_lookup(const std::string &host);

// "Authorization: Bearer <token>" when token is not empty, else nothing.
std::vector<Header> bearer_headers(const std::string &token);

} // namespace yt::http
