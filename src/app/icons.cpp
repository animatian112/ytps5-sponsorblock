// youtube-tv - SmartTube's icons, loaded once.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/icons.hpp"

#include "core/save_file.hpp"
#include "gfx/gl_batch.hpp"
#include "yt/image.hpp"

#include <map>
#include <vector>

namespace hui::app
{
namespace
{

struct Loaded
{
    std::uint32_t texture = 0;
    int width = 0;
    int height = 0;
};

struct Registry
{
    gfx::GlBatch *batch = nullptr;
    std::string root;
    std::map<std::string, Loaded, std::less<>> loaded;
};

Registry &registry()
{
    static Registry instance;
    return instance;
}

const Loaded &load(std::string_view name)
{
    Registry &r = registry();
    const auto found = r.loaded.find(name);
    if (found != r.loaded.end())
        return found->second;
    Loaded entry;
    if (r.batch != nullptr)
    {
        std::string bytes;
        if (save::read_file(r.root + "/" + std::string(name) + ".png", &bytes, 1u << 20))
        {
            std::vector<std::uint8_t> rgba;
            std::string error;
            int width = 0;
            int height = 0;
            if (yt::decode_image(bytes, &width, &height, &rgba, &error))
            {
                entry.texture = r.batch->create_texture(width, height, rgba.data());
                entry.width = width;
                entry.height = height;
            }
        }
    }
    return r.loaded.emplace(std::string(name), entry).first->second;
}

} // namespace

void init_icons(gfx::GlBatch &batch, std::string root)
{
    Registry &r = registry();
    r.batch = &batch;
    r.root = std::move(root);
}

std::uint32_t icon_texture(std::string_view name)
{
    return load(name).texture;
}

void icon_size(std::string_view name, int *width, int *height)
{
    const Loaded &entry = load(name);
    *width = entry.width;
    *height = entry.height;
}

bool draw_icon(gfx::DrawList &list, std::string_view name, const gfx::Rect &box, gfx::Color tint)
{
    const std::uint32_t texture = icon_texture(name);
    if (texture == 0)
        return false;
    list.image(texture, box, gfx::kFullUv, tint);
    return true;
}

} // namespace hui::app
