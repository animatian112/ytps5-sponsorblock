// ps5-homebrew-ui - Video frame uploads into GL textures.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/video_upload.hpp"

#include "gfx/gl_batch.hpp"

#include <chrono>
#include <cstdio>

namespace hui::gfx
{

namespace
{

GLuint new_texture(GLenum internal, GLenum format, int width, int height,
                   const std::uint8_t *pixels)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internal), width, height, 0, format,
                 GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void delete_texture(GLuint &texture)
{
    if (texture != 0)
    {
        glDeleteTextures(1, &texture);
        texture = 0;
    }
}

double ms_since(std::chrono::steady_clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace

const char *upload_method_name(UploadMethod method)
{
    switch (method)
    {
    case UploadMethod::automatic:
        return "auto";
    case UploadMethod::recreate:
        return "new texture";
    case UploadMethod::ring:
        return "3 textures";
    case UploadMethod::pbo:
        return "pixel buffer";
    case UploadMethod::deferred:
        return "late delete";
    case UploadMethod::buffer:
        return "buffer texture";
    }
    return "?";
}

VideoUploader::~VideoUploader()
{
    // The GL context may already be gone at exit; release() is the owner's job.
}

void VideoUploader::set_method(UploadMethod method)
{
    if (method == chosen_)
        return;
    chosen_ = method;
    if (method != UploadMethod::automatic)
        active_ = method;
    else if (!measured_)
        active_ = UploadMethod::recreate;
}

void VideoUploader::ensure_ring(int width, int height, bool with_buffers)
{
    if (ring_[0] != 0 && (ring_width_ != width || ring_height_ != height))
    {
        for (GLuint &texture : ring_)
            delete_texture(texture);
    }
    if (ring_[0] == 0)
    {
        for (GLuint &texture : ring_)
            texture = new_texture(GL_RGBA8, GL_RGBA, width, height, nullptr);
        ring_width_ = width;
        ring_height_ = height;
        next_ = 0;
    }
    if (with_buffers && buffers_[0] == 0)
        glGenBuffers(kRing, buffers_);
}

std::uint32_t VideoUploader::upload_with(UploadMethod method, int width, int height,
                                         const std::uint8_t *rgba)
{
    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    switch (method)
    {
    case UploadMethod::automatic:
    case UploadMethod::recreate:
        delete_texture(single_);
        single_ = new_texture(GL_RGBA8, GL_RGBA, width, height, rgba);
        return single_;
    case UploadMethod::ring:
    {
        ensure_ring(width, height, false);
        const GLuint texture = ring_[next_];
        next_ = (next_ + 1) % kRing;
        glBindTexture(GL_TEXTURE_2D, texture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        return texture;
    }
    case UploadMethod::pbo:
    {
        ensure_ring(width, height, true);
        const int slot = next_;
        next_ = (next_ + 1) % kRing;
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffers_[slot]);
        // Orphan, then fill: the driver may still be copying the last one.
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr,
                     GL_STREAM_DRAW);
        glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(bytes), rgba);
        glBindTexture(GL_TEXTURE_2D, ring_[slot]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        return ring_[slot];
    }
    case UploadMethod::deferred:
    {
        delete_texture(pending_[pending_next_]);
        const GLuint texture = new_texture(GL_RGBA8, GL_RGBA, width, height, rgba);
        pending_[pending_next_] = texture;
        pending_next_ = (pending_next_ + 1) % kRing;
        return texture;
    }
    case UploadMethod::buffer:
    {
        if (!buffer_fits(width, height))
            return upload_with(UploadMethod::ring, width, height, rgba);
        if (buffer_objects_[0] == 0)
        {
            glGenBuffers(kRing, buffer_objects_);
            glGenTextures(kRing, buffer_textures_);
        }
        const int slot = buffer_next_;
        buffer_next_ = (buffer_next_ + 1) % kRing;
        // A fresh store each picture (the GPU may still read the one three
        // pictures back), then a linear copy. Four pixels a GL_RGBA32UI
        // texel, so the store is rounded up to whole texels.
        const std::size_t store = (bytes + 15u) & ~static_cast<std::size_t>(15u);
        glBindBuffer(GL_TEXTURE_BUFFER, buffer_objects_[slot]);
        glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(store), nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_TEXTURE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), rgba);
        glBindTexture(GL_TEXTURE_BUFFER, buffer_textures_[slot]);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32UI, buffer_objects_[slot]);
        glBindTexture(GL_TEXTURE_BUFFER, 0);
        glBindBuffer(GL_TEXTURE_BUFFER, 0);
        current_buffer_ = buffer_textures_[slot];
        return kVideoBufferHandle;
    }
    }
    return 0;
}

