// youtube-tv - The Glow layout: the focused video behind a large header.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/glow.hpp"

#include "app/feed_view.hpp"
#include "app/theme.hpp"

#include <algorithm>

namespace hui::app
{

namespace
{

// The page under everything: a deep blue-black, like the night the picture
// glows out of.
const gfx::Color kPage = gfx::Color::rgb(0x08070f);
const gfx::Color kAccent = gfx::Color::rgb(0xff4e45);
const gfx::Color kTitle = gfx::Color::rgb(0xffffff);
const gfx::Color kMeta = gfx::Color::rgb(0xd0cfe0);
constexpr float kRestS = 0.18f;   // the focus rests this long before the picture follows
constexpr float kFadePerS = 3.5f; // the pictures cross-fade in ~0.3 s
constexpr float kPictureAlpha = 0.62f;

float approach(float value, float target, float step)
{
    return value < target ? std::min(target, value + step) : std::max(target, value - step);
}

} // namespace

void style_cards(ui::CardLook &card, const Settings &settings)
{
    card.title_lines = settings.title_lines;
    if (glow_layout(settings))
    {
        // Rounded pictures, a white ring on the focused one, the words on the
        // page under them.
        card.info_box = false;
        card.ring = true;
        card.focus_scale = 1.0f;
        card.lift = 0.0f;
        card.shadow = false;
        card.radius = 14.0f;
        card.title_lines = std::max(2, settings.title_lines);
    }
    else
    {
        // SmartTube's cards: the words in a box that lights up with the focus.
        card.info_box = true;
        card.ring = false;
        card.focus_scale = 1.1f;
        card.lift = 6.0f;
        card.shadow = true;
        card.radius = settings.rounded_cards ? 6.0f : 0.0f;
    }
}

std::string GlowBackdrop::picture_url(const std::string &video_id)
{
    // The 1280x720 still: sharp enough to fill the screen behind the fades.
    return "https://i.ytimg.com/vi/" + video_id + "/hq720.jpg";
}

void GlowBackdrop::clear()
{
    current_ = Layer{};
    previous_ = Layer{};
    pending_id_.clear();
    has_item_ = false;
    hero_alpha_ = 0.0f;
}

void GlowBackdrop::update(const yt::FeedItem *focused, ThumbCache &thumbs, float dt)
{
    const std::string id =
        focused != nullptr && focused->video_id.size() == 11 ? focused->video_id : std::string();
    if (id != pending_id_)
    {
        pending_id_ = id;
        rest_s_ = 0.0f;
    }
    rest_s_ += dt;

    // The header follows the focus at once; only the picture waits.
    has_item_ = focused != nullptr;
    if (focused != nullptr)
    {
        title_ = focused->title;
        meta_ = focused->author;
        if (!focused->detail.empty())
            meta_ += (meta_.empty() ? "" : "  \xE2\x80\xA2  ") + focused->detail;
        if (focused->live)
            label_ = "LIVE NOW";
        else if (focused->watched >= 0.03f && focused->watched <= 0.95f)
            label_ = "CONTINUE WATCHING";
        else
            label_ = ui::upper(focused->shelf);
    }
    hero_alpha_ = approach(hero_alpha_, has_item_ ? 1.0f : 0.0f, dt * kFadePerS);

    if (!pending_id_.empty() && pending_id_ != current_.video_id && rest_s_ >= kRestS)
    {
        previous_ = current_;
        current_ = Layer{};
        current_.video_id = pending_id_;
    }
    // Every frame: the cache keeps a texture alive while it is asked for.
    for (Layer *layer : {&current_, &previous_})
    {
        if (layer->video_id.empty())
            continue;
        std::uint32_t texture = thumbs.texture(picture_url(layer->video_id));
        if (texture == 0)
        {
            yt::FeedItem item;
            item.video_id = layer->video_id;
            texture = thumbs.texture(card_thumbnail(item)); // the card's, until it arrives
        }
        layer->texture = texture;
    }
    current_.alpha = approach(current_.alpha, current_.texture != 0 ? 1.0f : 0.0f, dt * kFadePerS);
    previous_.alpha = approach(previous_.alpha, 0.0f, dt * kFadePerS);
    if (previous_.alpha <= 0.0f)
        previous_ = Layer{};
}

void GlowBackdrop::draw_backdrop(gfx::DrawList &list) const
{
    const gfx::Rect screen{0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight};
    list.rounded_rect(screen, 0.0f, kPage);
    for (const Layer *layer : {&previous_, &current_})
    {
        if (layer->texture != 0 && layer->alpha > 0.0f)
            list.image(layer->texture, screen, gfx::kFullUv,
                       gfx::Color::rgb(0xffffff, layer->alpha * kPictureAlpha), 0.0f);
    }
    // The picture fades into the page: from the left, where the header
    // stands, from the bottom, where the rows are, and a little from the top.
    list.gradient_rect_h({0.0f, 0.0f, 1500.0f, gfx::kVirtualHeight}, 0.0f, kPage.with_alpha(0.96f),
                         kPage.with_alpha(0.0f));
    list.gradient_rect({0.0f, 360.0f, gfx::kVirtualWidth, 720.0f}, 0.0f, kPage.with_alpha(0.0f),
                       kPage.with_alpha(0.97f));
    list.gradient_rect({0.0f, 0.0f, gfx::kVirtualWidth, 180.0f}, 0.0f, kPage.with_alpha(0.55f),
                       kPage.with_alpha(0.0f));
}

void GlowBackdrop::draw_hero(gfx::DrawList &list, const ui::Fonts &fonts) const
{
    if (!has_item_ || hero_alpha_ <= 0.0f || fonts.display.font == nullptr)
        return;
    list.push_opacity(hero_alpha_);
    const float x = kContentX;
    const float width = 1180.0f;
    float y = 186.0f;
    if (!label_.empty())
    {
        ui::text(list, fonts.semibold, label_, x, y, 22.0f, kAccent, gfx::Align::left, 3.0f);
        y += 64.0f;
    }
    else
        y += 40.0f;
    y = ui::paragraph(list, fonts.display, title_, x, y, 58.0f, width, 66.0f, kTitle, 2);
    if (!meta_.empty())
        ui::text(list, fonts.regular, fonts.regular.font->fit(meta_, 28.0f, width), x, y + 4.0f,
                 28.0f, kMeta);
    list.pop_opacity();
}

} // namespace hui::app
