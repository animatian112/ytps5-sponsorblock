// ps5-homebrew-ui - OpenGL backend for the 2D draw list.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/gl_batch.hpp"

#include "gfx/gl_program.hpp"
#include "platform/ps5/system.hpp"

#include <algorithm>
#include <cmath>

namespace hui::gfx
{

namespace
{

constexpr const char *kVertex = R"(
layout(location = 0) in vec4 a_rect;
layout(location = 1) in vec4 a_top;
layout(location = 2) in vec4 a_bottom;
layout(location = 3) in vec4 a_border;
layout(location = 4) in vec4 a_params;
layout(location = 5) in vec4 a_extra;
layout(location = 0) uniform vec4 u_viewport; // scale, offset x, offset y
layout(location = 1) uniform vec2 u_surface;
out vec2 v_local;
out vec2 v_virtual;
out vec2 v_uv;
out float v_t;
flat out vec2 v_half;
flat out vec4 v_top;
flat out vec4 v_bottom;
flat out vec4 v_border;
flat out vec4 v_params;
flat out vec4 v_extra;
const vec2 kCorners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                 vec2(1.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0));
void main()
{
    vec2 corner = kCorners[gl_VertexID];
    int shape = int(a_params.w + 0.5);
    vec2 size = a_rect.zw;
    // Grow shapes by their softness plus one output pixel for anti-aliasing;
    // glyph and image quads map texels exactly and are not grown.
    float pad = (shape == 1 || shape == 3) ? 0.0 : a_params.z + 1.0 / u_viewport.x;
    vec2 local = mix(vec2(-pad), size + vec2(pad), corner);
    vec2 centred = local - 0.5 * size;
    vec2 virt = a_rect.xy + local;
    // Rounded rectangles, shadows and triangles may turn about their centre: the quad
    // rotates, the distance field keeps working in the unrotated frame.
    bool box = shape == 0 || shape == 2 || shape == 5;
    if (box && a_extra.x != 0.0)
    {
        float c = cos(a_extra.x);
        float s = sin(a_extra.x);
        virt = a_rect.xy + 0.5 * size + vec2(c * centred.x - s * centred.y,
                                             s * centred.x + c * centred.y);
    }
    v_virtual = virt;
    v_local = centred;
    v_half = 0.5 * size;
    v_t = (shape == 0 && a_extra.y > 0.5) ? clamp(local.x / max(size.x, 1e-3), 0.0, 1.0)
                                          : clamp(local.y / max(size.y, 1e-3), 0.0, 1.0);
    v_uv = mix(a_extra.xy, a_extra.zw, corner);
    v_top = a_top;
    v_bottom = a_bottom;
    v_border = a_border;
    v_params = a_params;
    v_extra = a_extra;
    vec2 surface = virt * u_viewport.x + u_viewport.yz;
    vec2 ndc = surface / u_surface * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

constexpr const char *kFragment = R"(
layout(location = 0) uniform vec4 u_viewport;
layout(location = 2) uniform sampler2D u_texture;
layout(location = 3) uniform sampler2D u_fonts[6];
in vec2 v_local;
in vec2 v_virtual;
in vec2 v_uv;
in float v_t;
flat in vec2 v_half;
flat in vec4 v_top;
flat in vec4 v_bottom;
flat in vec4 v_border;
flat in vec4 v_params;
flat in vec4 v_extra;
out vec4 frag_color;

float round_box(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float triangle(vec2 p, vec2 p0, vec2 p1, vec2 p2)
{
    vec2 e0 = p1 - p0, e1 = p2 - p1, e2 = p0 - p2;
    vec2 v0 = p - p0, v1 = p - p1, v2 = p - p2;
    vec2 q0 = v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0);
    vec2 q1 = v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0);
    vec2 q2 = v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0);
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    vec2 d = min(min(vec2(dot(q0, q0), s * (v0.x * e0.y - v0.y * e0.x)),
                     vec2(dot(q1, q1), s * (v1.x * e1.y - v1.y * e1.x))),
                 vec2(dot(q2, q2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

float star5(vec2 p, float r, float rf)
{
    const vec2 k1 = vec2(0.809016994375, -0.587785252292);
    const vec2 k2 = vec2(-k1.x, k1.y);
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}

void main()
{
    float px = 1.0 / u_viewport.x; // virtual units per output pixel
    int shape = int(v_params.w + 0.5);
    vec4 fill = mix(v_top, v_bottom, v_t);
    vec4 color;
    if (shape == 0)
    {
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        float d = round_box(v_local, v_half, radius);
        color = fill;
        if (v_params.y > 0.0)
        {
            float inner = clamp(0.5 - (d + v_params.y) / px, 0.0, 1.0);
            color = mix(v_border, fill, inner);
        }
        color.a *= clamp(0.5 - d / px, 0.0, 1.0);
    }
    else if (shape == 1)
    {
        // The font atlases stay bound to their own units; the slot picks one.
        int slot = int(v_params.z + 0.5);
        float sdf;
        if (slot == 1)
            sdf = texture(u_fonts[0], v_uv).r;
        else if (slot == 2)
            sdf = texture(u_fonts[1], v_uv).r;
        else if (slot == 3)
            sdf = texture(u_fonts[2], v_uv).r;
        else if (slot == 4)
            sdf = texture(u_fonts[3], v_uv).r;
        else if (slot == 5)
            sdf = texture(u_fonts[4], v_uv).r;
        else if (slot == 6)
            sdf = texture(u_fonts[5], v_uv).r;
        else
            sdf = texture(u_texture, v_uv).r;
        float distance = (sdf - 0.5) * 2.0 * v_params.x;
        color = vec4(fill.rgb, fill.a * clamp(distance / px + 0.5, 0.0, 1.0));
    }
    else if (shape == 2)
    {
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        float d = round_box(v_local, v_half, radius);
        color = vec4(fill.rgb, fill.a * (1.0 - smoothstep(-v_params.z, v_params.z, d)));
    }
    else if (shape == 3)
    {
        color = texture(u_texture, v_uv) * fill;
        if (v_params.x > 0.0)
        {
            float radius = min(v_params.x, min(v_half.x, v_half.y));
            color.a *= clamp(0.5 - round_box(v_local, v_half, radius) / px, 0.0, 1.0);
        }
    }
    else if (shape == 4)
    {
        vec2 pa = v_virtual - v_extra.xy;
        vec2 ba = v_extra.zw - v_extra.xy;
        float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-6), 0.0, 1.0);
        float d = length(pa - ba * h) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    else if (shape == 8)
    {
        // Box with 45 degree corner cuts: the box distance, limited by the
        // diagonal plane through each corner.
        float cut = min(v_params.x, min(v_half.x, v_half.y));
        vec2 q = abs(v_local) - v_half;
        float box = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
        float d = max(box, (q.x + q.y + cut) * 0.70710678);
        color = fill;
        if (v_params.y > 0.0)
        {
            float inner = clamp(0.5 - (d + v_params.y) / px, 0.0, 1.0);
            color = mix(v_border, fill, inner);
        }
        color.a *= clamp(0.5 - d / px, 0.0, 1.0);
    }
    else if (shape == 7)
    {
        // Ring sector, after Inigo Quilez's arc: turn the sector upright,
        // fold it on its axis, then measure to the ring or to its end.
        float rb = 0.5 * v_params.y;
        float ra = min(v_half.x, v_half.y) - rb;
        float half_sweep = 0.5 * v_extra.x;
        float mid = v_params.x + half_sweep;
        float cm = cos(mid);
        float sm = sin(mid);
        vec2 p = vec2(v_local.x, -v_local.y);
        p = vec2(abs(p.x * cm - p.y * sm), p.x * sm + p.y * cm);
        vec2 sc = vec2(sin(half_sweep), cos(half_sweep));
        float ring = abs(length(p) - ra);
        float d;
        if (v_extra.y > 0.5)
            d = max(ring - rb, p.x * sc.y - p.y * sc.x);
        else
            d = ((sc.y * p.x > sc.x * p.y) ? length(p - sc * ra) : ring) - rb;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    else if (shape == 6)
    {
        vec2 p = vec2(v_local.x, -v_local.y);
        float d = star5(p, min(v_half.x, v_half.y), 0.45);
        if (v_params.y > 0.0)
            d = abs(d + 0.5 * v_params.y) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    else
    {
        float d = triangle(v_local, vec2(0.0, -v_half.y), vec2(-v_half.x, v_half.y),
                           vec2(v_half.x, v_half.y));
        if (v_params.y > 0.0)
            d = abs(d + 0.5 * v_params.y) - 0.5 * v_params.y;
        color = vec4(fill.rgb, fill.a * clamp(0.5 - d / px, 0.0, 1.0));
    }
    if (color.a <= 0.0)
        discard;
    frag_color = color;
}
)";

// The video picture from a buffer texture: four pixel fetches and a
// bilinear blend stand in for the 2D texture's filtering. Same vertex shader
// and instance layout as the batch program, image shape only. The buffer is
// GL_RGBA32UI, four RGBA8 pixels a texel: ps5-opengl allows 1M texels a
// buffer texture, a quarter of a 1080p picture's pixels.
constexpr const char *kVideoFragment = R"(
layout(location = 0) uniform vec4 u_viewport;
layout(location = 9) uniform usamplerBuffer u_video;
layout(location = 10) uniform ivec2 u_video_size;
in vec2 v_local;
in vec2 v_uv;
in float v_t;
flat in vec2 v_half;
flat in vec4 v_top;
flat in vec4 v_bottom;
flat in vec4 v_params;
out vec4 frag_color;

vec4 fetch(ivec2 p)
{
    p = clamp(p, ivec2(0), u_video_size - ivec2(1));
    int index = p.y * u_video_size.x + p.x;
    uvec4 four = texelFetch(u_video, index >> 2);
    return unpackUnorm4x8(four[index & 3]);
}

float round_box(vec2 p, vec2 b, float r)
{
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main()
{
    vec2 pos = v_uv * vec2(u_video_size) - 0.5;
    vec2 base = floor(pos);
    vec2 f = pos - base;
    ivec2 i = ivec2(base);
    vec4 top = mix(fetch(i), fetch(i + ivec2(1, 0)), f.x);
    vec4 bottom = mix(fetch(i + ivec2(0, 1)), fetch(i + ivec2(1, 1)), f.x);
    vec4 color = mix(top, bottom, f.y) * mix(v_top, v_bottom, v_t);
    if (v_params.x > 0.0)
    {
        float px = 1.0 / u_viewport.x;
        float radius = min(v_params.x, min(v_half.x, v_half.y));
        color.a *= clamp(0.5 - round_box(v_local, v_half, radius) / px, 0.0, 1.0);
    }
    frag_color = color;
}
)";

// The buffer-texture self-check: one texel onto a 1x1 target.
constexpr const char *kProbeVertex = R"(
void main()
{
    const vec2 corners[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    gl_Position = vec4(corners[gl_VertexID], 0.0, 1.0);
}
)";

constexpr const char *kProbeFragment = R"(
layout(location = 0) uniform usamplerBuffer u_video;
layout(location = 1) uniform int u_index;
out vec4 frag_color;
void main()
{
    uvec4 four = texelFetch(u_video, u_index >> 2);
    frag_color = unpackUnorm4x8(four[u_index & 3]);
}
)";

constexpr const char *kMeshVertex = R"(
layout(location = 0) in vec2 a_position;
layout(location = 1) in vec4 a_color;
layout(location = 0) uniform vec4 u_viewport;
layout(location = 1) uniform vec2 u_surface;
out vec4 v_color;
void main()
{
    vec2 surface = a_position * u_viewport.x + u_viewport.yz;
    vec2 ndc = surface / u_surface * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    v_color = a_color;
}
)";

constexpr const char *kMeshFragment = R"(
in vec4 v_color;
out vec4 frag_color;
void main()
{
    frag_color = v_color;
}
)";

} // namespace

