// youtube-tv - Signature and "n" descrambler for ciphered stream URLs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A client whose answer gates playback behind the player's own signature
// returns signatureCipher instead of a plain URL (see yt/types.hpp). Undoing
// that is what YouTube's player JS does with two small helper functions; this
// fetches that file and interprets just those two, without a JS engine.

#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace yt
{

class Descrambler
{
  public:
    Descrambler();
    ~Descrambler();
    Descrambler(const Descrambler &) = delete;
    Descrambler &operator=(const Descrambler &) = delete;

    // Fetches the player JS through yt::http and extracts the signature and
    // throttle helpers. False leaves *error (when non-null) as the reason and
    // the object unusable but safe.
    bool prepare(std::string_view player_js_url, std::string *error);

    // The extraction half of prepare(), over JS already in memory. The unit
    // tests use it so they need no network.
    bool prepare_js(std::string_view player_js, std::string *error);

    // The URL to open for a ciphered format: applies the signature to `s`
    // (appending "&<sp>=<signature>") and the throttle helper to the "n"
    // parameter of `url`. Empty with *error set when descrambling fails.
    std::string descramble(std::string_view url, std::string_view s, std::string_view sp,
                           std::string *error) const;

    bool ready() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace yt
