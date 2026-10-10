// youtube-tv - HTTP transport: the convenience wrappers over perform().
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// perform() itself is the platform backend (console libcurl in
// src/platform/ps5/http_curl.cpp, the host's libcurl in host/http_host.cpp).
// These helpers only fill a Request and forward it.

#include "yt/http.hpp"

namespace yt::http
{

Response get(const std::string &url, const std::vector<Header> &headers, long timeout_s)
{
    Request request;
    request.method = "GET";
    request.url = url;
    request.headers = headers;
    request.timeout_s = timeout_s;
    return perform(request);
}

Response post_json(const std::string &url, const std::vector<Header> &headers,
                   const std::string &body, long timeout_s)
{
    Request request;
    request.method = "POST";
    request.url = url;
    request.headers = headers;
    request.body = body;
    request.timeout_s = timeout_s;
    return perform(request);
}

std::vector<Header> bearer_headers(const std::string &token)
{
    if (token.empty())
        return {};
    return {Header{"Authorization", "Bearer " + token}};
}

} // namespace yt::http