GlBatch::~GlBatch()
{
    release();
}

void GlBatch::release()
{
    if (buffer_ != 0)
        glDeleteBuffers(1, &buffer_);
    if (vao_ != 0)
        glDeleteVertexArrays(1, &vao_);
    if (program_ != 0)
        glDeleteProgram(program_);
    if (mesh_buffer_ != 0)
        glDeleteBuffers(1, &mesh_buffer_);
    if (mesh_vao_ != 0)
        glDeleteVertexArrays(1, &mesh_vao_);
    if (mesh_program_ != 0)
        glDeleteProgram(mesh_program_);
    if (video_program_ != 0)
        glDeleteProgram(video_program_);
    video_program_ = 0;
    video_state_ = 0;
    video_texture_ = 0;
    if (font_count_ != 0)
        glDeleteTextures(static_cast<GLsizei>(font_count_), font_textures_);
    for (GLuint &texture : font_textures_)
        texture = 0;
    font_count_ = 0;
    buffer_ = vao_ = program_ = mesh_buffer_ = mesh_vao_ = mesh_program_ = 0;
    capacity_ = 0;
    mesh_capacity_ = 0;
}

bool GlBatch::init()
{
    program_ = build_program("batch2d", kVertex, kFragment);
    mesh_program_ = build_program("mesh2d", kMeshVertex, kMeshFragment);
    if (program_ == 0 || mesh_program_ == 0)
        return false;
    glGenVertexArrays(1, &mesh_vao_);
    glGenBuffers(1, &mesh_buffer_);
    glBindVertexArray(mesh_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, mesh_buffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(MeshVertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(MeshVertex),
                          reinterpret_cast<const void *>(static_cast<std::uintptr_t>(8)));
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &buffer_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    constexpr GLsizei stride = sizeof(Instance);
    for (GLuint attribute = 0; attribute < 6; ++attribute)
    {
        glEnableVertexAttribArray(attribute);
        glVertexAttribPointer(
            attribute, 4, GL_FLOAT, GL_FALSE, stride,
            reinterpret_cast<const void *>(static_cast<std::uintptr_t>(attribute * 16)));
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);
    return true;
}

std::uint32_t GlBatch::create_font_texture(const Font &font)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, font.atlas_width(), font.atlas_height(), 0, GL_RED,
                 GL_UNSIGNED_BYTE, font.atlas().data());
    // One level only: mip chains leave ps5-opengl's batched fast path.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (font_count_ < kFontSlots)
    {
        font_textures_[font_count_] = texture;
        ++font_count_;
        return kFontHandleBase | font_count_;
    }
    return texture;
}

