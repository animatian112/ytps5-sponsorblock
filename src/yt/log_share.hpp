// youtube-tv - yt.log to a phone: a QR code and a one-file web server.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sending a log took FTP. Now the app serves it on the home network while
// its QR code is shown: the phone scans http://<console>:<port>/<token>,
// gets a small page with a download button, saves yt.log and shares it like
// any file. The token is random per opening, so only the phone that sees the
// code finds the file, and the server stops when the code is closed. Where
// the console refuses to listen, the log is pasted to dpaste.com (deleted
// after 7 days) and the code opens that.

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

    // When the console will not let the app listen (the PS5's sandbox says
    // "permission denied"): the log goes up to dpaste.com instead, kept 7
    // days, and the code points there. Runs on a thread of its own.
    void upload();
    bool uploading() const;
    // The paste's address once it is up (the raw file: ...txt), else "".
    std::string uploaded_url() const;
    // Why the upload failed, else "".
    std::string upload_error() const;

    // What the server thread shares with the object (defined in the .cpp).
    struct State;
    struct Upload;

  private:
    std::shared_ptr<State> state_;
    std::shared_ptr<Upload> upload_;
    std::string url_;
};

// dpaste's form body for a log (exposed for the tests).
std::string log_paste_form(const std::string &log_text);

// The HTTP answer for a request line's path (exposed for the tests):
// the page, the file, or 404.
std::string log_share_answer(const std::string &request_path, const std::string &token,
                             const std::string &log_text, bool *was_download);

} // namespace yt
