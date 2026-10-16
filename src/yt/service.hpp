// youtube-tv - The background service: every network request and the video
// player's start-up run on one worker thread with a 1 MiB stack, so the frame
// never waits on TLS. The frame thread reads immutable snapshots.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "yt/player.hpp"
#include "yt/types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace yt
{

enum class FeedKind : unsigned char
{
    home,
    subscriptions,
    search,
    related,
    // One channel's uploads, opened from a search result.
    channel,
    history,
    // The account's playlists (Library).
    library,
    // A topic page (Music, Gaming, News, Live, Sports): one slot, the screen
    // that asked last owns it (topic_id()).
    topic,
};

struct FeedView
{
    Load state = Load::idle;
    std::vector<FeedItem> items;
    std::vector<ChannelItem> channels; // search answers only
    std::string continuation;
    std::string error;
    std::uint64_t revision = 0;
};

struct VideoView
{
    Load state = Load::idle;
    VideoInfo info;
    // The account's standing toward the playing video, from its watch page:
    // like_status "LIKE" / "DISLIKE" / "INDIFFERENT" (empty: unknown).
    std::string like_status;
    bool subscribed = false;
    // The watch-history report for this video: "" (none yet), "ok", or why
    // it failed.
    std::string history_status;
    std::string error;
    // What the pipeline is doing right now ("Player info", "Streams",
    // "Decoder"): the player screen shows it instead of guessing.
    std::string stage;
    // Seconds the request has been waiting, for the progress line.
    float waiting_s = 0.0f;
    // The client whose stream is playing ("TVHTML5"); empty until ready.
    std::string source_client;
    // One line per stream the worker tried when nothing played ("TVHTML5
    // itag 248 1080p: 403 Forbidden"); cleared once one of them plays.
    std::vector<std::string> attempts;
    bool unauthorized = false;
    std::uint64_t revision = 0;
};

struct ChannelView
{
    Load state = Load::idle;
    ChannelItem channel;
    std::vector<ChannelTab> tabs;
    int selected_tab = -1;
    std::string error;
    std::uint64_t revision = 0;
};

struct LoginView
{
    Load state = Load::idle; // loading = waiting for the player to confirm
    std::string user_code;
    std::string verification_url;
    std::string error;
    std::uint64_t revision = 0;
};

class Service
{
  public:
    // token_path is /download0/hui/auth.json on the console; install_root is
    // the folder the app was started from (/app0), where a backup copy of the
    // token is read from and written to when the sandbox allows it. audio is
    // the ring the video player feeds; it may be null (no sound).
    Service(std::string token_path, std::string install_root, AudioSink *audio);
    ~Service();
    Service(const Service &) = delete;
    Service &operator=(const Service &) = delete;

    void start();
    void stop();

    // ---- requests, called from the frame thread ----
    void request_home(bool force = false);
    void request_subscriptions(bool force = false);
    void request_history();
    void request_library();
    // The signed-in account's name and avatar (empty until fetched).
    std::pair<std::string, std::string> account() const;
    void request_topic(std::string browse_id);
    // The browse id the topic feed holds (or is loading).
    std::string topic_id() const;
    void request_more(FeedKind kind);
    void request_search(std::string query);
    // Opens a channel page (channel_id "UC..."): header, tabs and the videos
    // of its first tab. The videos land in the channel feed, not in search.
    void request_channel(std::string channel_id);
    // Switches to one of the channel's tabs by index (see ChannelView::tabs).
    void request_channel_tab(int tab_index);
    // Resolves the streams and opens the player (on the worker).
    void request_video(std::string video_id, CodecChoice choice);
    void request_close_video();
    // Watch history: reports the span watched since the last report (the
    // playback ping went out when the video started). Cheap; call it every
    // half minute of playback and when the video is left.
    void report_watch_progress(double position_s);
    // Like / dislike / clear ("like", "dislike", "removelike"), and subscribe
    // or unsubscribe; both need the account and run on the worker. The video
    // view's like_status / subscribed follow once YouTube accepted them.
    void rate_video(std::string video_id, std::string rating);
    void set_subscribed(std::string channel_id, bool on);
    // Card menu actions (signed in). `done` is the line to show once YouTube
    // took it; action_revision() moves when one finished either way.
    void send_feedback(std::string token, std::string done);
    void add_to_playlist(std::string playlist_id, std::string video_id, std::string done);
    std::uint64_t action_revision() const;
    std::pair<bool, std::string> action_result() const; // ok, message
    // Drops a card from every loaded feed ("Not interested").
    void hide_item(const std::string &video_id);
    // Starts the device flow and then polls until it is answered.
    void request_sign_in();
    void cancel_sign_in();
    void sign_out();
    // Reads a token the user put in the install folder (yt-token.json) when
    // nothing is signed in yet. Runs on the worker like every other request.
    void import_token_file();
    // Writes a second copy of the refresh token so a wiped sandbox does not
    // cost the account. Returns false when no copy could be written; `where`
    // then names the path that failed.
    bool export_token(std::string *where);

    // ---- results, read from the frame thread ----
    std::uint64_t revision(FeedKind kind) const;
    FeedView feed(FeedKind kind) const;
    std::uint64_t search_revision() const;
    FeedView search_feed() const;
    std::string search_query() const;
    // The channel page the search screen is showing (empty when none).
    std::uint64_t channel_revision() const;
    ChannelView channel_view() const;

    std::uint64_t video_revision() const;
    VideoView video() const;
    // Abandons a video request that has been running too long: publishes a
    // failure and drops whatever the worker reports afterwards. Call it every
    // frame with the frame time.
    void video_watchdog(float dt);

    std::uint64_t login_revision() const;
    LoginView login() const;

    // The player the worker opened, if any. The caller takes ownership.
    std::unique_ptr<Player> take_player();

    bool signed_in() const;
    // Loads the saved token once at startup and refreshes it in the background.
    void refresh_token();
    std::string last_error() const;
    // True once when import_token_file() really restored a token from the
    // install folder and loaded it (the feeds carry it now). Consumed on
    // read: the frame shows the restore message exactly then, never for a
    // token that was not restored and loaded.
    bool take_token_restored();

    // ---- offline helpers, used by the PC snapshot run and the console
    // validation tour: publish data without a worker thread ----
    void inject_feed(FeedKind kind, std::vector<FeedItem> items, std::string continuation = {});
    // Channels the same injected page carries (search only).
    void inject_channels(FeedKind kind, std::vector<ChannelItem> channels);
    // A channel page without the network: publishes `view` and the tab's
    // videos into the channel feed.
    void inject_channel_page(ChannelView view, std::vector<FeedItem> items);
    void inject_login(LoginView view);
    void inject_video(VideoView view, std::unique_ptr<Player> player);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace yt