bool GlBatch::video_buffers_supported()
{
    if (video_state_ == 0)
    {
        video_state_ = -1;
        if (max_buffer_texels() * 4 >= 1920LL * 1080LL)
        {
            video_program_ = build_program("video_buffer", kVertex, kVideoFragment);
            if (video_program_ != 0)
                video_state_ = 1;
        }
    }
    return video_state_ == 1;
}

long long GlBatch::max_buffer_texels()
{
    GLint texels = 0;
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texels);
    while (glGetError() != GL_NO_ERROR)
    {
    }
    return texels;
}

void GlBatch::set_video_source(GLuint buffer_texture, int width, int height)
{
    video_texture_ = buffer_texture;
    video_width_ = width;
    video_height_ = height;
}

bool GlBatch::verify_video_buffer(GLuint buffer_texture, int index, const std::uint8_t *expected)
{
    const GLuint program = build_program("video_buffer_probe", kProbeVertex, kProbeFragment);
    if (program == 0)
        return false;
    // Whatever the frame has bound is put back afterwards.
    GLint previous_draw = 0;
    GLint previous_read = 0;
    GLint previous_viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous_draw);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previous_read);
    glGetIntegerv(GL_VIEWPORT, previous_viewport);
    GLuint target = 0;
    GLuint framebuffer = 0;
    GLuint vao = 0;
    glGenTextures(1, &target);
    glBindTexture(GL_TEXTURE_2D, target);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    std::uint8_t got[4] = {0, 0, 0, 0};
    if (ok)
    {
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glViewport(0, 0, 1, 1);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glUseProgram(program);
        glUniform1i(0, 7);
        glUniform1i(1, index);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_BUFFER, buffer_texture);
        glActiveTexture(GL_TEXTURE0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
        glBindVertexArray(0);
        for (int c = 0; c < 4; ++c)
        {
            const int difference = static_cast<int>(got[c]) - static_cast<int>(expected[c]);
            if (difference > 2 || difference < -2)
                ok = false;
        }
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(previous_draw));
    glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(previous_read));
    glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2],
               previous_viewport[3]);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &target);
    if (vao != 0)
        glDeleteVertexArrays(1, &vao);
    glDeleteProgram(program);
    while (glGetError() != GL_NO_ERROR)
    {
    }
    sys::log("[HUI] video buffer check: texel %d read %u,%u,%u,%u, expected %u,%u,%u,%u -> %s",
             index, got[0], got[1], got[2], got[3], expected[0], expected[1], expected[2],
             expected[3], ok ? "ok" : "mismatch");
    return ok;
}

