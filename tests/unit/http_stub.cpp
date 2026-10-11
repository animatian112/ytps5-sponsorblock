// youtube-tv - The unit tests' HTTP transport: canned answers only.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The definition lives in its own translation unit: the inline version in the
// header was never emitted, because no test calls yt::http::perform directly -
// only the modules under test do, so the unit binary failed to link.

#include "http_stub.hpp"

#include "yt/http.hpp"

namespace yt::http
{

Response perform(const Request &request)
{
    return hui::testing::HttpStub::instance().perform(request);
}

} // namespace yt::http
