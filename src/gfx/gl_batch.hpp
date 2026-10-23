// ps5-homebrew-ui - OpenGL backend for the 2D draw list.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "gfx/draw_list.hpp"
#include "gfx/font.hpp"

#include <GL/glcorearb.h>

#include <cstddef>
#include <cstdint>

namespace hui::gfx
{

// The texture handle of the video picture when it lives in a buffer texture
// (see GlBatch::set_video_source): an image drawn with it samples that buffer.
constexpr std::uint32_t kVideoBufferHandle = 0xe0000001u;

// Draws a DrawList with one instanced program: all instances are uploaded
// once per frame into an orphaned stream buffer, then each run is one
// glDrawArraysInstancedBaseInstance call (triangle lists only).
class GlBatch
{
  public:
    GlBatch() = default;
    GlBatch(const GlBatch &) = delete;
    GlBatch &operator=(const GlBatch &) = delete;
    ~GlBatch();

    bool init();
    // Deletes the programs, vertex arrays and buffers; init() recreates them.
    void release();
    // Uploads a font atlas as a single-level R8 texture and gives it one of
    // the font slots; returns the handle DrawList::text takes. With every
    // slot taken it returns a plain texture name, which also works but starts
    // a new draw call wherever that font is used.
    std::uint32_t create_font_texture(const Font &font);
    // Uploads RGBA8 pixels as a single-level texture; returns its name.
    std::uint32_t create_texture(int width, int height, const std::uint8_t *rgba);

    // Video pictures read from a buffer texture (GL_RGBA8 texels, one per
    // pixel, rows top first). A buffer upload is a plain copy, where a 2D
    // texture upload on ps5-opengl costs ~15 ns a texel to tile. The video
    // program is built on first use; false when this GL cannot run it, and
    // then the caller keeps 2D textures.
    bool video_buffers_supported();
    // Texels one buffer texture may hold (GL_MAX_TEXTURE_BUFFER_SIZE).
    long long max_buffer_texels();
    void set_video_source(GLuint buffer_texture, int width, int height);
    // Draws texel `index` of a filled buffer texture into a 1x1 target and
    // reads it back: true when it matches `expected` (RGBA), so a GL that
    // builds the program but samples wrongly falls back instead of showing
    // a black picture. The frame's framebuffer and viewport are restored.
    bool verify_video_buffer(GLuint buffer_texture, int index, const std::uint8_t *expected);

    // Draws into the currently bound framebuffer of the given size.
    void draw(const DrawList &list, const Viewport &viewport, int surface_width,
              int surface_height);

    std::size_t last_draw_calls() const
    {
        return draw_calls_;
    }

  private:
    GLuint program_ = 0;
    GLuint vao_ = 0;
    GLuint buffer_ = 0;
    std::size_t capacity_ = 0; // instances
    GLuint mesh_program_ = 0;
    GLuint mesh_vao_ = 0;
    GLuint mesh_buffer_ = 0;
    std::size_t mesh_capacity_ = 0; // vertices
    std::size_t draw_calls_ = 0;
    GLuint video_program_ = 0;
    int video_state_ = 0; // 0 not tried, 1 ready, -1 unavailable
    GLuint video_texture_ = 0;
    int video_width_ = 0;
    int video_height_ = 0;
    GLuint font_textures_[kFontSlots] = {};
    std::uint32_t font_count_ = 0;
};

} // namespace hui::gfx
