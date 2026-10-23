// ps5-homebrew-ui - Video frame uploads into GL textures.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <GL/glcorearb.h>

#include <cstdint>
#include <string>

namespace hui::gfx
{

// Ways of getting one RGBA picture a frame into a texture. They differ in
// what the GL driver does with the pixels, and on ps5-opengl that decides
// whether a 1080p picture costs a few milliseconds or two whole refreshes.
enum class UploadMethod : int
{
    automatic = 0, // measured once, the fastest is kept
    recreate = 1,  // a new texture each picture, the previous one deleted
    ring = 2,      // three textures in turn, glTexSubImage2D into the oldest
    pbo = 3,       // three textures in turn, filled from a pixel buffer
    deferred = 4,  // a new texture each picture, deleted three pictures later
};

constexpr int kUploadMethodCount = 5;
const char *upload_method_name(UploadMethod method);

class VideoUploader
{
  public:
    VideoUploader() = default;
    VideoUploader(const VideoUploader &) = delete;
    VideoUploader &operator=(const VideoUploader &) = delete;
    ~VideoUploader();

    // The method used from the next upload on; automatic measures the others
    // on the first picture after it is chosen (once per run).
    void set_method(UploadMethod method);
    UploadMethod active_method() const
    {
        return active_;
    }

    // Puts the picture into a texture and returns its name; the name stays
    // valid until the next upload() or release().
    std::uint32_t upload(int width, int height, const std::uint8_t *rgba);
    // Deletes every texture and buffer.
    void release();

    // One line per measured method, for the log; empty before a measurement.
    const std::string &benchmark_report() const
    {
        return report_;
    }

  private:
    static constexpr int kRing = 3;

    void ensure_ring(int width, int height, bool with_buffers);
    void measure(int width, int height, const std::uint8_t *rgba);
    std::uint32_t upload_with(UploadMethod method, int width, int height, const std::uint8_t *rgba);

    UploadMethod chosen_ = UploadMethod::automatic;
    UploadMethod active_ = UploadMethod::recreate;
    bool measured_ = false;
    std::string report_;

    GLuint single_ = 0; // recreate
    GLuint ring_[kRing] = {};
    GLuint buffers_[kRing] = {};
    int ring_width_ = 0;
    int ring_height_ = 0;
    int next_ = 0;
    GLuint pending_[kRing] = {}; // deferred: textures still waiting to be deleted
    int pending_next_ = 0;
    GLuint current_ = 0;
};

} // namespace hui::gfx
