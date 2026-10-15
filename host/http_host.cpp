// youtube-tv - yt::http::perform for the PC build (host snapshot + smoke test).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Same behaviour as the console backend (src/platform/ps5/http_curl.cpp), but
// it links the host's own libcurl: no console_curl.c, no SO_NBIO wrapper, and
// the host's certificate store is used as-is.

#include "yt/http.hpp"

#include <curl/curl.h>

#include <cstddef>
#include <string>

namespace yt::http
{

std::string describe_lookup(const std::string &host)
{
    return "host resolver: " + host;
}

namespace
{

// One process-wide libcurl initialisation. A function-local static is
// initialised once, thread-safely, and the calls run on worker threads.
void ensure_global()
{
    static const bool ready = []()
    {
        curl_global_init(CURL_GLOBAL_DEFAULT);
        return true;
    }();
    (void)ready;
}

// The write callback appends into the response body up to a cap. Anything
// past the cap is dropped, but the transfer is allowed to finish so a large
// page still reports its status instead of a spurious write error.
struct Sink
{
    std::string *body;
    std::size_t limit;
};

std::size_t on_write(char *data, std::size_t size, std::size_t count, void *user)
{
    Sink *sink = static_cast<Sink *>(user);
    const std::size_t bytes = size * count;
    if (bytes == 0)
        return 0;

    const std::size_t used = sink->body->size();
    const std::size_t room = sink->limit > used ? sink->limit - used : 0;
    const std::size_t take = bytes < room ? bytes : room;
    if (take > 0)
        sink->body->append(data, take);
    return bytes;
}

// Transport failures become the short strings the app shows.
const char *describe(CURLcode code)
{
    switch (code)
    {
    case CURLE_OPERATION_TIMEDOUT:
        return "timeout";
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
        return "resolve";
    case CURLE_COULDNT_CONNECT:
        return "connect";
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CERTPROBLEM:
        return "tls";
    case CURLE_WRITE_ERROR:
        return "write";
    case CURLE_READ_ERROR:
        return "read";
    case CURLE_SEND_ERROR:
        return "send";
    case CURLE_RECV_ERROR:
        return "recv";
    case CURLE_GOT_NOTHING:
        return "empty";
    case CURLE_TOO_MANY_REDIRECTS:
        return "redirect";
    case CURLE_OUT_OF_MEMORY:
        return "memory";
    default:
        return curl_easy_strerror(code);
    }
}

} // namespace

Response perform(const Request &request)
{
    Response response;
    ensure_global();

    CURL *handle = curl_easy_init();
    if (handle == nullptr)
    {
        response.error = "init";
        return response;
    }

    Sink sink{&response.body, request.max_bytes};

    curl_slist *headers = nullptr;
    for (const Header &header : request.headers)
    {
        std::string line = header.name;
        line += ": ";
        line += header.value;
        curl_slist *updated = curl_slist_append(headers, line.c_str());
        if (updated != nullptr)
            headers = updated;
    }

    const long timeout = request.timeout_s > 0 ? request.timeout_s : 20L;

    (void)curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
    (void)curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    (void)curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout);
    (void)curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, timeout);
    (void)curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    (void)curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, on_write);
    (void)curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
    if (headers != nullptr)
        (void)curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

    if (request.method == "POST")
    {
        (void)curl_easy_setopt(handle, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.data());
        (void)curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                               static_cast<curl_off_t>(request.body.size()));
    }

    const CURLcode code = curl_easy_perform(handle);

    long status = 0;
    (void)curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    response.status = status;
    if (code != CURLE_OK)
        response.error = describe(code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);
    return response;
}

} // namespace yt::http
