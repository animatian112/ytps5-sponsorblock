// youtube-tv - Sign-in: OAuth 2.0 device flow (TV code and QR code).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Google's device flow is the only sign-in YouTube offers to a TV client: the
// screen shows a code (and the QR of the same URL), the player confirms on a
// phone, and the app keeps the refresh token in /download0/auth.json.

#pragma once

#include <string>

namespace yt
{

struct DeviceCode
{
    std::string device_code;
    std::string user_code;        // what the player types on the phone
    std::string verification_url; // QR target, verbatim from the response
    int interval_s = 5;           // polling interval
    std::string error;            // empty = usable
    bool ok() const
    {
        return error.empty();
    }
};

enum class Poll : unsigned char
{
    pending,   // authorization_pending: keep waiting
    slow_down, // server asked to wait longer
    done,      // tokens issued
    denied,    // access_denied
    expired,   // expired_token: begin() again
    failed,    // anything else; the error string has the detail
};

class Auth
{
  public:
    // token_path is /download0/auth.json on the console.
    explicit Auth(std::string token_path);
    ~Auth();

    DeviceCode begin();
    Poll poll(const DeviceCode &code, std::string *error);
    // Exchanges the saved refresh token for an access token. A grant the
    // server rejects signs the account out (memory and file), so a rejected
    // token ends in a clean sign-in state instead of a dead one.
    bool refresh(std::string *error);
    // The access token of the last successful sign-in or refresh.
    const std::string &access_token() const;
    // Where the token is kept (the file name is chosen by the app).
    const std::string &token_path() const;
    bool signed_in() const;
    void sign_out();
    // The saved refresh token (empty when signed out): the key the app's
    // account list remembers each account by.
    const std::string &refresh_token() const;

    // The saved refresh token as the JSON the app writes: {"v":1,
    // "refresh_token":"..."}. Empty when nobody is signed in. Used to keep a
    // second copy where the user can reach it (the install folder), so a
    // wiped sandbox does not cost the account.
    std::string token_json() const;
    // Adopts a token from such a JSON (a file the user put in the install
    // folder). Returns false when the text has no usable refresh token; a
    // token that the server rejects is reported by the refresh() call that
    // follows.
    bool adopt_token_json(std::string_view json, std::string *error);

  private:
    struct Impl;
    Impl *impl_;
};

// The address the sign-in QR code opens: the device page with the code
// already in its box (?user_code=), so a phone that scans it only has to
// confirm. Google's page fills the field from that parameter; if it ever
// ignores it, the code is still on screen to type. Adds https:// when the
// answer names the page without a scheme.
std::string prefilled_verification_url(const std::string &verification_url,
                                       const std::string &user_code);

} // namespace yt
