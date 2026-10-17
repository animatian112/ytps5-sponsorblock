// youtube-tv - The app: owns the screens, the tab bar, toasts, haptics and
// the scripted tour used by the PC snapshots and the console validation run.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/card_menu.hpp"
#include "app/frame.hpp"
#include "app/screen.hpp"
#include "app/screens.hpp"
#include "app/telemetry.hpp"
#include "core/settings.hpp"
#include "core/tween.hpp"
#include "ui/components.hpp"
#include "yt/service.hpp"
#include "yt/update.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hui::gfx
{
class GlBatch;
class Renderer;
} // namespace hui::gfx

namespace yt
{
class Service;
}

namespace hui::app
{

class ThumbCache;

class App
{
  public:
    App(const ui::Fonts &fonts, Settings settings, std::string data_root, gfx::GlBatch &batch,
        yt::Service &service);
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;

    static Settings load_settings(const std::string &data_root);

    void update(const InputFrame &input, float dt);
    // Queues this frame's layers; the caller presents them.
    void compose(gfx::Renderer &renderer);
    // Records the active screen's frame without rendering it (tour frames).
    void rehearse();

    const ui::Feedback &feedback() const
    {
        return feedback_;
    }
    bool take_settings_changed();
    bool wants_quit() const
    {
        return quit_;
    }
    Telemetry &telemetry()
    {
        return telemetry_;
    }
    gfx::Color accent() const;
    audio::SoundSet sound_set() const;
    const Settings &settings() const
    {
        return settings_;
    }
    yt::Service &service()
    {
        return service_;
    }
    // Starts the first feed request once the app is on screen.
    void start();
    void set_version(std::string version);

    // ---- the tour (host snapshots and console validation) ----
    std::size_t screen_count() const;
    // Switches to a screen without animation.
    void show(std::size_t index);
    void set_info_open(bool)
    {
    }
    // Advances the script and returns this frame's input.
    InputFrame tour_step(float dt);
    bool tour_finished() const
    {
        return tour_.finished;
    }
    const std::string &tour_capture() const
    {
        return tour_capture_;
    }
    void tour_capture_done()
    {
        tour_capture_.clear();
    }
    void tour_record(float frame_ms, std::size_t draw_calls, std::size_t instances);
    const std::vector<std::string> &tour_report() const
    {
        return tour_report_;
    }
    // Starts the tour at a screen id ("home", "player", ...), or all.
    void begin_tour(const std::string &only);

  private:
    void draw_chrome(Frame &frame);
    void draw_sidebar(ui::Canvas &canvas);
    void draw_screen();
    void handle_global(const InputFrame &input, float dt);
    // The card menu: hold Cross (or press Square) on a video card.
    void filter_card_hold(InputFrame &input, float dt);
    void open_card_menu(const yt::FeedItem &item);
    void open_playlist_menu();
    void apply_card_pick(int pick);
    void refresh_home();
    void save_settings();
    std::size_t previous_tab() const
    {
        return last_tab_;
    }

    // ---- auto-update ----
    // Reads the updater's snapshot and reacts to a revision change only, so a
    // card is never pushed twice for the same news.
    void pump_update_card();
    // Answers the card while it is on screen; true when it held the frame.
    bool handle_update_card(const InputFrame &input);
    void show_update_card(const yt::update::Snapshot &snapshot);
    void refresh_update_progress(const yt::update::Snapshot &snapshot);
    void dismiss_update_card();
    void on_update_action(int id, int action);
    ui::Notification update_note(const yt::update::Snapshot &snapshot) const;