void VideoUploader::measure(int width, int height, const std::uint8_t *rgba)
{
    // A one-off look at what each way costs on this console, so the next
    // log says where the time goes. Every method runs a few uploads of the
    // real picture; the probes show whether the cost follows the bytes.
    constexpr int kRuns = 4;
    char line[160];
    report_.clear();
    double best = 1e9;
    UploadMethod best_method = UploadMethod::recreate;
    const UploadMethod methods[] = {UploadMethod::recreate, UploadMethod::ring, UploadMethod::pbo,
                                    UploadMethod::deferred, UploadMethod::buffer};
    for (const UploadMethod method : methods)
    {
        if (method == UploadMethod::buffer && !buffer_fits(width, height))
        {
            report_ += "buffer texture: not available on this GL\n";
            continue;
        }
        glFinish();
        double worst = 0.0;
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kRuns; ++i)
        {
            const auto one = std::chrono::steady_clock::now();
            upload_with(method, width, height, rgba);
            const double took = ms_since(one);
            worst = took > worst ? took : worst;
        }
        const double calls = ms_since(start);
        const auto finish_start = std::chrono::steady_clock::now();
        glFinish();
        const double finish = ms_since(finish_start);
        const double each = (calls + finish) / kRuns;
        std::snprintf(line, sizeof line,
                      "%s: %.1f ms a picture (calls %.1f ms, worst %.1f, finish %.1f)",
                      upload_method_name(method), each, calls / kRuns, worst, finish);
        report_ += line;
        report_ += '\n';
        if (each < best)
        {
            best = each;
            best_method = method;
        }
    }
    // Probes: half the size, and one byte a pixel (what a luma plane costs).
    struct Probe
    {
        const char *name;
        GLenum internal;
        GLenum format;
        int width;
        int height;
    };
    const Probe probes[] = {
        {"probe RGBA half size", GL_RGBA8, GL_RGBA, width / 2, height / 2},
        {"probe R8 full size", GL_R8, GL_RED, width, height},
    };
    for (const Probe &probe : probes)
    {
        glFinish();
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kRuns; ++i)
        {
            GLuint texture =
                new_texture(probe.internal, probe.format, probe.width, probe.height, rgba);
            delete_texture(texture);
        }
        const double calls = ms_since(start);
        const auto finish_start = std::chrono::steady_clock::now();
        glFinish();
        std::snprintf(line, sizeof line, "%s %dx%d: %.1f ms each (finish %.1f)", probe.name,
                      probe.width, probe.height, calls / kRuns, ms_since(finish_start));
        report_ += line;
        report_ += '\n';
    }
    std::snprintf(line, sizeof line, "using %s", upload_method_name(best_method));
    report_ += line;
    release(); // drop what the other methods left behind
    active_ = best_method;
    measured_ = true;
}

std::uint32_t VideoUploader::upload(int width, int height, const std::uint8_t *rgba)
{
    if (rgba == nullptr || width <= 0 || height <= 0)
        return current_;
    if (chosen_ == UploadMethod::automatic && !measured_)
        measure(width, height, rgba);
    current_ = upload_with(active_, width, height, rgba);
    return current_;
}

void VideoUploader::release()
{
    delete_texture(single_);
    for (GLuint &texture : ring_)
        delete_texture(texture);
    for (GLuint &texture : pending_)
        delete_texture(texture);
    if (buffers_[0] != 0)
    {
        glDeleteBuffers(kRing, buffers_);
        for (GLuint &buffer : buffers_)
            buffer = 0;
    }
    if (buffer_objects_[0] != 0)
    {
        glDeleteTextures(kRing, buffer_textures_);
        glDeleteBuffers(kRing, buffer_objects_);
        for (int i = 0; i < kRing; ++i)
        {
            buffer_textures_[i] = 0;
            buffer_objects_[i] = 0;
        }
    }
    ring_width_ = 0;
    ring_height_ = 0;
    next_ = 0;
    pending_next_ = 0;
    buffer_next_ = 0;
    current_ = 0;
    current_buffer_ = 0;
}

} // namespace hui::gfx
