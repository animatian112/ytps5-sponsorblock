// youtube-tv - Search: a query field, the on-screen keyboard and the results
// grid. The field never suggests anything (no network while typing); a search
// runs only when the keyboard's Done key fires. Confirming one of the channel
// cards the answer carried opens that channel's page - header, tabs and the
// videos of the selected tab - in place of the query answer.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/search_screen.hpp"

#include "app/theme.hpp"
#include "app/thumbs.hpp"
#include "yt/service.hpp"
#include "ui/components.hpp"

#include <string_view>
#include <vector>

namespace hui::app
{
namespace
{

const TourStep kTour[] = {
    {1.2f, 0, Direction::none, "field"},
    {0.8f, hui::action_bit(Action::confirm), Direction::none, nullptr},
    {1.0f, 0, Direction::none, "keyboard"},
    {0.6f, hui::action_bit(Action::confirm), Direction::none, nullptr},
    // After the query has run: the channel row the answer carried.
    {1.0f, 0, Direction::none, "channels"},
    // Circle closes the keyboard, down walks onto the channel row, confirm
    // opens the first card. The snapshot fixture injects that channel's page.
    {0.6f, hui::action_bit(Action::back), Direction::none, nullptr},
    {0.6f, 0, Direction::down, nullptr},
    {0.8f, hui::action_bit(Action::confirm), Direction::none, nullptr},
    {1.0f, 0, Direction::none, "channel"},
    // Down into the grid and back up: the grid walks its own rows and only
    // hands the focus over when it runs off the top one.
    {0.6f, 0, Direction::down, nullptr},
    {0.6f, 0, Direction::up, nullptr},
    // Up to the tab bar; left and right turn the tabs now (L1/R1 are the
    // app's section keys again).
    {0.6f, 0, Direction::up, nullptr},
    {0.5f, 0, Direction::right, nullptr},
    {1.0f, 0, Direction::none, "channel-tab"},
    // Down again: the grid returns to the cell it was left on.
    {0.6f, 0, Direction::down, nullptr},
    {1.0f, 0, Direction::none, "channel-grid"},
};

const gfx::Color kInk = gfx::Color::rgb(0xf1f1f1);
const gfx::Color kMuted = gfx::Color::rgb(0xaaaaaa);
const gfx::Color kWell = gfx::Color::rgb(0x272727);

// The content column, and the room the channel row and the results grid share.
constexpr float kChannelRowY = 230.0f;
constexpr float kChannelRowH = 200.0f;
constexpr float kResultsY = 300.0f;
constexpr float kResultsH = 660.0f;

// A channel page: the header under the field, its tab bar, then the grid.
constexpr float kAvatarX = kContentX;
constexpr float kAvatarY = 236.0f;
constexpr float kAvatarSize = 96.0f;
constexpr float kChannelNameX = kAvatarX + kAvatarSize + 28.0f;
constexpr float kChannelNameBaseline = 288.0f;
constexpr float kChannelSubsBaseline = 320.0f;
constexpr float kChannelTabY = 360.0f;
constexpr float kChannelTabH = 60.0f;
constexpr float kChannelResultsY = 452.0f;
constexpr float kChannelResultsH = 560.0f;

} // namespace

SearchScreen::SearchScreen(Context &context) : context_(context)
{
    field_.style.theme = youtube_theme();
    field_.set_placeholder("Search videos");
    // provider stays empty: no suggestions from the network.
    field_.set_bounds({kContentX, 140.0f, kContentW, 64.0f});
    field_.set_active(true);

    // The search filters (Settings > Search keeps the same values).
    filters_.style.theme = youtube_theme();
    filters_.style.panel = true;
    filters_.style.on_page = false;
    filters_.set_bounds({1120.0f, 230.0f, 720.0f, 330.0f});
    filters_.add_header("Search filters");
    filters_.add_choice(1, "Sort by", {"Relevance", "Upload date", "View count", "Rating"},
                        context_.settings.search_sort);
    filters_.add_choice(2, "Upload date",
                        {"Any time", "Last hour", "Today", "This week", "This month", "This year"},
                        context_.settings.search_date);
    filters_.add_choice(3, "Length", {"Any", "Under 4 minutes", "4-20 minutes", "Over 20 minutes"},
                        context_.settings.search_duration);

    // The PlayStation's own keyboard: a full-width dark band at the bottom,
    // grey keys, the focused one white with dark text, Done a lighter key; Square
    // deletes, Triangle types a space, L2 shifts, R2 switches to symbols.
    static const ui::Theme kPs5Keys = []
    {
        ui::Theme theme = youtube_theme();
        theme.surface = gfx::Color::rgb(0x3d3d3d);
        theme.surface_high = gfx::Color::rgb(0x3d3d3d);
        theme.secondary = gfx::Color::rgb(0x3d3d3d);
        theme.text = gfx::Color::rgb(0xffffff);
        theme.accent = gfx::Color::rgb(0xffffff);
        theme.focus = gfx::Color::rgb(0xffffff);
        theme.primary = gfx::Color::rgb(0x9a9a9a);    // the Done key, lighter than the rest
        theme.on_primary = gfx::Color::rgb(0x111111); // dark text on the white focus
        theme.radius = 8.0f;
        return theme;
    }();
    keys_.style.theme = kPs5Keys;
    keys_.style.panel = false;
    keys_.style.highlight.color = gfx::Color::rgb(0xffffff);
    keys_.style.highlight.breathe = false;
    keys_.style.gap = 10.0f;
    keys_.style.label_size = 32.0f;
    keys_.style.bindings = ui::KeyboardBindings::standard();
    {
        using ui::KeyboardKey;
        using ui::KeyKind;
        ui::KeyboardLayout letters;
        letters.name = "abc";
        letters.columns = 11;
        letters.add_row("1234567890");
        letters.rows.back().push_back({KeyKind::backspace, {}, {}, 1, {}});
        letters.add_row("qwertyuiop-");
        letters.add_row("asdfghjkl'&");
        letters.rows[2][9].shifted = "\"";
        letters.add_row(std::vector<KeyboardKey>{{KeyKind::shift, {}, {}, 1, {}}});
        for (const char ch : std::string("zxcvbnm,.?"))
        {
            KeyboardKey key;
            key.text = std::string(1, ch);
            if (ch >= 'a' && ch <= 'z')
                key.shifted = std::string(1, static_cast<char>(ch - 'a' + 'A'));
            letters.rows.back().push_back(std::move(key));
        }
        letters.add_row(std::vector<KeyboardKey>{{KeyKind::layout, {}, {}, 2, "@#:"},
                                                 {KeyKind::space, {}, {}, 6, {}},
                                                 {KeyKind::done, {}, {}, 3, {}}});
        ui::KeyboardLayout symbols = ui::KeyboardLayout::symbols();
        symbols.name = "@#:";
        keys_.set_layouts({letters, symbols});
    }
    keys_.set_bounds({300.0f, 640.0f, 1320.0f, 410.0f});
    // Up from the top row leaves for the suggestion row above it.
    keys_.style.exits.up = true;
    keys_.on_text = [this](std::string_view text) { field_.insert(text); };
    keys_.on_backspace = [this] { field_.backspace(); };
    keys_.on_done = [this] { run_search(); };

    results_.grid().style.theme = card_theme();
    results_.set_bounds({kContentX, kResultsY, kContentW, kResultsH});
    results_.set_empty_text("No results");
    results_.set_active(false);

    // Channel avatars: square art, the name and the subscriber line under it.
    channels_.style.theme = youtube_theme();
    channels_.style.mode = ui::CarouselMode::leading;
    channels_.style.item_width = 104.0f;
    channels_.style.gap = 24.0f;
    channels_.style.card.art_aspect = 1.0f;
    channels_.style.card.text = ui::CardText::below;
    channels_.style.card.title_size = 22.0f;
    channels_.style.card.subtitle_size = 18.0f;
    channels_.style.card.ring = true;
    channels_.style.counter = false;
    channels_.style.dots = false;
    channels_.set_bounds({kContentX, kChannelRowY, kContentW, kChannelRowH});
    channels_.set_active(false);

    // The tab bar of an open channel page; only its titles show.
    tabs_.style.theme = youtube_theme();
    tabs_.style.kind = ui::TabKind::underline;
    tabs_.style.on_page = true;
    tabs_.set_bounds({kContentX, kChannelTabY, kContentW, kChannelTabH});
    tabs_.set_focused(false);

    seen_revision_ = context_.service.search_revision();
    refresh_results();
}

void SearchScreen::enter()
{
    if (!context_.pending_channel.channel_id.empty())
    {
        // Sent here by the player's Channel button: open that page directly.
        const yt::ChannelItem channel = context_.pending_channel;
        context_.pending_channel = {};
        open_channel_item(channel);
        channel_external_ = true;
        return;
    }
    // A fresh visit starts on the query answer, whatever the last one opened.
    in_channel_ = false;
    channel_external_ = false;
    spot_ = Spot::field;
    grid_focus_ = 0;
    grid_scrolled_ = false;
    seen_revision_ = context_.service.search_revision();
    refresh_results();
    field_.set_active(true);
    results_.set_active(false);
    channels_.set_active(false);
    tabs_.set_focused(false);
    // Settings > Search > Show keyboard automatically.
    if (context_.settings.keyboard_auto)
    {
        keys_.enter();
        spot_ = Spot::keyboard;
    }
}

std::string SearchScreen::filter_summary() const
{
    static const char *const kSort[] = {"", "Upload date", "View count", "Rating"};
    static const char *const kDate[] = {"",          "Last hour",  "Today",
                                        "This week", "This month", "This year"};
    static const char *const kLength[] = {"", "Under 4 min", "4-20 min", "Over 20 min"};
    const Settings &s = context_.settings;
    std::string out;
    for (const char *part :
         {kSort[std::clamp(s.search_sort, 0, 3)], kDate[std::clamp(s.search_date, 0, 5)],
          kLength[std::clamp(s.search_duration, 0, 3)]})
        if (*part != '\0')
            out += (out.empty() ? "" : "  \xE2\x80\xA2  ") + std::string(part);
    return out;
}

void SearchScreen::update_filters(const InputFrame &input, ui::Feedback &feedback)
{
    Settings &s = context_.settings;
    const ui::Event event = filters_.handle(input, feedback);
    if (event == ui::Event::changed)
    {
        s.search_sort = filters_.choice_index(1);
        s.search_date = filters_.choice_index(2);
        s.search_duration = filters_.choice_index(3);
        context_.settings_changed = true;
    }
    if (event == ui::Event::cancelled || input.is_pressed(Action::north))
    {
        filters_open_ = false;
        filters_.set_active(false);
        // The service takes the new filters with the next frame's
        // preferences; the search goes out after that.
        const int now = s.search_sort * 100 + s.search_date * 10 + s.search_duration;
        if (now != filters_before_ && has_searched_)
            search_again_ = true;
    }
    filters_.update(1.0f / 60.0f);
}

void SearchScreen::build_chips()
{
    chips_.clear();
    chip_recent_.clear();
    const std::string &text = asked_;
    const auto lower = [](std::string value)
    {
        for (char &ch : value)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return value;
    };
    const std::string needle = lower(text);
    const auto add = [&](const std::string &chip, bool recent)
    {
        if (chip.empty() || chips_.size() >= 8)
            return;
        const std::string key = lower(chip);
        for (const std::string &have : chips_)
            if (lower(have) == key)
                return;
        chips_.push_back(chip);
        chip_recent_.push_back(recent);
    };
    // The account's own searches first: the ones that start with what is
    // typed (all of the newest with nothing typed).
    if (context_.settings.search_history)
    {
        int taken = 0;
        for (const std::string &past : context_.service.search_history())
        {
            if (taken >= (needle.empty() ? 8 : 3))
                break;
            if (needle.empty() || lower(past).rfind(needle, 0) == 0)
            {
                add(past, true);
                ++taken;
            }
        }
    }
    // Then YouTube's completions of it.
    std::string answered;
    const std::vector<std::string> found = suggest_.answer(&answered);
    if (!text.empty() && lower(answered) == needle)
        for (const std::string &completion : found)
            add(completion, false);
    chip_focus_ = std::clamp(chip_focus_, 0, std::max(0, static_cast<int>(chips_.size()) - 1));
}

void SearchScreen::update_suggestions(float dt)
{
    // The row follows the typing; while it has the focus it fills the field
    // itself and stays as it is.
    if (spot_ == Spot::suggestions)
        return;
    const std::string &text = field_.text();
    if (text != asked_)
    {
        asked_ = text;
        ask_wait_ = 0.18f;
        build_chips(); // the account's searches show at once
    }
    if (ask_wait_ > 0.0f)
    {
        ask_wait_ -= dt;
        if (ask_wait_ <= 0.0f)
        {
            const int region = std::clamp(context_.settings.region, 0, Settings::kRegionCount - 1);
            suggest_.set_region(Settings::kRegions[region][1], Settings::kRegions[region][0]);
            suggest_.ask(asked_);
        }
    }
    if (suggest_.revision() != suggest_seen_)
    {
        suggest_seen_ = suggest_.revision();
        build_chips();
    }
}

void SearchScreen::update_chips(const InputFrame &input, ui::Feedback &feedback)
{
    const int count = static_cast<int>(chips_.size());
    if (count == 0 || input.is_pressed(Action::back))
    {
        // Circle: back to typing, with what was typed.
        field_.set_text(typed_);
        asked_ = typed_;
        spot_ = Spot::keyboard;
        if (count > 0)
            feedback.play(audio::Cue::back);
        return;
    }
    if (input.nav == Direction::left || input.nav == Direction::right)
    {
        const int next = chip_focus_ + (input.nav == Direction::right ? 1 : -1);
        if (next >= 0 && next < count)
        {
            chip_focus_ = next;
            field_.set_text(chips_[static_cast<std::size_t>(next)]);
            feedback.play(audio::Cue::focus);
        }
        else
            feedback.play(audio::Cue::error);
        return;
    }
    if (input.nav == Direction::down)
    {
        // Back to the keys with the suggestion in the field: typing goes on
        // from it.
        asked_ = field_.text();
        spot_ = Spot::keyboard;
        feedback.play(audio::Cue::focus);
        return;
    }
    if (input.is_pressed(Action::confirm))
    {
        feedback.play(audio::Cue::select);
        run_search();
    }
}

void SearchScreen::draw_chips(ui::Canvas &canvas, const Context &context) const
{
    if (chips_.empty() || context.fonts.regular.font == nullptr)
        return;
    const bool active = spot_ == Spot::suggestions;
    constexpr float kY = 548.0f;
    constexpr float kHeight = 50.0f;
    constexpr float kSize = 24.0f;
    constexpr float kPad = 22.0f;
    constexpr float kGap = 14.0f;
    constexpr float kLeft = 300.0f;
    constexpr float kRight = 1620.0f;
    // The row scrolls so the focused chip is on screen.
    std::vector<float> widths;
    for (const std::string &chip : chips_)
        widths.push_back(std::min(520.0f, context.fonts.regular.measure(chip, kSize)) +
                         kPad * 2.0f);
    float shift = 0.0f;
    {
        float x = kLeft;
        for (int i = 0; i <= chip_focus_ && i < static_cast<int>(widths.size()); ++i)
        {
            const float end = x + widths[static_cast<std::size_t>(i)];
            if (i == chip_focus_ && end - shift > kRight)
                shift = end - kRight;
            x = end + kGap;
        }
    }
    float x = kLeft - shift;
    for (std::size_t i = 0; i < chips_.size(); ++i)
    {
        const float w = widths[i];
        if (x + w > kLeft - 1.0f && x < kRight + 1.0f)
        {
            const bool focused = active && static_cast<int>(i) == chip_focus_;
            canvas.list.rounded_rect({x, kY, w, kHeight}, kHeight * 0.5f,
                                     focused ? gfx::Color::rgb(0xffffff)
                                             : gfx::Color::rgb(0x3d3d3d, 0.95f));
            std::string label = chips_[i];
            while (label.size() > 1 &&
                   context.fonts.regular.measure(label, kSize) > w - kPad * 2.0f + 0.5f)
                label.pop_back();
            ui::text(canvas.list, context.fonts.regular, label, x + kPad, kY + 33.0f, kSize,
                     focused           ? gfx::Color::rgb(0x111111)
                     : chip_recent_[i] ? gfx::Color::rgb(0xb9c7ff)
                                       : gfx::Color::rgb(0xf1f1f1));
        }
        x += w + kGap;
    }
    if (!active)
        ui::text(canvas.list, context.fonts.regular, "Up: suggestions", kRight, kY - 12.0f, 18.0f,
                 gfx::Color::rgb(0x9a9a9a), gfx::Align::right);
}

void SearchScreen::run_search()
{
    has_searched_ = true;
    if (context_.settings.search_history)
        context_.service.remember_search(field_.text());
    in_channel_ = false;
    spot_ = Spot::field;
    searching_ = true;
    more_requested_ = false;
    context_.service.request_search(field_.text());
    results_.set_items(std::vector<yt::FeedItem>{}, context_);
    results_.set_state(yt::Load::loading, std::string());
    results_.set_empty_text("No results");
}

void SearchScreen::refresh_results()
{
    const yt::FeedView view = context_.service.search_feed();
    results_.set_items(view.items, context_);
    results_.set_state(view.state, view.error);
    results_.set_empty_text("No results");
    searching_ = view.state == yt::Load::loading;
    more_requested_ = false;

    // The channels the same answer carried become the row above the grid.
    channel_items_ = view.channels;
    std::vector<ui::CardItem> cards;
    cards.reserve(channel_items_.size());
    for (const yt::ChannelItem &channel : channel_items_)
    {
        ui::CardItem card;
        card.title = channel.name.empty() ? channel.channel_id : channel.name;
        card.subtitle = channel.subscribers;
        card.texture = context_.thumbs.texture(channel.thumbnail_url);
        card.image_aspect = 1.0f;
        cards.push_back(std::move(card));
    }
    channels_.set_items(std::move(cards));
    channels_.set_bounds({kContentX, kChannelRowY, kContentW, kChannelRowH});

    // The row sits under the field: the grid gives up its height.
    const bool row = !channel_items_.empty();
    results_.set_bounds({kContentX, row ? kResultsY + kChannelRowH : kResultsY, kContentW,
                         row ? kResultsH - kChannelRowH : kResultsH});
}

void SearchScreen::refresh_channel()
{
    const yt::ChannelView view = context_.service.channel_view();
    const bool mine = view.channel.channel_id == channel_id_;
    channel_state_ = view.state;
    channel_header_ = mine ? view.channel : yt::ChannelItem{};

    tabs_.set_bounds({kContentX, kChannelTabY, kContentW, kChannelTabH});
    if (!mine)
    {
        // The service still holds the previous page (or none): show nothing
        // until this channel's own answer lands.
        tabs_.set_tabs({});
        return;
    }

    std::vector<ui::TabItem> items;
    items.reserve(view.tabs.size());
    for (const yt::ChannelTab &tab : view.tabs)
    {
        ui::TabItem item;
        item.label = tab.title;
        items.push_back(std::move(item));
    }
    tabs_.set_tabs(std::move(items));
    if (view.selected_tab >= 0 && view.selected_tab < static_cast<int>(view.tabs.size()))
        tabs_.set_active(view.selected_tab, true);
}

void SearchScreen::refresh_channel_feed()
{
    const yt::FeedView view = context_.service.feed(yt::FeedKind::channel);
    results_.set_items(view.items, context_);
    results_.set_state(view.state, view.error);
    results_.set_empty_text("No videos");
    results_.set_bounds({kContentX, kChannelResultsY, kContentW, kChannelResultsH});
    more_requested_ = false;
}

void SearchScreen::open_channel(std::size_t index)
{
    if (index >= channel_items_.size())
        return;
    // The channel's own page, not a page inside the search.
    context_.pending_channel = channel_items_[index];
    context_.nav.open_channel = true;
}

bool SearchScreen::uses_back() const
{
    return in_channel_ || filters_open_ || spot_ != Spot::field;
}

void SearchScreen::open_channel_item(const yt::ChannelItem &channel)
{
    in_channel_ = true;
    spot_ = Spot::results;
    more_requested_ = false;
    channel_id_ = channel.channel_id;
    channel_avatar_ = 0;
    grid_focus_ = 0;
    grid_scrolled_ = false;

    // A page already on screen for this channel is kept: confirming the same
    // card again must not throw it away. Any other page is replaced.
    const yt::ChannelView current = context_.service.channel_view();
    const bool loaded =
        current.state != yt::Load::loading && current.channel.channel_id == channel.channel_id;
    if (!loaded)
    {
        context_.service.request_channel(channel.channel_id);
        // Drop the previous channel's videos; the answer replaces them.
        results_.set_items(std::vector<yt::FeedItem>{}, context_);
        results_.set_state(yt::Load::loading, std::string());
        results_.set_empty_text("No videos");
        results_.set_bounds({kContentX, kChannelResultsY, kContentW, kChannelResultsH});
    }

    seen_channel_revision_ = context_.service.channel_revision();
    seen_channel_feed_revision_ = context_.service.revision(yt::FeedKind::channel);
    refresh_channel();
    if (loaded)
        refresh_channel_feed();
    results_.enter();
}

void SearchScreen::back_to_results()
{
    // Circle leaves the channel page; the query answer is still in the feed.
    in_channel_ = false;
    spot_ = Spot::results;
    grid_focus_ = 0;
    grid_scrolled_ = false;
    seen_revision_ = context_.service.search_revision();
    refresh_results();
    results_.enter();
}

void SearchScreen::remember_grid()
{
    // A grid with no cells has no row to come back to.
    grid_focus_ = results_.grid().count() == 0 ? 0 : results_.focus_index();
    // A focus below the first row means the grid had scrolled: coming back
    // down must put that row into view again, not just keep the number.
    grid_scrolled_ = grid_focus_ >= results_.grid().columns();
}

bool SearchScreen::channel_loading() const
{
    return channel_state_ == yt::Load::loading;
}

void SearchScreen::update_search(const InputFrame &input, ui::Feedback &feedback)
{
    // The channel row disappeared with the answer: hand the focus on.
    if (spot_ == Spot::channels && channel_items_.empty())
        spot_ = has_searched_ ? Spot::results : Spot::field;

    switch (spot_)
    {
    case Spot::keyboard:
        if (keys_.handle(input, feedback) == ui::Event::cancelled)
            spot_ = Spot::field;
        else if (keys_.exit() == Direction::up && !chips_.empty())
        {
            // Up out of the keys: the suggestion row, its first chip in the
            // field.
            typed_ = field_.text();
            chip_focus_ = 0;
            field_.set_text(chips_.front());
            spot_ = Spot::suggestions;
            feedback.play(audio::Cue::focus);
        }
        break;
    case Spot::suggestions:
        update_chips(input, feedback);
        break;
    case Spot::channels:
    {
        const ui::Event event = channels_.handle(input, feedback);
        if (event == ui::Event::activated)
        {
            const int index = channels_.focus();
            if (index >= 0)
                open_channel(static_cast<std::size_t>(index));
        }
        else if (event == ui::Event::cancelled)
        {
            spot_ = Spot::field;
        }
        else if (input.nav == Direction::up)
        {
            spot_ = Spot::field;
        }
        else if (input.nav == Direction::down)
        {
            spot_ = has_searched_ ? Spot::results : Spot::field;
        }
        break;
    }
    case Spot::results:
        if (input.is_pressed(Action::back))
        {
            spot_ = Spot::field;
        }
        else if (results_.handle(input, feedback, context_) && results_.has_activation())
        {
            context_.nav.open_video = true;
            context_.nav.video_id = results_.activated().video_id;
            results_.clear_activation();
        }
        break;
    default: // the field
        if (field_.handle(input, feedback) == ui::Event::activated &&
            field_.focus() == ui::SearchField::kFocusField)
        {
            keys_.enter();
            spot_ = Spot::keyboard;
        }
        else if (input.nav == Direction::down)
        {
            if (!channel_items_.empty())
            {
                channels_.enter();
                spot_ = Spot::channels;
            }
            else if (has_searched_)
            {
                spot_ = Spot::results;
            }
        }
        break;
    }
}

void SearchScreen::update_channel(const InputFrame &input, ui::Feedback &feedback)
{
    // Nothing owns the focus while the page is on its way: no press lands.
    if (channel_loading())
    {
        // Circle still leaves a page that is taking its time.
        if (input.is_pressed(Action::back))
        {
            if (channel_external_)
                context_.nav.leave_channel = true;
            else
                back_to_results();
        }
        return;
    }

    // Circle anywhere leaves the page for the answer that opened it. The
    // keyboard, while it is up, gets it first (it closes itself).
    if (spot_ != Spot::keyboard && input.is_pressed(Action::back))
    {
        if (channel_external_)
            context_.nav.leave_channel = true;
        else
            back_to_results();
        return;
    }

    switch (spot_)
    {
    case Spot::keyboard:
        if (keys_.handle(input, feedback) == ui::Event::cancelled)
            spot_ = Spot::field;
        break;
    case Spot::tabs:
    {
        const ui::Event event = tabs_.handle(input, feedback);
        if (event == ui::Event::changed)
            context_.service.request_channel_tab(tabs_.active());
        else if (input.nav == Direction::up)
            spot_ = Spot::field;
        else if (input.nav == Direction::down)
        {
            spot_ = Spot::results;
            // Back into the grid where it was left: a focus that had scrolled
            // below the first row is put back into view.
            if (grid_scrolled_)
                results_.set_focus(grid_focus_);
            grid_scrolled_ = false;
        }
        break;
    }
    case Spot::results:
    {
        const bool handled = results_.handle(input, feedback, context_);
        if (handled && results_.has_activation())
        {
            context_.nav.open_video = true;
            context_.nav.video_id = results_.activated().video_id;
            results_.clear_activation();
        }
        // The grid hands the focus up when it has run off its top row - or
        // when it holds no cells at all, so an empty tab is not a trap: the
        // tab bar takes the focus and the grid remembers the cell it was left
        // on for the way down.
        else if (input.nav == Direction::up &&
                 (results_.grid().count() == 0 || results_.grid().exit() == Direction::up))
        {
            remember_grid();
            spot_ = Spot::tabs;
        }
        break;
    }
    default: // the field
        if (input.nav == Direction::down)
        {
            spot_ = Spot::tabs;
        }
        else if (field_.handle(input, feedback) == ui::Event::activated &&
                 field_.focus() == ui::SearchField::kFocusField)
        {
            keys_.enter();
            spot_ = Spot::keyboard;
        }
        break;
    }
}

void SearchScreen::update(const InputFrame &input, float dt, ui::Feedback &feedback)
{
    clock_ += dt;
    keys_.set_length(field_.length());

    if (in_channel_)
    {
        // The page answers on two revisions: the header and tabs with one, the
        // videos of the selected tab with the other.
        const std::uint64_t revision = context_.service.channel_revision();
        if (revision != seen_channel_revision_)
        {
            seen_channel_revision_ = revision;
            refresh_channel();
        }
        const std::uint64_t feed_revision = context_.service.revision(yt::FeedKind::channel);
        if (feed_revision != seen_channel_feed_revision_)
        {
            seen_channel_feed_revision_ = feed_revision;
            refresh_channel_feed();
        }
        update_channel(input, feedback);
    }
    else
    {
        const std::uint64_t revision = context_.service.search_revision();
        if (revision != seen_revision_)
        {
            seen_revision_ = revision;
            refresh_results();
        }
        if (search_again_)
        {
            search_again_ = false;
            run_search();
        }
        update_suggestions(dt);
        if (filters_open_)
        {
            update_filters(input, feedback);
            return;
        }
        if (spot_ != Spot::keyboard && input.is_pressed(Action::north))
        {
            // Triangle: the filter panel (on the keyboard it types a space).
            const Settings &s = context_.settings;
            filters_.set_choice(1, s.search_sort);
            filters_.set_choice(2, s.search_date);
            filters_.set_choice(3, s.search_duration);
            filters_before_ = s.search_sort * 100 + s.search_date * 10 + s.search_duration;
            filters_open_ = true;
            filters_.enter();
            filters_.set_focus(1);
            filters_.set_active(true);
            return;
        }
        update_search(input, feedback);
    }

    // The avatars arrive asynchronously, like the grid's thumbnails.
    for (int index = 0; index < static_cast<int>(channel_items_.size()) &&
                        index < static_cast<int>(channels_.items().size());
         ++index)
    {
        ui::CardItem &card = channels_.item(index);
        if (card.texture == 0)
        {
            const std::uint32_t texture = context_.thumbs.texture(
                channel_items_[static_cast<std::size_t>(index)].thumbnail_url);
            if (texture != 0)
                card.texture = texture;
        }
    }
    if (in_channel_ && channel_avatar_ == 0 && !channel_header_.thumbnail_url.empty())
        channel_avatar_ = context_.thumbs.texture(channel_header_.thumbnail_url);

    // Pagination: ask for the next page once the focused cell sits in the last
    // row, as long as the feed still has a continuation. Only the query answer
    // has one the service can follow.
    if (!in_channel_)
    {
        const yt::FeedBrief view = context_.service.feed_brief(yt::FeedKind::search);
        if (!more_requested_ && view.state == yt::Load::ready && view.more &&
            results_.at_last_row())
        {
            more_requested_ = true;
            context_.service.request_more(yt::FeedKind::search);
        }
    }

    const bool live = !(in_channel_ && channel_loading());
    field_.set_busy(searching_ || (in_channel_ && channel_loading()));
    field_.set_active(live && spot_ == Spot::field);
    keys_.set_active(live && spot_ == Spot::keyboard);
    results_.set_active(live && spot_ == Spot::results);
    channels_.set_active(live && spot_ == Spot::channels);
    tabs_.set_focused(live && spot_ == Spot::tabs);

    field_.update(dt);
    keys_.update(dt);
    results_.update(dt, context_);
    channels_.update(dt);
    tabs_.update(dt);
}

void SearchScreen::draw_channel(ui::Canvas &canvas, const Context &context) const
{
    if (channel_header_.name.empty())
    {
        if (channel_state_ == yt::Load::loading)
            ui::text(canvas.list, context.fonts.regular, "Loading channel...", kChannelNameX,
                     kChannelNameBaseline, 24.0f, kMuted);
        return;
    }

    // Header: the avatar, the channel name and the subscriber line.
    const gfx::Rect avatar{kAvatarX, kAvatarY, kAvatarSize, kAvatarSize};
    if (channel_avatar_ != 0)
        canvas.list.image(channel_avatar_, avatar, gfx::kFullUv, gfx::Color::rgb(0xffffff),
                          kAvatarSize * 0.5f);
    else
        canvas.list.rounded_rect(avatar, kAvatarSize * 0.5f, kWell);
    ui::text(canvas.list, context.fonts.semibold, channel_header_.name, kChannelNameX,
             kChannelNameBaseline, 30.0f, kInk);
    ui::text(canvas.list, context.fonts.regular, channel_header_.subscribers, kChannelNameX,
             kChannelSubsBaseline, 20.0f, kMuted);

    // The tabs of the page, and the note that its videos are still coming.
    tabs_.draw(canvas);
    if (channel_state_ == yt::Load::loading)
        ui::text(canvas.list, context.fonts.regular, "Loading channel...", kContentX,
                 kChannelResultsY - 14.0f, 22.0f, kMuted);
}

void SearchScreen::draw(const Context &context, Frame &frame) const
{
    ui::Canvas canvas{frame.scene, context.fonts, frame.glass_texture, clock_};
    field_.draw(canvas);
    if (in_channel_)
    {
        draw_channel(canvas, context);
    }
    else if (!channel_items_.empty())
    {
        channels_.draw(canvas);
    }
    results_.draw(canvas, context);

    if (!in_channel_)
    {
        // Which filters the next search carries, above the field.
        const std::string summary = filter_summary();
        const std::string line = summary.empty() ? std::string("Triangle: search filters")
                                                 : "Filters: " + summary + "   (Triangle)";
        ui::text(canvas.list, context.fonts.regular, line.c_str(), kContentX, 122.0f, 20.0f,
                 summary.empty() ? gfx::Color::rgb(0x777777) : gfx::Color::rgb(0xcfcfcf));
    }
    if (filters_open_)
    {
        ui::Canvas overlay{frame.overlay, context.fonts, frame.glass_texture, clock_};
        overlay.list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                                  gfx::Color::rgb(0x000000, 0.45f));
        filters_.draw(overlay);
        return;
    }