std::uint32_t GlBatch::create_texture(int width, int height, const std::uint8_t *rgba)
{
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return texture;
}

void GlBatch::draw(const DrawList &list, const Viewport &viewport, int surface_width,
                   int surface_height)
{
    draw_calls_ = 0;
    const auto &instances = list.instances();
    const auto &mesh = list.mesh_vertices();
    if (instances.empty() && mesh.empty())
        return;
    // Orphan, then fill: re-specifying storage avoids waiting on the GPU's
    // reads of the previous frame (ps5-opengl drains on same-size reuse).
    if (!instances.empty())
    {
        glBindBuffer(GL_ARRAY_BUFFER, buffer_);
        capacity_ = std::max(capacity_, instances.size());
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(capacity_ * sizeof(Instance)),
                     nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(instances.size() * sizeof(Instance)),
                        instances.data());
    }
    if (!mesh.empty())
    {
        glBindBuffer(GL_ARRAY_BUFFER, mesh_buffer_);
        mesh_capacity_ = std::max(mesh_capacity_, mesh.size());
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(mesh_capacity_ * sizeof(MeshVertex)),
                     nullptr, GL_STREAM_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(mesh.size() * sizeof(MeshVertex)), mesh.data());
    }

    glViewport(0, 0, surface_width, surface_height);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    const bool video = video_state_ == 1 && video_texture_ != 0;
    for (GLuint program : {program_, mesh_program_, video ? video_program_ : 0u})
    {
        if (program == 0)
            continue;
        glUseProgram(program);
        glUniform4f(0, viewport.scale, viewport.offset_x, viewport.offset_y, 0.0f);
        glUniform2f(1, static_cast<float>(surface_width), static_cast<float>(surface_height));
    }
    if (video)
    {
        // The video program keeps its buffer on unit 7 for the whole frame.
        glUniform1i(9, 7);
        glUniform2i(10, video_width_, video_height_);
        glActiveTexture(GL_TEXTURE7);
        glBindTexture(GL_TEXTURE_BUFFER, video_texture_);
    }
    glUseProgram(program_);
    glUniform1i(2, 0);
    // Font atlases on units 1..4 for the whole frame; unit 0 changes per run.
    const GLint font_units[kFontSlots] = {1, 2, 3, 4, 5, 6};
    glUniform1iv(3, static_cast<GLsizei>(kFontSlots), font_units);
    for (std::uint32_t slot = 0; slot < font_count_; ++slot)
    {
        glActiveTexture(GL_TEXTURE1 + slot);
        glBindTexture(GL_TEXTURE_2D, font_textures_[slot]);
    }
    glActiveTexture(GL_TEXTURE0);

    GLuint bound_texture = 0;
    bool scissor = false;
    bool mesh_bound = false;
    GLuint current_program = program_;
    glBindVertexArray(vao_);
    for (const Run &run : list.runs())
    {
        if (run.count == 0)
            continue;
        const bool video_run = !run.mesh && run.texture == kVideoBufferHandle;
        if (video_run && !video)
            continue; // no picture in a buffer yet
        if (run.mesh != mesh_bound)
        {
            mesh_bound = run.mesh;
            glBindVertexArray(mesh_bound ? mesh_vao_ : vao_);
        }
        const GLuint wanted = run.mesh ? mesh_program_ : video_run ? video_program_ : program_;
        if (wanted != current_program)
        {
            glUseProgram(wanted);
            current_program = wanted;
        }
        // The video run samples unit 7; unit 0 keeps what it had.
        if (!video_run && run.texture != 0 && run.texture != bound_texture)
        {
            glBindTexture(GL_TEXTURE_2D, run.texture);
            bound_texture = run.texture;
        }
        if (run.clipped)
        {
            const float x0 = std::floor(run.clip.x * viewport.scale + viewport.offset_x);
            const float y0 = std::floor(run.clip.y * viewport.scale + viewport.offset_y);
            const float x1 =
                std::ceil((run.clip.x + run.clip.w) * viewport.scale + viewport.offset_x);
            const float y1 =
                std::ceil((run.clip.y + run.clip.h) * viewport.scale + viewport.offset_y);
            if (!scissor)
                glEnable(GL_SCISSOR_TEST);
            scissor = true;
            glScissor(static_cast<GLint>(x0), surface_height - static_cast<GLint>(y1),
                      static_cast<GLsizei>(std::max(0.0f, x1 - x0)),
                      static_cast<GLsizei>(std::max(0.0f, y1 - y0)));
        }
        else if (scissor)
        {
            glDisable(GL_SCISSOR_TEST);
            scissor = false;
        }
        if (run.mesh)
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(run.first),
                         static_cast<GLsizei>(run.count));
        else
            glDrawArraysInstancedBaseInstance(GL_TRIANGLES, 0, 6, static_cast<GLsizei>(run.count),
                                              run.first);
        ++draw_calls_;
    }
    if (scissor)
        glDisable(GL_SCISSOR_TEST);
    glBindVertexArray(0);
}

} // namespace hui::gfx
