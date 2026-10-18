// youtube-tv - A YouTube channel, laid out as SmartTube does it.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/channel_screen.hpp"

#include "app/screens.hpp"
#include "app/theme.hpp"
#include "app/thumbs.hpp"
#include "yt/service.hpp"

#include <memory>

namespace hui::app
{

ChannelScreen::ChannelScreen(Context &context) : context_(context)
{
    rows_.set_bounds({kContentX, 250.0f, kContentW, 800.0f});
}

void ChannelScreen::enter()
{
    if (!context_.pending_channel.channel_id.empty())
    {
        channel_id_ = context_.pending_channel.channel_id;
        context_.pending_channel = {};
    }
    const yt::ChannelView view = context_.service.channel_view();
    if (!channel_id_.empty() &&
        (view.channel.channel_id != channel_id_ || view.state == yt::Load::failed))
        context_.service.request_channel(channel_id_);
    seen_revision_ = 0;
    seen_channel_revision_ = 0;
    rows_.set_items({}, context_);
}

void ChannelScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    if (input.is_pressed(Action::back))
    {
        context_.nav.leave_channel = true;
        return;
    }
    const std::uint64_t revision = context_.service.revision(yt::FeedKind::channel);
    if (revision != seen_revision_)
    {
        seen_revision_ = revision;
        rows_.set_items(context_.service.feed(yt::FeedKind::channel).items, context_);
    }
    rows_.set_active(true);
    if (rows_.handle(input, feedback) && rows_.has_activation())
    {
        // A video plays, a playlist opens its page, a channel opens here.
        context_.nav.open_video = true;
        context_.nav.video_id = rows_.activated();
        rows_.clear_activation();
    }
    rows_.update(dt, context_);
}

void ChannelScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    gfx::DrawList &list = canvas.list;
    const yt::ChannelView view = context.service.channel_view();
    // The header: SmartTube's channel picture, name and subscriber count.
    const float x = kContentX;
    const float avatar = 120.0f;
    const std::uint32_t texture = view.channel.thumbnail_url.empty()
                                      ? 0u
                                      : context.thumbs.texture(view.channel.thumbnail_url);
    if (texture != 0)
        list.image(texture, {x, 70.0f, avatar, avatar}, gfx::kFullUv, gfx::Color::rgb(0xffffff),
                   avatar * 0.5f);
    else
        list.circle(x + avatar * 0.5f, 70.0f + avatar * 0.5f, avatar * 0.5f,
                    gfx::Color::rgb(0x3a3a3a));
    const std::string name = view.channel.name.empty() ? std::string("Channel") : view.channel.name;
    if (context.fonts.semibold.font != nullptr)
        ui::text(list, context.fonts.semibold,
                 context.fonts.semibold.font->fit(name, 50.0f, kContentW - avatar - 420.0f),
                 x + avatar + 32.0f, 128.0f, 50.0f, gfx::Color::rgb(0xffffff));
    if (!view.channel.subscribers.empty())
        ui::text(list, context.fonts.regular, view.channel.subscribers, x + avatar + 32.0f, 172.0f,
                 28.0f, gfx::Color::rgb(0xaaaaaa));

    const yt::FeedBrief brief = context.service.feed_brief(yt::FeedKind::channel);
    if (brief.empty)
    {
        const bool failed = view.state == yt::Load::failed;
        ui::text(list, context.fonts.regular,
                 failed ? (view.error.empty() ? std::string("This channel didn't load")
                                              : "This channel didn't load: " + view.error)
                        : std::string("Loading..."),
                 kContentX + kContentW * 0.5f, 480.0f, 30.0f, gfx::Color::rgb(0xaaaaaa),
                 gfx::Align::center);
        return;
    }
    rows_.draw(canvas);
}

std::unique_ptr<Screen> make_channel_screen(Context &context)
{
    return std::make_unique<ChannelScreen>(context);
}

} // namespace hui::app
