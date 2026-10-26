// youtube-tv - The GL side of colour emoji: pictures decoded on first use.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/emoji_textures.hpp"

#include "core/save_file.hpp"
#include "gfx/emoji.hpp"
#include "gfx/gl_batch.hpp"
#include "yt/image.hpp"
#include "yt/log.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hui::app
{

namespace
{

gfx::GlBatch *g_batch = nullptr;
std::unordered_map<int, std::uint32_t> &textures()
{
    static std::unordered_map<int, std::uint32_t> map;
    return map;
}

// A picture decodes the first time it is drawn (a few dozen microseconds for
// 72 px) and stays: titles use a few dozen different emoji at most.
std::uint32_t texture_for(int id)
{
    auto &map = textures();
    if (const auto found = map.find(id); found != map.end())
        return found->second;
    std::uint32_t texture = 0;
    const std::string_view png = gfx::emoji::png(id);
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
    std::string error;
    if (g_batch != nullptr && !png.empty() &&
        yt::decode_image(std::string(png), &width, &height, &rgba, &error) && width > 0 &&
        height > 0)
        texture = g_batch->create_texture(width, height, rgba.data());
    map.emplace(id, texture); // a failure is not retried every frame
    return texture;
}

} // namespace

bool init_emoji(gfx::GlBatch &batch, const std::string &pack_path)
{
    g_batch = &batch;
    std::string data;
    if (!save::read_file(pack_path, &data, 16u << 20) || !gfx::emoji::load(std::move(data)))
    {
        yt::log::line("emoji: no pack at %s", pack_path.c_str());
        return false;
    }
    gfx::emoji::set_texture_function(&texture_for);
    yt::log::line("emoji: %zu pictures", gfx::emoji::count());
    return true;
}

} // namespace hui::app
