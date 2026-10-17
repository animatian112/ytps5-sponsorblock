// youtube-tv - Thumbnail and QR textures: downloaded on worker threads,
// uploaded on the frame, evicted least-recently-used.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace hui::gfx
{
class GlBatch;
}

namespace hui::app
{

class ThumbCache
{
  public:
    // How a texture is freed when an entry is evicted. The console passes
    // glDeleteTextures; the unit tests pass a no-op.
    using Destroy = void (*)(std::uint32_t texture);

    // batch uploads; destroy frees. capacity is the number of live textures.
    ThumbCache(gfx::GlBatch &batch, Destroy destroy, std::size_t capacity = 64);
    ~ThumbCache();
    ThumbCache(const ThumbCache &) = delete;
    ThumbCache &operator=(const ThumbCache &) = delete;

    // The texture for an image URL, or 0 while it is still downloading or
    // decoding (the caller draws its placeholder). Requests the download
    // once; calling it every frame is cheap.
    std::uint32_t texture(const std::string &url);
    // width / height of a loaded image, 0 while it is not loaded.
    float aspect(const std::string &url) const;
    // Registers an already decoded image (the QR bitmap) under `key`.
    std::uint32_t upload(const std::string &key, const std::uint8_t *rgba, int width, int height);
    // Uploads finished downloads and frees evicted textures; call once per
    // frame from the frame thread.
    void pump();
    // Drops everything (leaving the player, signing out).
    void clear();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hui::app
