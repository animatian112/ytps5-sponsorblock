// youtube-tv - yt.log to a phone: a QR code and a one-file web server.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sending a log took FTP. Now the app serves it on the home network while
// its QR code is shown: the phone scans http://<console>:<port>/<token>,
// gets a small page with a download button, saves yt.log and shares it like
// any file. The token is random per opening, so only the phone that sees the
// code finds the file, and the server stops when the code is closed.

#pragma once

#include <memory>
#include <string>

namespace yt
{

class LogShare
{
  public:
    LogShare();
    ~LogShare();
    LogShare(const LogShare &) = delete;
    LogShare &operator=(const LogShare &) = delete;

    // Starts serving; false with *error when there is no network address or
    // no port to listen on.
    bool start(std::string *error);
    void stop();
    bool running() const;
    // The address to scan, e.g. http://192.168.1.20:8090/3f9a1c7e
    const std::string &url() const;
    // How many times the log was downloaded since start().
    int downloads() const;

    // What the server thread shares with the object (defined in the .cpp).
    struct State;

  private:
    std::shared_ptr<State> state_;
    std::string url_;
};

// The HTTP answer for a request line's path (exposed for the tests):
// the page, the file, or 404.
std::string log_share_answer(const std::string &request_path, const std::string &token,
                             const std::string &log_text, bool *was_download);

} // namespace yt