    ui::Fonts fonts_;
    std::string data_root_;
    Settings settings_;
    Telemetry telemetry_;
    yt::Service &service_;
    std::unique_ptr<ThumbCache> thumbs_;
    std::string active_video_;
    ResumeStore resume_;
    yt::ChannelItem pending_channel_;
    std::string pending_browse_;              // the playlist the browse screen opens
    std::vector<yt::FeedItem> play_queue_;    // "Play next" from the card menu
    std::vector<yt::FeedItem> section_items_; // the section a video was opened from
    bool shuffle_started_ = false;            // "Shuffle play" set section_items_ itself
    // The videos of the section the focus is in (for Shuffle).
    std::vector<yt::FeedItem> current_section(const std::string &video_id) const;
    std::size_t player_return_ = 0; // where Circle in the player goes back to
    Context context_;
    std::vector<std::unique_ptr<Screen>> screens_;
    std::vector<std::unique_ptr<Screen>>::size_type current_ = kHomeScreen;
    std::size_t last_tab_ = kHomeScreen; // the tab a non-tab screen returns to
    std::size_t login_return_ = 0;       // where a finished sign-in goes back to
    Nav nav_;
    ui::ToastStack toasts_;
    ui::NotificationStack updates_;
    // The left rail (YouTube's own layout): icons only while the content has
    // the focus, expanded with labels while it has the focus itself.
    ui::SideNav rail_;
    bool rail_focused_ = false;
    bool rail_pinned_ = false;
    std::uint32_t rail_hidden_ = 0;  // the hidden sections the rail was built with
    float idle_s_ = 0.0f;            // seconds without input, for screen dimming
    float token_retry_s_ = 0.0f;     // a kept token that did not refresh: try again
    float rail_settle_ = -1.0f;      // seconds the rail focus rested on a section; -1 idle
    std::size_t channel_return_ = 1; // where Circle on a channel page opened from elsewhere goes
    ui::Feedback feedback_;
    CardMenu card_menu_;
    yt::FeedItem menu_item_;
    bool hold_active_ = false;
    bool avatar_focus_ = false; // the sidebar's account picture has the focus
    void open_account_menu();
    // The sidebar: SmartTube's sections, My videos, Blocked Channels, the
    // pinned channels and Settings; each entry's tag names its target.
    void rebuild_rail();
    // The service's content filters and history switch, from the settings.
    yt::Service::Preferences preferences() const;
    int rail_position(std::size_t screen) const;
    void activate_rail(int position);
    void load_pins();
    void save_pins() const;
    void pin_channel(const yt::ChannelItem &channel);
    void unpin_channel(const std::string &channel_id);
    bool pinned(const std::string &channel_id) const;
    void open_blocked_menu();
    std::vector<yt::ChannelItem> pins_;
    // "Add to new playlist": the name typed on the on-screen keyboard.
    ui::Keyboard prompt_keys_;
    bool prompt_active_ = false;
    std::string prompt_text_;
    std::string prompt_video_;
    void open_name_prompt(const std::string &video_id);
    void draw_prompt(ui::Canvas &canvas) const;
    static constexpr std::size_t kMaxPins = 8;
    float hold_time_ = 0.0f;
    bool waiting_playlists_ = false; // "Save to playlist" asked before the list loaded
    std::vector<yt::FeedItem> menu_playlists_;
    std::uint64_t seen_action_revision_ = 0;
    std::uint64_t seen_resolved_revision_ = 0;
    Frame frame_;
    std::string version_;
    bool quit_ = false;
    bool settings_changed_ = true;
    float save_delay_ = 0.0f;
    float clock_ = 0.0f;
    bool started_ = false;
    bool was_signed_in_ = false; // a fresh sign-in reloads the home feed

    // auto-update: one card at a time, driven by the updater's revision.
    // update_card_ is the live card (0: none), update_revision_ is what it
    // reflects, update_latest_ is the newer release while one exists, and the
    // flags remember a "Later" dismissal, a forced rebuild, the last attempt
    // and the staged close.
    int update_card_ = 0;
    std::uint64_t update_revision_ = 0;
    yt::update::State update_state_ = yt::update::State::idle;
    std::string update_latest_;
    bool update_dismissed_ = false;
    bool update_forced_ = false;
    bool update_installing_ = false;
    float update_close_in_ = -1.0f;
    bool touring_ = false;

    // tour
    struct TourState
    {
        bool started = false;
        bool finished = false;
        std::size_t index = 0;
        std::size_t step = 0;
        float clock = 0.0f;
        bool intro_done = false;
        bool pictured = false;
        std::string only;
        struct Stats
        {
            int frames = 0;
            double total_ms = 0.0;
            float worst_ms = 0.0f;
            std::size_t draw_calls = 0;
            std::size_t instances = 0;
        } stats;
    } tour_;
    std::string tour_capture_;
    std::vector<std::string> tour_report_;
};

} // namespace hui::app