    if (spot_ != Spot::keyboard && spot_ != Spot::suggestions)
        return;

    frame.glass = true;
    ui::Canvas overlay{frame.overlay, context.fonts, frame.glass_texture, clock_};
    overlay.list.rounded_rect({0.0f, 0.0f, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0.0f,
                              gfx::Color::rgb(0x000000, 0.5f));
    if (spot_ == Spot::keyboard || spot_ == Spot::suggestions)
    {
        // The PlayStation keyboard's band behind the keys.
        overlay.list.rounded_rect({0.0f, 612.0f, gfx::kVirtualWidth, gfx::kVirtualHeight - 612.0f},
                                  0.0f, gfx::Color::rgb(0x1a1a1a, 0.97f));
        overlay.list.rounded_rect({0.0f, 612.0f, gfx::kVirtualWidth, 2.0f}, 0.0f,
                                  gfx::Color::rgb(0x3d3d3d));
    }
    keys_.draw(overlay);
    // The field above the dimmed page, so a filled-in suggestion reads, and
    // the suggestion row just over the keys.
    field_.draw(overlay);
    draw_chips(overlay, context);
}

std::span<const TourStep> SearchScreen::tour() const
{
    return kTour;
}

bool SearchScreen::wants_rail_focus() const
{
    // Only the results grid is a vertical list here; the field, the horizontal
    // channel row and the tab bar never hand the focus to the rail. A channel
    // page that has not answered owns no focus at all.
    if (filters_open_ || (in_channel_ && channel_loading()))
        return false;
    return spot_ == Spot::results && results_.wants_rail_focus();
}

std::unique_ptr<Screen> make_search_screen(Context &context)
{
    return std::make_unique<SearchScreen>(context);
}

} // namespace hui::app
