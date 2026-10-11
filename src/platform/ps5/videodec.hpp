// youtube-tv - PS5 hardware video decode (Videodec2) session.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The console's fixed-function media decoder has no first-party public API.
// This header is the whole surface the player sees; videodec.cpp holds the
// session and cites the public, hardware-verified ABI it is built against (see
// the long note there). Two things a caller can rely on:
//
//   * videodec_supports() says which codecs have a console-proven decode path;
//   * an open()/decode() failure always names the step that stopped it, so a
//     fallback to the software decoder can log exactly where hardware stands.

#pragma once

#include "yt/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace hui::ps5
{

// How many entry points this app imports from libSceVideodec2 (the address
// table in videodec.cpp). The startup log reports the bound subset against it.
inline constexpr int kVideodecImportCount = 26;

// How many of those imports the loader bound, 0..26. On the console this is the
// first answer to "is libSceVideodec2 reachable for a homebrew title?"; 0 means
// every later step would have to call a null pointer, which this file never
// does. videodec_probe.cpp logs it once at startup.
int videodec_imports_reachable();

// Whether the hardware path has a proven decode for this codec: AVC and VP9
// only. docs/HARDWARE_DECODE.md is explicit that AV1 is not established on the
// console ("AV1 ist dort nicht etabliert"), and audio formats never decode in
// Videodec2 at all.
bool videodec_supports(yt::Codec codec);

// One decoded picture, described but not presented: this file never touches GL
// or AGC. Binding the surface (and deciding whether it can be bound at all) is
// the player's call - docs/HARDWARE_DECODE.md lists the OpenGL interop path as
// unproven.
struct DecodedPicture
{
    const void *address = nullptr; // decoder surface, 16 KiB-aligned
    int width = 0;
    int height = 0;
    int pitch = 0;
    unsigned format = 0; // the decoder's reported frame format
};

// Depth-1 Videodec2 session for one codec/resolution. Not copyable: the session
// owns the decoder instance and its direct/flexible memory regions, both of
// which are process-global resources on the console.
class HardwareDecoder
{
  public:
    // All special members are defined out of line: the session is a pimpl, so a
    // translation unit that only holds a HardwareDecoder (the player) must not
    // instantiate the deleter of an incomplete type.
    HardwareDecoder();
    HardwareDecoder(HardwareDecoder &&) noexcept;
    HardwareDecoder &operator=(HardwareDecoder &&) noexcept;
    HardwareDecoder(const HardwareDecoder &) = delete;
    HardwareDecoder &operator=(const HardwareDecoder &) = delete;
    ~HardwareDecoder();

    // Creates the session for one codec/resolution. On failure *error names the
    // step (and the decoder library's return value); the object stays closed.
    bool open(yt::Codec codec, int width, int height, std::string *error);
    bool ready() const;
    // One compressed access unit; on success *out describes the decoded picture
    // that now sits in the caller-owned frame slot. Depth one: the caller must
    // be done with the previous picture before submitting the next unit.
    bool decode(const std::uint8_t *data, std::size_t size, DecodedPicture *out,
                std::string *error);
    void close();

  private:
    struct Session;
    std::unique_ptr<Session> m_session;
};

} // namespace hui::ps5
