// youtube-tv - The background service implementation.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One worker thread (1 MiB stack: console threads are never time-sliced and
// the default stack is small) owns every network call, the InnerTube client,
// the auth object and the player's start-up. The frame thread posts requests
// and reads snapshots; a snapshot is copied only when its revision changed.

#include "yt/service.hpp"

#include "core/save_file.hpp"
#include "yt/auth.hpp"
#include "yt/log.hpp"
#include "yt/codec.hpp"
#include "yt/decipher.hpp"
#include "yt/innertube.hpp"
#include "yt/watch.hpp"
#include "yt/http.hpp"

#include <pthread.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace yt
{
namespace
{

constexpr std::size_t kWorkerStack = 1u << 20; // 1 MiB
constexpr int kLoginPollMs = 200;
constexpr std::size_t kMaxAccounts = 3; // accounts remembered for switching
// Formats of one client answer that may be tried before the next client is
// asked. A format that cannot be opened (no decoder for its codec in this
// build, a URL YouTube refuses) is skipped and the next-best format from the
// same answer is tried instead of failing the video outright.
constexpr int kFormatAttemptsPerClient = 3;

// pthread_create with an explicit stack; std::thread cannot size its stack.
bool spawn(pthread_t *thread, void *(*entry)(void *), void *argument)
{
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
        return false;
    pthread_attr_setstacksize(&attributes, kWorkerStack);
    const int created = pthread_create(thread, &attributes, entry, argument);
    pthread_attr_destroy(&attributes);
    return created == 0;
}

// select_video_format over the formats that have not failed to open yet. The
// retry inside one client answer must not pick the entry that just failed
// again, and a codec this build cannot open at all (AV1 without a decoder)
// must not be retried at another height either: both are excluded. The
// selector itself is unchanged; the returned pointer refers to *candidates,
// which therefore has to outlive it.
const Format *select_video_format_retry(const std::vector<Format> &formats,
                                        const CodecChoice &choice,
                                        const std::vector<int> &failed_itags,
                                        const std::vector<Codec> &failed_codecs,
                                        std::vector<Format> *candidates)
{
    if (failed_itags.empty() && failed_codecs.empty())
    {
        return select_video_format(formats, choice);
    }
    candidates->clear();
    candidates->reserve(formats.size());
    for (const Format &format : formats)
    {
        if (std::find(failed_itags.begin(), failed_itags.end(), format.itag) != failed_itags.end())
        {
            continue;
        }
        if (std::find(failed_codecs.begin(), failed_codecs.end(), format.codec) !=
            failed_codecs.end())
        {
            continue;
        }
        candidates->push_back(format);
    }
    return select_video_format(*candidates, choice);
}

} // namespace

struct Service::Impl
{
    // ---- shared between the frame and the worker ----
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;

    // Requests. Values are overwritten by newer ones (latest wins).
    struct Job
    {
        bool home = false;
        bool home_force = false;
        bool subscriptions = false;
        bool history = false;
        bool topic = false;
        std::string topic_id;
        bool library = false;
        bool more = false;
        FeedKind more_kind = FeedKind::home;
        bool channel = false;
        std::string channel_id;
        bool channel_tab = false;
        int channel_tab_index = 0;
        bool search = false;
        std::string search_query;
        bool video = false;
        std::string video_id;
        CodecChoice choice;
        std::uint64_t video_generation = 0;
        bool close_video = false;
        bool sign_in = false;
        bool cancel_sign_in = false;
        bool sign_out = false;
        bool refresh = false;
        bool import_token = false;
        bool rate = false;
        std::string rate_video;
        std::string rating;
        bool subscribe = false;
        std::string subscribe_channel;
        bool subscribe_on = false;
        // Card menu actions, in the order asked: "feedback" (a token) or
        // "playlist" (playlist id, video id); `done` is the toast on success.
        struct Action
        {
            std::string kind;
            std::string first;
            std::string second;
            std::string done;
        };
        std::vector<Action> actions;
        bool add_account = false;
        bool switch_account = false;
        int switch_index = 0;
        // A card's channel looked up from its watch page (TV Home tiles
        // carry none): purpose 0 opens it, 1 subscribes to it.
        bool resolve = false;
        std::string resolve_video;
        std::string resolve_name;
        int resolve_purpose = 0;
    } job;
    std::vector<ChannelItem> blocked; // <token file>.blocked: id TAB name
    Service::Preferences preferences;
    std::string account_channel;
    std::uint64_t resolved_revision = 0;
    ChannelItem resolved_channel;
    int resolved_purpose = 0;

    // ---- the remembered accounts (up to kMaxAccounts) ----
    // One line per account in <token file>.accounts: refresh token, name and
    // picture URL, tab-separated. The signed-in account is the one whose token
    // the Auth object holds.
    struct SavedAccount
    {
        std::string token;
        std::string name;
        std::string photo;
    };
    std::vector<SavedAccount> saved;
    std::string previous_token; // the account an "Add account" left; back to it on cancel
    bool restore_previous = false;
    std::uint64_t action_revision = 0;
    std::string action_message;
    bool action_ok = false;

    // Results.
    FeedView home;
    FeedView subscriptions;
    FeedView history;
    FeedView topic;
    std::string topic_id;
    FeedView library;
    std::string account_name;
    std::string account_photo;
    bool account_fetched = false;
    FeedView search;
    FeedView related;
    FeedView channel_feed; // one channel's videos, opened from search
    ChannelView channel;
    std::string search_query;
    VideoView video;
    LoginView login;
    // Bumped by every new request and by the watchdog: a worker whose
    // generation is stale publishes nothing.
    std::uint64_t video_generation = 0;
    float video_wait_s = 0.0f;
    // True while the worker sits out a pre-roll wait: the watchdog's clock stops.
    bool video_holding = false;

    // Watch history for the video playing (see yt/watch.hpp), under `mutex`.
    struct WatchSession
    {
        std::string watchtime_url;
        std::string cpn;
        std::string token;
        double length_s = 0.0;
        double reported_s = 0.0;
        std::chrono::steady_clock::time_point started{};
    } watch;
    bool video_running = false;
    // The device flow runs on the worker; `login_running` tells the frame
    // thread that it is still in flight and `login_cancel` is the frame's
    // request to stop it (the job flags are consumed by the dispatch loop and
    // cannot be used for this: doing so aborted every sign-in after 200 ms).
    bool login_running = false;
    bool login_cancel = false;
    std::unique_ptr<Player> player;
    bool signed_in = false;
    // Set when import_token_file() really restored a token from the install
    // folder and loaded it; taken once by the frame (take_token_restored) so
    // the restore message runs exactly then.
    bool token_restored = false;
    std::string last_error;

    // ---- worker-only ----
    std::string install_root;
    Auth auth;
    InnerTube tube;
    // One descrambler per Service, prepared once per player JS file and reused
    // across the client chain; descrambler_js_url keys that cache.
    Descrambler descrambler;
    std::string descrambler_js_url;
    AudioSink *audio = nullptr;
    pthread_t worker{};
    std::atomic<bool> running{false};
    bool player_open = false;
    std::string opened_video;
    CodecChoice opened_choice;
    // The channel whose page is open; a tab switch reuses it.
    std::string open_channel_id;

    Impl(std::string token_path, std::string install_root_path, AudioSink *sink)
        : install_root(std::move(install_root_path)), auth(std::move(token_path)), audio(sink)
    {
        load_accounts();
        load_blocked();
    }

    std::string blocked_path() const
    {
        return auth.token_path().empty() ? std::string() : auth.token_path() + ".blocked";
    }

    void load_blocked()
    {
        blocked.clear();
        std::string data;
        if (blocked_path().empty() || !hui::save::read_file(blocked_path(), &data, 64u << 10))
            return;
        std::size_t at = 0;
        while (at < data.size())
        {
            std::size_t end = data.find('\n', at);
            if (end == std::string::npos)
                end = data.size();
            const std::string line = data.substr(at, end - at);
            at = end + 1;
            const std::size_t tab = line.find('\t');
            ChannelItem channel;
            channel.channel_id = line.substr(0, tab);
            if (tab != std::string::npos)
                channel.name = line.substr(tab + 1);
            if (!channel.channel_id.empty() || !channel.name.empty())
                blocked.push_back(std::move(channel));
        }
    }

    void save_blocked() const
    {
        if (blocked_path().empty())
            return;
        std::string data;
        for (const ChannelItem &channel : blocked)
            data += channel.channel_id + '\t' + clean_field(channel.name) + '\n';
        (void)hui::save::write_atomic(blocked_path(), data);
    }

    // Callers hold the mutex.
    bool is_blocked(const FeedItem &item) const
    {
        for (const ChannelItem &channel : blocked)
        {
            if (!channel.channel_id.empty() &&
                (item.channel_id == channel.channel_id || item.video_id == channel.channel_id))
                return true;
            if (!channel.name.empty() && item.author == channel.name)
                return true;
        }
        return false;
    }

    FeedView filtered(const FeedView &view, FeedKind kind = FeedKind::topic) const
    {
        FeedView out = view;
        const Service::Preferences &p = preferences;
        const bool home = kind == FeedKind::home;
        const bool subs = kind == FeedKind::subscriptions;
        const bool shorts = (home && p.shorts_home) || (subs && p.shorts_subscriptions) ||
                            (kind == FeedKind::history && p.shorts_history) ||
                            (kind == FeedKind::search && p.shorts_search);
        const bool watched = (home && p.watched_home) || (subs && p.watched_subscriptions);
        const bool streams = subs && p.streams_subscriptions;
        const bool mixes = p.mixes && kind != FeedKind::library;
        if (blocked.empty() && !shorts && !watched && !streams && !mixes)
            return out;
        const auto drop = [&](const FeedItem &item)
        {
            if (is_blocked(item))
                return true;
            if (shorts && item.short_form)
                return true;
            if (watched && item.watched >= 0.85f)
                return true;
            if (streams && item.live)
                return true;
            // A Mix: a radio playlist ("RD..."), or a card YouTube titles so.
            if (mixes && (item.video_id.rfind("VLRD", 0) == 0 ||
                          item.video_id.rfind("RD", 0) == 0 || item.title.rfind("Mix - ", 0) == 0))
                return true;
            return false;
        };
        out.items.erase(std::remove_if(out.items.begin(), out.items.end(), drop), out.items.end());
        return out;
    }

    std::string bundled_token_path() const
    {
        return install_root.empty() ? std::string() : install_root + "/yt-token.json";
    }

    std::string sandbox_backup_path() const
    {
        return auth.token_path() + ".backup";
    }

    std::string accounts_path() const
    {
        return auth.token_path().empty() ? std::string() : auth.token_path() + ".accounts";
    }

    static std::string clean_field(std::string text)
    {
        for (char &c : text)
            if (c == '\t' || c == '\n' || c == '\r')
                c = ' ';
        return text;
    }

    // Callers hold the mutex.
    void load_accounts()
    {
        saved.clear();
        std::string data;
        if (accounts_path().empty() || !hui::save::read_file(accounts_path(), &data, 64u << 10))
            return;
        std::size_t at = 0;
        while (at < data.size() && saved.size() < kMaxAccounts)
        {
            std::size_t end = data.find('\n', at);
            if (end == std::string::npos)
                end = data.size();
            const std::string line = data.substr(at, end - at);
            at = end + 1;
            const std::size_t a = line.find('\t');
            const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
            SavedAccount account;
            account.token = line.substr(0, a);
            if (a != std::string::npos)
                account.name = line.substr(a + 1, b == std::string::npos ? b : b - a - 1);
            if (b != std::string::npos)
                account.photo = line.substr(b + 1);
            if (!account.token.empty())
                saved.push_back(std::move(account));
        }
    }

    void save_accounts() const
    {
        if (accounts_path().empty())
            return;
        std::string data;
        for (const SavedAccount &account : saved)
            data += account.token + '\t' + clean_field(account.name) + '\t' +
                    clean_field(account.photo) + '\n';
        (void)hui::save::write_atomic(accounts_path(), data);
    }

    // Puts the signed-in account in the list (or refreshes its name and
    // picture). Callers hold the mutex.
    void remember_current()
    {
        const std::string &token = auth.refresh_token();
        if (token.empty())
            return;
        for (SavedAccount &account : saved)
        {
            if (account.token == token)
            {
                if (!account_name.empty())
                    account.name = account_name;
                if (!account_photo.empty())
                    account.photo = account_photo;
                save_accounts();
                return;
            }
        }
        if (saved.size() >= kMaxAccounts)
            return;
        saved.push_back({token, account_name, account_photo});
        save_accounts();
    }

    // Signs in with a remembered token; true when the server took it.
    bool use_token(const std::string &token)
    {
        std::string json = "{\"v\":1,\"refresh_token\":";
        json += '"' + token + "\"}"; // refresh tokens are URL-safe: nothing to escape
        std::string error;
        if (!auth.adopt_token_json(json, &error))
            return false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            reset_account_feeds();
        }
        if (!refresh_token_locked())
            return false;
        std::lock_guard<std::mutex> lock(mutex);
        job.home = true;
        job.subscriptions = true;
        job.history = true;
        login.revision++;
        return true;
    }

    // Leaves the signed-in account on this console only (the list keeps it).
    void sign_out_locally()
    {
        auth.sign_out();
        tube.set_access_token({});
        std::lock_guard<std::mutex> lock(mutex);
        signed_in = false;
        token_restored = false;
        login = LoginView{};
        login.revision++;
        reset_account_feeds();
    }

    void do_import_token()
    {
        if (auth.signed_in())
            return;
        const std::string path = bundled_token_path();
        if (path.empty())
            return;
        std::string data;
        if (!hui::save::read_file(path, &data, 64u << 10))
            return;

        std::string error;
        if (!auth.adopt_token_json(data, &error))
        {
            std::lock_guard<std::mutex> lock(mutex);
            login.state = Load::failed;
            login.error = error;
            login.revision++;
            return;
        }
        // The flag goes up before the signed-in flip inside
        // refresh_token_locked(): the frame reads the one-shot flag in the
        // same frame it first sees signed_in() == true, and a frame that
        // catches the flip before the flag was published would consume it as
        // false and lose the "Account stored" toast.
        {
            std::lock_guard<std::mutex> lock(mutex);
            token_restored = true;
        }
        // A token that was adopted but never loaded (the server or the
        // network refused the exchange) is not a restore: no message, no
        // signed-in state.
        if (!refresh_token_locked())
        {
            std::lock_guard<std::mutex> lock(mutex);
            token_restored = false;
            return;
        }
        log::line("login: token restored from %s", path.c_str());
        std::lock_guard<std::mutex> lock(mutex);
        // The feeds must be requested with the token: queue both so Home and
        // Subscriptions load the signed-in answer instead of whatever was
        // published while the account was still missing.
        job.home = true;
        job.subscriptions = true;
    }

    FeedView *slot(FeedKind kind)
    {
        switch (kind)
        {
        case FeedKind::home:
            return &home;
        case FeedKind::subscriptions:
            return &subscriptions;
        case FeedKind::related:
            return &related;
        case FeedKind::channel:
            return &channel_feed;
        case FeedKind::history:
            return &history;
        case FeedKind::topic:
            return &topic;
        case FeedKind::library:
            return &library;
        default:
            return &search;
        }
    }

    void publish(FeedKind kind, Load state, std::vector<FeedItem> items, std::string continuation,
                 std::string error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        FeedView &view = *slot(kind);
        view.state = state;
        if (!items.empty() || state != Load::loading)
            view.items = std::move(items);
        view.continuation = std::move(continuation);
        view.error = std::move(error);
        view.revision++;
    }

    void publish_error(FeedKind kind, const std::string &error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        FeedView &view = *slot(kind);
        view.state = Load::failed;
        view.error = error;
        view.revision++;
    }

    // The account is gone (signed out, or a token the server rejects): the
    // account feeds go with it, so no screen shows old content or claims to
    // lack it for the wrong reason. Callers hold the mutex.
    void reset_account_feeds()
    {
        home = FeedView{};
        home.revision++;
        subscriptions = FeedView{};
        subscriptions.revision++;
        history = FeedView{};
        history.revision++;
        library = FeedView{};
        library.revision++;
        account_name.clear();
        account_photo.clear();
        account_channel.clear();
        account_fetched = false;
    }

    // ---- jobs ----
    void do_channel_page(const std::string &channel_id)
    {
        log::line("channel: request id=%s", channel_id.c_str());
        open_channel_id = channel_id;

        ChannelPage page;
        bool ok = tube.channel_page(channel_id, &page);
        if (!ok)
        {
            // A stale token: refresh once and retry, as do_feed and do_video
            // do - a public channel must not go blank over an access token
            // the server would have renewed.
            if (tube.last_status() == 401 && refresh_token_locked())
                ok = tube.channel_page(channel_id, &page);
        }
        if (!ok)
        {
            log::line("channel: page failed status=%ld error=%s", tube.last_status(),
                      tube.last_error().c_str());
            {
                std::lock_guard<std::mutex> lock(mutex);
                channel.state = Load::failed;
                channel.error = tube.last_error();
                channel.revision++;
            }
            publish_error(FeedKind::channel, tube.last_error());
            return;
        }

        int selected = 0;
        for (std::size_t i = 0; i < page.tabs.size(); i++)
        {
            if (page.tabs[i].selected)
            {
                selected = static_cast<int>(i);
                break;
            }
        }
        log::line("channel: page tabs=%zu items=%zu", page.tabs.size(), page.items.size());
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel.state = Load::ready;
            channel.channel = std::move(page.channel);
            channel.tabs = std::move(page.tabs);
            channel.selected_tab = selected;
            channel.error.clear();
            channel.revision++;
        }
        publish(FeedKind::channel, Load::ready, std::move(page.items), std::move(page.continuation),
                {});
    }

    void do_channel_tab(int index)
    {
        std::string channel_id;
        std::string params;
        std::string title;
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel_id = open_channel_id;
            if (index >= 0 && index < static_cast<int>(channel.tabs.size()))
            {
                params = channel.tabs[static_cast<std::size_t>(index)].params;
                title = channel.tabs[static_cast<std::size_t>(index)].title;
            }
        }
        if (channel_id.empty() || params.empty())
        {
            log::line("channel: tab %d unavailable", index);
            return;
        }

        FeedPage page;
        bool ok = tube.channel_tab(channel_id, params, &page);
        if (!ok)
        {
            // A stale token: refresh once and retry, as do_feed and do_video
            // do - a tab must not go blank over an access token the server
            // would have renewed.
            if (tube.last_status() == 401 && refresh_token_locked())
                ok = tube.channel_tab(channel_id, params, &page);
        }
        if (!ok)
        {
            log::line("channel: tab %d \"%s\" failed status=%ld error=%s", index, title.c_str(),
                      tube.last_status(), tube.last_error().c_str());
            // Keep the items already on screen; only the error is published.
            publish_error(FeedKind::channel, tube.last_error());
            return;
        }

        log::line("channel: tab %d \"%s\" items=%zu", index, title.c_str(), page.items.size());
        {
            std::lock_guard<std::mutex> lock(mutex);
            channel.selected_tab = index;
            channel.error.clear();
            channel.revision++;
        }
        publish(FeedKind::channel, Load::ready, std::move(page.items), std::move(page.continuation),
                {});
    }

    void do_feed(FeedKind kind, const std::string &query, bool more)
    {
        if (!more)
        {
            std::lock_guard<std::mutex> lock(mutex);
            FeedView &view = *slot(kind);
            view.state = Load::loading;
            view.revision++;
        }

        std::string continuation;
        if (more)
        {
            std::lock_guard<std::mutex> lock(mutex);
            continuation = slot(kind)->continuation;
            if (continuation.empty())
                return;
        }

        FeedPage page;
        bool ok = false;
        switch (kind)
        {
        case FeedKind::home:
            ok = tube.home(&page, continuation);
            break;
        case FeedKind::subscriptions:
            ok = tube.subscriptions(&page, continuation);
            break;
        case FeedKind::history:
            ok = tube.history(&page, continuation);
            break;
        case FeedKind::topic:
            ok = query == "FEshorts" ? tube.shorts(&page, continuation)
                                     : tube.browse(query, &page, continuation);
            break;
        case FeedKind::library:
            ok = tube.browse("FElibrary", &page, continuation);
            if (continuation.empty() && (!ok || page.items.empty()))
                ok = tube.browse("FEplaylist_aggregation", &page, continuation);
            break;
        default:
            ok = tube.search(query, &page, continuation);
            break;
        }

        bool retried = false;
        if (!ok)
        {
            // A stale token: refresh once and retry.
            if (tube.last_status() == 401 && refresh_token_locked())
            {
                retried = true;
                switch (kind)
                {
                case FeedKind::home:
                    ok = tube.home(&page, continuation);
                    break;
                case FeedKind::subscriptions:
                    ok = tube.subscriptions(&page, continuation);
                    break;
                case FeedKind::history:
                    ok = tube.history(&page, continuation);
                    break;
                case FeedKind::topic:
                    ok = query == "FEshorts" ? tube.shorts(&page, continuation)
                                             : tube.browse(query, &page, continuation);
                    break;
                case FeedKind::library:
                    ok = tube.browse("FElibrary", &page, continuation);
                    break;
                default:
                    ok = tube.search(query, &page, continuation);
                    break;
                }
            }
        }
        if (!ok)
        {
            // A 401 that survives a successful token refresh is this page
            // refusing this account (some browse pages do for brand or second
            // accounts), not a dead sign-in: the refresh just proved the
            // token good. Signing out here dropped the access token while the
            // refresh token stayed, so every screen said "signed out" until a
            // restart. A rejected grant is handled in refresh_token_locked().
            if (retried && tube.last_status() == 401)
                log::line("feed: 401 after a good refresh (%s); staying signed in",
                          tube.last_error().c_str());
            publish_error(kind, tube.last_error());
            return;
        }

        std::vector<FeedItem> items;
        if (more)
        {
            std::lock_guard<std::mutex> lock(mutex);
            FeedView &view = *slot(kind);
            items = view.items;
            if (page.items.empty() && page.continuation.empty())
            {
                view.state = Load::ready;
                view.continuation.clear();
                view.revision++;
                return;
            }
        }
        if (more && kind == FeedKind::home)
        {
            // A later Home page whose items name no shelf still is a row of
            // its own, not more of the first one.
            for (FeedItem &item : page.items)
                if (item.shelf.empty())
                    item.shelf = "More recommendations";
        }
        items.insert(items.end(), page.items.begin(), page.items.end());
        publish(kind, Load::ready, std::move(items), page.continuation, {});
        if (kind == FeedKind::history && !more)
        {
            std::lock_guard<std::mutex> lock(mutex);
            history.clear_history_token = page.clear_history_token;
        }
        if (!page.channels.empty())
        {
            std::lock_guard<std::mutex> lock(mutex);
            slot(kind)->channels = std::move(page.channels);
        }
    }

    bool refresh_token_locked()
    {
        std::string error;
        const std::string before = auth.refresh_token();
        if (!auth.refresh(&error))
        {
            std::lock_guard<std::mutex> lock(mutex);
            last_error = error;
            // The auth object drops a token the server rejects; with it gone
            // the account is signed out and the screens must say so. A
            // transport failure keeps the token and the session: the next
            // request reports the network error instead of telling the
            // player to sign in again.
            if (!auth.signed_in())
            {
                signed_in = false;
                tube.set_access_token({});
                login.revision++;
                reset_account_feeds();
            }
            return false;
        }
        tube.set_access_token(auth.access_token());
        std::lock_guard<std::mutex> lock(mutex);
        signed_in = true;
        // A refresh may hand out a new refresh token: the remembered account
        // follows it, or switching back later would offer a dead grant.
        const std::string &after = auth.refresh_token();
        if (!before.empty() && after != before)
        {
            for (SavedAccount &account : saved)
                if (account.token == before)
                    account.token = after;
            save_accounts();
        }
        return true;
    }

    // Publishes a video failure, but only while this job is still the current
    // one: the watchdog may have thrown the request away in the meantime.
    void cancel_login_now()
    {
        std::lock_guard<std::mutex> lock(mutex);
        login_cancel = true;
        if (!login_running && login.state != Load::idle)
        {
            login.state = Load::idle;
            login.error.clear();
            login.revision++;
        }
    }

    bool fail_video(std::uint64_t generation, const std::string &reason)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation != video_generation)
            return false;
        video.state = Load::failed;
        video.error = reason;
        video.stage.clear();
        video_running = false;
        video.revision++;
        return true;
    }

    void set_stage(std::uint64_t generation, const char *stage)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation != video_generation)
            return;
        video.stage = stage;
        video.revision++;
    }

    // googlevideo refuses (403) the URLs of an answer that scheduled pre-roll
    // ads until those ads would have played out. Sleeps until then, showing a
    // countdown; the watchdog is held meanwhile. False when the request was
    // dropped (closed, or a newer video) while waiting.
    bool wait_for_preroll(std::uint64_t generation, const VideoInfo &info)
    {
        constexpr long long kMaxWaitMs = 180 * 1000;
        for (;;)
        {
            const long long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count();
            const long long left_ms = info.available_at_ms - now_ms;
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (generation != video_generation)
                    return false;
                if (left_ms <= 0 || left_ms > kMaxWaitMs)
                {
                    video_holding = false;
                    return true;
                }
                if (!video_holding)
                    log::line("video: %s answer had %d pre-roll ad(s); waiting %.0f s",
                              info.client.c_str(), info.preroll_ads,
                              static_cast<double>(left_ms) / 1000.0);
                video_holding = true;
                char text[96];
                std::snprintf(text, sizeof(text), "Waiting %lld s (YouTube ad time, no ad shown)",
                              (left_ms + 999) / 1000);
                if (video.stage != text)
                {
                    video.stage = text;
                    video.revision++;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }

    // Records one stream the worker could not play, so a failure can name
    // every client that was tried.
    void add_attempt(std::uint64_t generation, const std::string &line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (generation != video_generation)
            return;
        video.attempts.push_back(line);
        video.revision++;
    }

    // Prepares the descrambler for this answer's player JS, cached across the
    // client chain by JS URL. False leaves *error as the reason.
    bool ensure_descrambler(const VideoInfo &info, std::string *error)
    {
        if (info.player_js_url.empty())
        {
            *error = "no player JS URL in the answer";
            return false;
        }
        if (info.player_js_url == descrambler_js_url && descrambler.ready())
            return true;
        std::string reason;
        if (!descrambler.prepare(info.player_js_url, &reason))
        {
            descrambler_js_url.clear();
            *error = "player JS: " + reason;
            return false;
        }
        descrambler_js_url = info.player_js_url;
        log::line("video: descrambler ready js=%s", descrambler_js_url.c_str());
        return true;
    }

    // The directory the app may write to: the one that already holds the token
    // (/download0/hui on the console).
    std::string raw_dump_path() const
    {
        const std::string token = auth.token_path();
        const std::size_t slash = token.find_last_of('/');
        const std::string directory =
            slash == std::string::npos ? std::string() : token.substr(0, slash);
        return (directory.empty() ? std::string() : directory + "/") + "yt-player-last.json";
    }

    // Keeps the first client's untouched answer when nothing was usable, so a
    // SABR-only failure can be told apart on the console. Best effort: a failed
    // write is only a log line, never a playback failure.
    void keep_raw_answer(const std::vector<VideoInfo> &variants)
    {
        if (variants.empty() || variants.front().raw_body.empty())
            return;
        const std::string path = raw_dump_path();
        const std::string error = hui::save::write_atomic(path, variants.front().raw_body);
        if (error.empty())
            log::line("video: raw player answer kept at %s", path.c_str());
        else
            log::line("video: raw player answer not kept: %s", error.c_str());
    }

    // Why nothing played, with the census of the first answer: the log and the
    // player screen both read the "no_stream (...)" text, whose prefix is the
    // marker the screen keys on.
    std::string no_stream_reason(const std::vector<VideoInfo> &variants, const CodecChoice &choice)
    {
        const VideoInfo &first = variants.front();
        // SABR-only answers carry no per-format URL at all (yt-dlp #12482):
        // name that cause instead of reporting an empty histogram.
        bool all_sabr = true;
        for (const VideoInfo &info : variants)
        {
            if (!info.sabr_only)
            {
                all_sabr = false;
                break;
            }
        }
        if (all_sabr && !first.census.empty())
            return "no_stream (SABR-only answers from every client: " + first.census + ")";
        const std::string summary = format_summary(first.formats);
        const std::size_t total = first.formats.size();
        std::size_t ciphered = 0;
        std::size_t enabled = 0;
        std::size_t in_cap = 0;
        for (const Format &format : first.formats)
        {
            if (format.ciphered)
                ++ciphered;
            // Mirror select_video_format: an enabled codec alone is not enough,
            // the format also needs a known height and to be inside the cap.
            if (!is_video_codec(format.codec) || !codec_enabled(choice, format.codec))
                continue;
            if (format.height <= 0)
                continue;
            ++enabled;
            if (choice.max_height <= 0 || format.height <= choice.max_height)
                ++in_cap;
        }
        std::string cause;
        if (total == 0)
            cause = "no formats in the client answer";
        else if (ciphered == total && !descrambler.ready())
            cause = "only ciphered formats and no player JS";
        else if (enabled == 0)
            cause = "all filtered by the codec switches";
        else if (in_cap == 0)
            cause = "all filtered by the height cap";
        else
            cause = "the cipher could not be descrambled";
        return "no_stream (" + summary + "; " + cause + ")";
    }

    void do_video(const std::string &video_id, const CodecChoice &choice, std::uint64_t generation)
    {
        log::line("video: request id=%s cap=%d codecs avc=%d vp9=%d av1=%d", video_id.c_str(),
                  choice.max_height, choice.avc ? 1 : 0, choice.vp9 ? 1 : 0, choice.av1 ? 1 : 0);
        set_stage(generation, "Player info");
        tube.set_timeout(12); // each client answer is bounded; the chain runs a few

        // Every client that answered, in chain order: one may hand out a URL
        // googlevideo refuses (403) while another's plays, so try them all
        // instead of giving up on the first.
        std::vector<VideoInfo> variants;
        std::string player_error;
        if (!tube.player_variants(video_id, &variants, &player_error))
        {
            log::line("video: player info failed status=%ld error=%s", tube.last_status(),
                      player_error.c_str());
            if (tube.last_status() == 401 && refresh_token_locked())
            {
                variants.clear();
                player_error.clear();
                if (tube.player_variants(video_id, &variants, &player_error))
                {
                    // recovered
                }
                else
                {
                    fail_video(generation, player_error);
                    return;
                }
            }
            else
            {
                fail_video(generation, player_error);
                return;
            }
        }
        if (variants.empty())
        {
            // player_variants answers true only with an entry; stay safe.
            fail_video(generation, player_error.empty()
                                       ? std::string("no_stream (no formats in the client answer)")
                                       : player_error);
            return;
        }
        log::line("video: player info ok id=%s title=%s clients=%zu", video_id.c_str(),
                  variants.front().title.c_str(), variants.size());

        set_stage(generation, "Streams");
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != video_generation)
                return;
            video.info = variants.front(); // the title, before anything plays
        }

        // The first client's player error is the interesting one: if every
        // client failed it becomes the published error.
        std::string first_error;
        bool any_format = false;

        for (const VideoInfo &info : variants)
        {
            // The watchdog or a newer request drops the whole loop: checked
            // before each client's stream is opened.
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (generation != video_generation)
                    return;
            }

            // The format that cannot be opened is skipped and the next-best
            // format from the same answer is tried before the next client: a
            // client's tallest in-ceiling pick can be AV1 this build has no
            // decoder for, and the URL YouTube refuses can sit next to one it
            // serves. Both exclusion sets live for this client only.
            std::vector<int> failed_itags;
            std::vector<Codec> failed_codecs;
            std::vector<Format> candidates; // the copy select_video_format reads
            bool selected_any = false;

            for (int attempt = 0; attempt < kFormatAttemptsPerClient; ++attempt)
            {
                const Format *video_format = select_video_format_retry(
                    info.formats, choice, failed_itags, failed_codecs, &candidates);
                if (video_format == nullptr)
                {
                    // Only the first selection speaks for the answer: when the
                    // retries ran out of candidates the census was already
                    // reported, and when nothing ever matched it still has to
                    // be.
                    if (!selected_any)
                    {
                        if (info.sabr_only)
                        {
                            const std::string detail =
                                "SABR-only answer (YouTube serves this client no per-format URLs)";
                            log::line("video: try %s %s", info.client.c_str(), detail.c_str());
                            add_attempt(generation, info.client + ": " + detail);
                        }
                        else
                        {
                            // Keep the old wording, plus the raw counts so
                            // yt.log shows what the client actually answered.
                            log::line("video: try %s no stream matches the codec switches (%s)",
                                      info.client.c_str(), info.census.c_str());
                            add_attempt(generation, info.client + ": no matching video format (" +
                                                        info.census + ")");
                        }
                    }
                    break;
                }
                selected_any = true;
                any_format = true;
                const Format *audio_format = select_audio_format(info.formats, choice.audio_track);
                const bool has_n = video_format->url.find("&n=") != std::string::npos ||
                                   video_format->url.find("?n=") != std::string::npos ||
                                   video_format->url.find("/n/") != std::string::npos;
                log::line("video: selected client=%s itag=%d %dx%d %s audio=%d ciphered=%d n=%d "
                          "wait=%.0fs",
                          info.client.c_str(), video_format->itag, video_format->width,
                          video_format->height, codec_label(video_format->codec),
                          audio_format != nullptr ? audio_format->itag : 0,
                          video_format->ciphered ? 1 : 0, has_n ? 1 : 0, info.preroll_wait_s);

                // A ciphered format arrives with its unsigned base URL; the
                // player JS has to descramble it before it plays. Without that
                // JS, or when descrambling fails, this client is an ordinary
                // failed attempt; the JS is per answer, so another format of
                // the same answer cannot help and the client is left alone.
                std::string video_url = video_format->url;
                std::string audio_url = audio_format != nullptr ? audio_format->url : std::string();
                if (video_format->ciphered || (audio_format != nullptr && audio_format->ciphered))
                {
                    std::string cipher_error;
                    if (!ensure_descrambler(info, &cipher_error))
                    {
                        log::line("video: try %s cipher: %s", info.client.c_str(),
                                  cipher_error.c_str());
                        add_attempt(generation, info.client + ": " + cipher_error);
                        break;
                    }
                    if (video_format->ciphered)
                    {
                        video_url =
                            descrambler.descramble(video_format->url, video_format->cipher_s,
                                                   video_format->cipher_sp, &cipher_error);
                        if (video_url.empty())
                        {
                            log::line("video: try %s descramble: %s", info.client.c_str(),
                                      cipher_error.c_str());
                            add_attempt(generation, info.client + " itag " +
                                                        std::to_string(video_format->itag) +
                                                        " descramble: " + cipher_error);
                            set_stage(generation, "Streams");
                            break;
                        }
                    }
                    if (audio_format != nullptr && audio_format->ciphered)
                    {
                        audio_url =
                            descrambler.descramble(audio_format->url, audio_format->cipher_s,
                                                   audio_format->cipher_sp, &cipher_error);
                        if (audio_url.empty())
                        {
                            // The picture can still play without sound.
                            log::line("video: audio descramble failed: %s", cipher_error.c_str());
                            audio_url.clear();
                        }
                    }
                }

                if (!wait_for_preroll(generation, info))
                    return;

                set_stage(generation, "Decoder");
                std::unique_ptr<Player> fresh = make_player();
                fresh->set_decode_mode(choice.decode);
                log::line("video: open begin client=%s", info.client.c_str());
                const bool opened = fresh->open(video_url, audio_url, audio);
                log::line("video: try %s itag=%d %dx%d -> %s", info.client.c_str(),
                          video_format->itag, video_format->width, video_format->height,
                          opened ? "ok" : fresh->error().c_str());
                if (!opened)
                {
                    if (first_error.empty())
                        first_error = fresh->error();
                    // The failure names the format and its URL kind: a 403 on a
                    // ciphered format means the descrambled URL was refused,
                    // which reads very differently from a decoder error.
                    add_attempt(generation, info.client + " " + yt::describe_format(video_format) +
                                                ": " + fresh->error());
                    set_stage(generation, "Streams");
                    // This itag, and the codec of a format this build cannot
                    // open at all, are not tried again in this client.
                    failed_itags.push_back(video_format->itag);
                    failed_codecs.push_back(video_format->codec);
                    continue;
                }

                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (generation != video_generation)
                        return; // the watchdog gave up while the demuxers opened
                    player = std::move(fresh);
                    player_open = true;
                    opened_video = video_id;
                    opened_choice = choice;
                    video.info = info;
                    // Subtitles: the stream's client may answer without the
                    // caption list another client gave.
                    for (const VideoInfo &variant : variants)
                    {
                        if (!video.info.captions.empty())
                            break;
                        video.info.captions = variant.captions;
                    }
                    // The seek preview sheets, likewise from any client.
                    for (const VideoInfo &variant : variants)
                    {
                        if (!video.info.storyboard_spec.empty())
                            break;
                        video.info.storyboard_spec = variant.storyboard_spec;
                    }
                    video.state = Load::ready;
                    video.stage.clear();
                    video.source_client = info.client;
                    video.attempts.clear();
                    video_running = false;
                    video.revision++;
                }

                log::line("video: playing via %s", info.client.c_str());
                start_watch_history(info, variants);
                // The related row under the player; best effort, a failure
                // leaves the previous row in place.
                FeedPage related_page;
                if (tube.related(video_id, &related_page))
                {
                    if (!related_page.channel_avatar.empty())
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        video.channel_avatar = related_page.channel_avatar;
                        video.revision++;
                    }
                    if (!related_page.chapters.empty())
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        video.chapters = related_page.chapters;
                        video.revision++;
                    }
                    if (related_page.meta_known)
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        video.like_status = related_page.like_status;
                        video.subscribed = related_page.subscribed;
                        if (video.info.channel_id.empty())
                            video.info.channel_id = related_page.channel_id;
                        video.revision++;
                    }
                    publish(FeedKind::related, Load::ready, related_page.items, {}, {});
                }
                else
                    publish(FeedKind::related, Load::failed, {}, {}, tube.last_error());
                return;
            }
        }

        if (!any_format)
        {
            std::string reason = no_stream_reason(variants, choice);
            // Nothing was usable, so keep the first client's untouched answer
            // and point the on-screen detail (after the "no_stream" marker) at
            // it; the path goes inside the parentheses the screen strips.
            if (!variants.front().raw_body.empty())
            {
                keep_raw_answer(variants);
                reason.insert(reason.size() - 1, "; raw answer " + raw_dump_path());
            }
            log::line("video: %s", reason.c_str());
            fail_video(generation, reason);
            return;
        }
        log::line("video: every client failed, first error=%s", first_error.c_str());
        fail_video(generation, first_error.empty() ? std::string("unavailable") : first_error);
    }

    // Watch history: the playback ping, sent with the account's token so the
    // video is added to its history. The tracking URLs of a TV client answer
    // (the one that saw the token) are preferred over the stream's client.
    void start_watch_history(const VideoInfo &playing, const std::vector<VideoInfo> &variants)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            watch = WatchSession{};
        }
        if (!auth.signed_in())
            return;
        {
            // Settings > General > History off: YouTube is not told.
            std::lock_guard<std::mutex> lock(mutex);
            if (!preferences.history)
                return;
        }
        // A signed-in TV answer's tracking URLs; asked for on their own when
        // the stream came from another client.
        std::string playback;
        std::string watchtime;
        std::string source = "TVHTML5";
        for (const VideoInfo &variant : variants)
        {
            if (variant.client == "TVHTML5" && !variant.playback_url.empty())
            {
                playback = variant.playback_url;
                watchtime = variant.watchtime_url;
                break;
            }
        }
        if (playback.empty() && !tube.tracking(playing.video_id, &playback, &watchtime))
        {
            playback = playing.playback_url;
            watchtime = playing.watchtime_url;
            source = playing.client;
        }
        if (playback.empty())
        {
            set_history_status("no tracking URL");
            return;
        }
        const std::string cpn = watch::make_cpn(static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::string token = auth.access_token();
        // The ping goes to the stats host the answer names and to www (where
        // a web session's history is kept); the shared cpn makes them one view.
        const std::string ping = watch::playback_ping(playback, cpn, tv_watch_client());
        // The address in full (the token travels in a header, not in it): what
        // YouTube was told is the first thing to compare when history stays empty.
        log::line("history: playback url %s", ping.c_str());
        if (watch::account_host(ping) != ping)
        {
            const http::Response stats = http::get(ping, watch_headers(token), 8);
            log::line("history: playback ping (stats host) -> %ld %s", stats.status,
                      stats.error.c_str());
        }
        const http::Response response =
            http::get(watch::account_host(ping), watch_headers(token), 8);
        log::line("history: playback ping (%s) -> %ld %s", source.c_str(), response.status,
                  response.error.c_str());
        set_history_status(response.ok() && response.status >= 200 && response.status < 300
                               ? std::string("ok")
                               : (response.error.empty() ? "HTTP " + std::to_string(response.status)
                                                         : response.error));
        std::lock_guard<std::mutex> lock(mutex);
        watch.watchtime_url = watchtime;
        watch.cpn = cpn;
        watch.token = token;
        watch.length_s = playing.length_s;
        watch.started = std::chrono::steady_clock::now();
    }

    static watch::Client tv_watch_client()
    {
        return {"TVHTML5", tv_client_version()};
    }

    void set_history_status(const std::string &status)
    {
        std::lock_guard<std::mutex> lock(mutex);
        video.history_status = status;
        video.revision++;
    }

    static std::vector<http::Header> watch_headers(const std::string &token)
    {
        return {{"Authorization", "Bearer " + token},
                {"User-Agent", "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/25.lts.30.1034943-gold "
                               "(unlike Gecko), Unknown_TV_Unknown_0/Unknown (Unknown, Unknown)"}};
    }

    void finish_login(Load state, const std::string &error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        login_running = false;
        login.state = state;
        login.error = error;
        login.revision++;
    }

    void do_login()
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            login_running = true;
            login_cancel = false;
        }
        DeviceCode code = auth.begin();
        if (!code.ok())
        {
            finish_login(Load::failed, code.error);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            login.state = Load::loading;
            login.user_code = code.user_code;
            login.verification_url = code.verification_url;
            login.error.clear();
            login.revision++;
        }

        for (;;)
        {
            // Sleep in slices so a cancel is seen quickly.
            for (int waited = 0; waited < std::max(1, code.interval_s) * 1000;
                 waited += kLoginPollMs)
            {
                if (quit)
                    return;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (login_cancel)
                    {
                        restore_previous = !previous_token.empty();
                        login_cancel = false;
                        login_running = false;
                        login.state = Load::idle;
                        login.error.clear();
                        login.revision++;
                        return;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kLoginPollMs));
            }

            std::string error;
            const Poll poll = auth.poll(code, &error);
            if (poll == Poll::pending)
                continue;
            if (poll == Poll::slow_down)
            {
                code.interval_s += 5;
                continue;
            }
            if (poll == Poll::done)
                tube.set_access_token(auth.access_token());
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (poll == Poll::done)
                {
                    signed_in = true;
                    previous_token.clear();
                    remember_current();
                }
            }
            log::line("login: poll -> %d (%s)", static_cast<int>(poll), error.c_str());
            finish_login(poll == Poll::done ? Load::ready : Load::failed,
                         poll == Poll::done ? std::string() : error);
            return;
        }
    }

    void loop()
    {
        for (;;)
        {
            Job taken;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return quit || any_request(); });
                if (quit)
                    return;
                taken = job;
                job = Job{};
            }

            if (taken.sign_out)
            {
                // The signed-in account leaves the list too; the next
                // remembered one takes over.
                std::string next;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const std::string token = auth.refresh_token();
                    saved.erase(std::remove_if(saved.begin(), saved.end(),
                                               [&](const SavedAccount &account)
                                               { return account.token == token; }),
                                saved.end());
                    save_accounts();
                    if (!saved.empty())
                        next = saved.front().token;
                }
                sign_out_locally();
                if (!next.empty() && !use_token(next))
                    log::line("accounts: the next account was refused");
            }
            if (taken.add_account)
            {
                // The current account stays in the list; the console signs
                // it out and runs the sign-in for the new one. A cancel
                // brings the old one back.
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    remember_current();
                    previous_token = auth.refresh_token();
                }
                sign_out_locally();
                taken.sign_in = true;
            }
            if (taken.switch_account)
            {
                std::string token;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    remember_current();
                    if (taken.switch_index >= 0 &&
                        taken.switch_index < static_cast<int>(saved.size()))
                        token = saved[static_cast<std::size_t>(taken.switch_index)].token;
                }
                const std::string before = auth.refresh_token();
                if (!token.empty() && token != before)
                {
                    if (!use_token(token))
                    {
                        // Back to the account that was working, rather than
                        // half in the new one (its token, the old access).
                        log::line("accounts: switch refused (%s); staying", last_error.c_str());
                        if (!before.empty())
                            (void)use_token(before);
                        std::lock_guard<std::mutex> lock(mutex);
                        last_error = "Couldn't switch: that account's sign-in didn't refresh";
                    }
                }
            }
            if (taken.cancel_sign_in)
            {
                // Sets the worker's cancel flag; a running flow stops on its
                // next slice, an idle one is answered directly.
                cancel_login_now();
            }
            if (taken.refresh)
                refresh_token_locked();
            if (taken.import_token)
                do_import_token();
            if (taken.close_video)
            {
                std::lock_guard<std::mutex> lock(mutex);
                player.reset();
                player_open = false;
            }
            if (taken.home)
            {
                do_feed(FeedKind::home, {}, false);
                // SmartTube's Home is many shelves: the TV answer gives a few
                // per page, so the next pages are fetched right away.
                for (int page = 0; page < 4; ++page)
                {
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        if (home.state != Load::ready || home.continuation.empty() || quit)
                            break;
                    }
                    do_feed(FeedKind::home, {}, true);
                }
            }
            if (taken.subscriptions)
                do_feed(FeedKind::subscriptions, {}, false);
            if (taken.history)
                do_feed(FeedKind::history, {}, false);
            if (taken.topic)
                do_feed(FeedKind::topic, taken.topic_id, false);
            if (taken.library)
                do_feed(FeedKind::library, {}, false);
            if (auth.signed_in() && !account_fetched)
            {
                // Once per sign-in: the name and avatar for the corner.
                account_fetched = true;
                std::string name;
                std::string photo;
                std::string channel;
                const bool found = tube.account(&name, &photo, &channel);
                if (found && channel.empty() && !name.empty())
                {
                    // Last resort: the channel named like the account, from a
                    // search for that name.
                    FeedPage page;
                    if (tube.search(name, &page))
                    {
                        const auto lower = [](std::string text)
                        {
                            for (char &c : text)
                                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                            return text;
                        };
                        for (const ChannelItem &candidate : page.channels)
                        {
                            if (lower(candidate.name) == lower(name))
                            {
                                channel = candidate.channel_id;
                                break;
                            }
                        }
                    }
                    log::line("account: search for \"%s\" -> %s", name.c_str(),
                              channel.empty() ? "(no channel of that name)" : channel.c_str());
                }
                if (found)
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    account_name = name;
                    account_photo = photo;
                    account_channel = channel;
                    remember_current();
                }
            }
            if (taken.channel)
                do_channel_page(taken.channel_id);
            if (taken.channel_tab)
                do_channel_tab(taken.channel_tab_index);
            if (taken.search)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    search_query = taken.search_query;
                }
                do_feed(FeedKind::search, taken.search_query, false);
            }
            if (taken.more)
                do_feed(taken.more_kind, taken.search_query, true);
            if (taken.rate)
            {
                const bool done = tube.rate(taken.rate_video, taken.rating);
                std::lock_guard<std::mutex> lock(mutex);
                if (done && video.info.video_id == taken.rate_video)
                {
                    video.like_status = taken.rating == "like"      ? "LIKE"
                                        : taken.rating == "dislike" ? "DISLIKE"
                                                                    : "INDIFFERENT";
                    video.revision++;
                }
            }
            if (taken.subscribe)
            {
                const bool done = tube.subscribe(taken.subscribe_channel, taken.subscribe_on);
                std::lock_guard<std::mutex> lock(mutex);
                if (done && video.info.channel_id == taken.subscribe_channel)
                {
                    video.subscribed = taken.subscribe_on;
                    video.revision++;
                }
            }
            if (taken.resolve)
            {
                FeedPage page;
                const bool ok = tube.related(taken.resolve_video, &page);
                std::lock_guard<std::mutex> lock(mutex);
                resolved_channel = ChannelItem{};
                if (ok)
                    resolved_channel.channel_id = page.channel_id;
                resolved_channel.name = taken.resolve_name;
                resolved_purpose = taken.resolve_purpose;
                resolved_revision++;
                log::line("channel: %s belongs to %s", taken.resolve_video.c_str(),
                          resolved_channel.channel_id.empty()
                              ? "(unknown)"
                              : resolved_channel.channel_id.c_str());
            }
            for (const Job::Action &action : taken.actions)
            {
                const bool done = action.kind == "feedback" ? tube.send_feedback(action.first)
                                  : action.kind == "create"
                                      ? tube.create_playlist(action.first, action.second)
                                      : tube.add_to_playlist(action.first, action.second);
                std::lock_guard<std::mutex> lock(mutex);
                action_ok = done;
                action_message = done ? action.done : std::string("That didn't go through");
                action_revision++;
            }
            if (taken.video)
                do_video(taken.video_id, taken.choice, taken.video_generation);
            if (taken.sign_in)
                do_login();
            bool restore = false;
            std::string back;
            {
                std::lock_guard<std::mutex> lock(mutex);
                restore = restore_previous && !auth.signed_in();
                restore_previous = false;
                back = previous_token;
                if (restore)
                    previous_token.clear();
            }
            if (restore && !back.empty() && !use_token(back))
                log::line("accounts: could not return to the previous account");
        }
    }

    bool any_request()
    {
        return job.home || job.subscriptions || job.history || job.topic || job.library ||
               job.more || job.search || job.channel || job.channel_tab || job.video ||
               job.close_video || job.sign_in || job.cancel_sign_in || job.sign_out ||
               job.refresh || job.import_token || job.rate || job.subscribe ||
               !job.actions.empty() || job.add_account || job.switch_account || restore_previous ||
               job.resolve;
    }
};

Service::Service(std::string token_path, std::string install_root, AudioSink *audio)
    : impl_(std::make_unique<Impl>(std::move(token_path), std::move(install_root), audio))
{
}

Service::~Service()
{
    stop();
}

void Service::start()
{
    if (impl_->running.exchange(true))
        return;
    if (!spawn(
            &impl_->worker,
            [](void *argument) -> void *
            {
                static_cast<Impl *>(argument)->loop();
                return nullptr;
            },
            impl_.get()))
    {
        impl_->running = false;
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->last_error = "worker thread failed";
    }
}

void Service::stop()
{
    if (!impl_->running.exchange(false))
        return;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->quit = true;
    }
    impl_->wake.notify_all();
    pthread_join(impl_->worker, nullptr);
}

void Service::request_home(bool force)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.home = true;
    impl_->job.home_force = force;
    impl_->wake.notify_all();
}

void Service::request_subscriptions(bool force)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.subscriptions = true;
    (void)force;
    impl_->wake.notify_all();
}

void Service::rate_video(std::string video_id, std::string rating)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.rate = true;
    impl_->job.rate_video = std::move(video_id);
    impl_->job.rating = std::move(rating);
    impl_->wake.notify_all();
}

void Service::send_feedback(std::string token, std::string done)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.actions.push_back({"feedback", std::move(token), {}, std::move(done)});
    impl_->wake.notify_all();
}

void Service::add_to_playlist(std::string playlist_id, std::string video_id, std::string done)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.actions.push_back(
        {"playlist", std::move(playlist_id), std::move(video_id), std::move(done)});
    impl_->wake.notify_all();
}

void Service::block_channel(ChannelItem channel)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const ChannelItem &known : impl_->blocked)
    {
        if ((!channel.channel_id.empty() && known.channel_id == channel.channel_id) ||
            (!channel.name.empty() && known.name == channel.name))
            return;
    }
    impl_->blocked.push_back(std::move(channel));
    impl_->save_blocked();
    // Every feed's screen rebuilds without the channel's cards.
    for (FeedKind kind :
         {FeedKind::home, FeedKind::subscriptions, FeedKind::history, FeedKind::topic,
          FeedKind::related, FeedKind::search, FeedKind::library, FeedKind::channel})
        impl_->slot(kind)->revision++;
}

void Service::unblock_channel(std::size_t index)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (index >= impl_->blocked.size())
        return;
    impl_->blocked.erase(impl_->blocked.begin() + static_cast<std::ptrdiff_t>(index));
    impl_->save_blocked();
    for (FeedKind kind :
         {FeedKind::home, FeedKind::subscriptions, FeedKind::history, FeedKind::topic,
          FeedKind::related, FeedKind::search, FeedKind::library, FeedKind::channel})
        impl_->slot(kind)->revision++;
}

std::vector<ChannelItem> Service::blocked_channels() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->blocked;
}

std::string Service::account_channel() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->account_channel;
}

void Service::resolve_channel(std::string video_id, std::string name, int purpose)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.resolve = true;
    impl_->job.resolve_video = std::move(video_id);
    impl_->job.resolve_name = std::move(name);
    impl_->job.resolve_purpose = purpose;
    impl_->wake.notify_all();
}

std::uint64_t Service::resolved_revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->resolved_revision;
}

std::pair<ChannelItem, int> Service::resolved_channel() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->resolved_channel, impl_->resolved_purpose};
}

void Service::create_playlist(std::string title, std::string video_id, std::string done)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.actions.push_back(
        {"create", std::move(title), std::move(video_id), std::move(done)});
    impl_->job.library = true; // the new list shows up in the pickers
    impl_->wake.notify_all();
}

std::uint64_t Service::action_revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->action_revision;
}

std::pair<bool, std::string> Service::action_result() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->action_ok, impl_->action_message};
}

void Service::hide_item(const std::string &video_id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (FeedKind kind : {FeedKind::home, FeedKind::subscriptions, FeedKind::history,
                          FeedKind::topic, FeedKind::related, FeedKind::search})
    {
        FeedView &view = *impl_->slot(kind);
        const auto before = view.items.size();
        view.items.erase(std::remove_if(view.items.begin(), view.items.end(),
                                        [&](const FeedItem &item)
                                        { return item.video_id == video_id; }),
                         view.items.end());
        if (view.items.size() != before)
            view.revision++;
    }
}

void Service::set_subscribed(std::string channel_id, bool on)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.subscribe = true;
    impl_->job.subscribe_channel = std::move(channel_id);
    impl_->job.subscribe_on = on;
    impl_->wake.notify_all();
}

void Service::request_topic(std::string browse_id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->topic_id != browse_id)
    {
        // Another topic's rows must not show under this one's title.
        impl_->topic = FeedView{};
        impl_->topic.state = Load::loading;
        impl_->topic.revision++;
        impl_->topic_id = browse_id;
    }
    impl_->job.topic = true;
    impl_->job.topic_id = std::move(browse_id);
    impl_->wake.notify_all();
}

std::string Service::topic_id() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->topic_id;
}

void Service::request_library()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.library = true;
    impl_->wake.notify_all();
}

std::pair<std::string, std::string> Service::account() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->account_name, impl_->account_photo};
}

void Service::request_history()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.history = true;
    impl_->wake.notify_all();
}

void Service::request_more(FeedKind kind)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.more = true;
    impl_->job.more_kind = kind;
    impl_->job.search_query = impl_->search_query;
    impl_->wake.notify_all();
}

void Service::request_search(std::string query)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.search = true;
    impl_->job.search_query = std::move(query);
    impl_->job.channel = false;
    impl_->job.channel_tab = false;
    // The search screen reads any open channel through channel_view(); a new
    // query dismisses it.
    const std::uint64_t revision = impl_->channel.revision + 1;
    impl_->channel = ChannelView{};
    impl_->channel.revision = revision;
    impl_->wake.notify_all();
}

void Service::request_channel(std::string channel_id)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.channel = true;
    impl_->job.channel_id = std::move(channel_id);
    impl_->job.channel_tab = false;
    impl_->channel.state = Load::loading;
    impl_->channel.error.clear();
    impl_->channel.revision++;
    impl_->wake.notify_all();
}

void Service::request_channel_tab(int tab_index)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.channel = false;
    impl_->job.channel_tab = true;
    impl_->job.channel_tab_index = tab_index;
    impl_->wake.notify_all();
}

void Service::request_video(std::string video_id, CodecChoice choice)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.video = true;
    impl_->job.video_id = std::move(video_id);
    impl_->job.choice = choice;
    impl_->video_generation++;
    impl_->job.video_generation = impl_->video_generation;
    impl_->video.state = Load::loading;
    impl_->video.stage = "Warteschlange";
    impl_->video.error.clear();
    impl_->video.info = VideoInfo{};
    impl_->video.like_status.clear();
    impl_->video.history_status.clear();
    impl_->video.chapters.clear();
    impl_->video.channel_avatar.clear();
    impl_->video.subscribed = false;
    impl_->video.source_client.clear();
    impl_->video.attempts.clear();
    impl_->video.revision++;
    impl_->video_wait_s = 0.0f;
    impl_->video.waiting_s = 0.0f;
    impl_->video_running = true;
    impl_->video_holding = false;
    impl_->wake.notify_all();
}

void Service::video_watchdog(float dt)
{
    VideoView current;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->video_running || impl_->video.state != Load::loading)
        {
            impl_->video_wait_s = 0.0f;
            impl_->video_holding = false;
            return;
        }
        if (impl_->video_holding)
        {
            impl_->video_wait_s = 0.0f; // a known wait, not a hang
            return;
        }
        impl_->video_wait_s += dt;
        impl_->video.waiting_s = impl_->video_wait_s; // progress, no revision bump
        current = impl_->video;
    }

    // A request that neither arrives nor fails would leave the player screen
    // on "loading" forever: after 20 s the frame thread ends it and whatever
    // the worker reports later is dropped.
    if (impl_->video_wait_s > 20.0f)
    {
        yt::log::line("video: watchdog timeout after %.0f s (stage=%s)", impl_->video_wait_s,
                      current.stage.c_str());
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->video_generation++;
        impl_->video.state = Load::failed;
        impl_->video.error = "timeout";
        impl_->video.stage.clear();
        impl_->video_running = false;
        impl_->video.revision++;
        impl_->video_wait_s = 0.0f;
    }
}

namespace
{

struct DetachedGet
{
    std::vector<std::string> urls;
    std::vector<http::Header> headers;
};

void *run_detached_get(void *argument)
{
    std::unique_ptr<DetachedGet> job(static_cast<DetachedGet *>(argument));
    for (const std::string &url : job->urls)
    {
        log::line("history: watchtime url %s", url.c_str());
        const http::Response response = http::get(url, job->headers, 8);
        log::line("history: watchtime ping -> %ld %s", response.status, response.error.c_str());
    }
    return nullptr;
}

} // namespace

void Service::report_watch_progress(double position_s)
{
    DetachedGet *job = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        Impl::WatchSession &watch = impl_->watch;
        if (watch.watchtime_url.empty() || watch.token.empty() ||
            position_s <= watch.reported_s + 1.0)
            return;
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - watch.started).count();
        const std::string ping =
            watch::watchtime_ping(watch.watchtime_url, watch.cpn, watch.reported_s, position_s,
                                  watch.length_s, Impl::tv_watch_client(), elapsed);
        job = new DetachedGet{{}, Impl::watch_headers(watch.token)};
        if (watch::account_host(ping) != ping)
            job->urls.push_back(ping); // the stats host the answer named
        job->urls.push_back(watch::account_host(ping));
        watch.reported_s = position_s;
    }
    // Off the frame thread: the request may take a second.
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
    {
        delete job;
        return;
    }
    (void)pthread_attr_setstacksize(&attributes, 1u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, &run_detached_get, job) == 0)
        pthread_detach(thread);
    else
        delete job;
    pthread_attr_destroy(&attributes);
}

void Service::request_close_video()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    // Dropping the current video also abandons a worker that is still trying
    // clients: the bumped generation makes it stop before the next stream is
    // opened and keeps it from publishing the player afterwards.
    impl_->video_generation++;
    impl_->video_running = false;
    impl_->job.close_video = true;
    impl_->wake.notify_all();
}

void Service::request_sign_in()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.sign_in = true;
    impl_->login_cancel = false;
    impl_->login_running = false;
    log::line("login: requested");
    impl_->wake.notify_all();
}

void Service::cancel_sign_in()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->login_cancel = true;
    if (!impl_->login_running && !impl_->previous_token.empty())
    {
        // "Add account" left before its sign-in started: back to the old one.
        impl_->restore_previous = true;
        impl_->wake.notify_all();
    }
    if (!impl_->login_running && impl_->login.state != Load::idle)
    {
        // Nothing in flight: answer the frame right away.
        impl_->login.state = Load::idle;
        impl_->login.error.clear();
        impl_->login.revision++;
    }
    log::line("login: cancelled (running=%d)", impl_->login_running ? 1 : 0);
    impl_->wake.notify_all();
}

std::vector<Service::AccountInfo> Service::saved_accounts() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<AccountInfo> out;
    const std::string &current = impl_->auth.refresh_token();
    for (const Impl::SavedAccount &account : impl_->saved)
        out.push_back({account.name, account.photo, account.token == current});
    return out;
}

void Service::add_account()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.add_account = true;
    impl_->login_cancel = false;
    // The login screen must not ask for a second sign-in of its own while the
    // worker is still signing the current account out.
    impl_->login.state = Load::loading;
    impl_->login.user_code.clear();
    impl_->login.revision++;
    impl_->wake.notify_all();
}

void Service::switch_account(int index)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.switch_account = true;
    impl_->job.switch_index = index;
    impl_->wake.notify_all();
}

void Service::sign_out()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.sign_out = true;
    impl_->wake.notify_all();
}

void Service::import_token_file()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.import_token = true;
    impl_->wake.notify_all();
}

bool Service::take_token_restored()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const bool restored = impl_->token_restored;
    impl_->token_restored = false;
    return restored;
}

bool Service::export_token(std::string *where)
{
    std::string token;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        token = impl_->auth.token_json();
    }
    if (token.empty())
    {
        if (where != nullptr)
            *where = "not signed in";
        return false;
    }

    // The install folder first: it survives an uninstall or a wiped sandbox
    // and the user already copies files there. A sandboxed title usually
    // cannot write it, which is why the second copy exists.
    for (const std::string &path : {impl_->bundled_token_path(), impl_->sandbox_backup_path()})
    {
        if (path.empty())
            continue;
        if (hui::save::write_atomic(path, token).empty())
        {
            if (where != nullptr)
                *where = path;
            return true;
        }
    }
    if (where != nullptr)
        *where = impl_->install_root + "/yt-token.json";
    return false;
}

void Service::refresh_token()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->job.refresh = true;
    impl_->wake.notify_all();
}

std::uint64_t Service::revision(FeedKind kind) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    switch (kind)
    {
    case FeedKind::home:
        return impl_->home.revision;
    case FeedKind::subscriptions:
        return impl_->subscriptions.revision;
    case FeedKind::related:
        return impl_->related.revision;
    case FeedKind::channel:
        return impl_->channel_feed.revision;
    case FeedKind::history:
        return impl_->history.revision;
    case FeedKind::topic:
        return impl_->topic.revision;
    case FeedKind::library:
        return impl_->library.revision;
    default:
        return impl_->search.revision;
    }
}

FeedView Service::feed(FeedKind kind) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    switch (kind)
    {
    case FeedKind::home:
        return impl_->filtered(impl_->home, kind);
    case FeedKind::subscriptions:
        return impl_->filtered(impl_->subscriptions, kind);
    case FeedKind::related:
        return impl_->filtered(impl_->related, kind);
    case FeedKind::channel:
        return impl_->channel_feed;
    case FeedKind::history:
        return impl_->filtered(impl_->history, kind);
    case FeedKind::topic:
        return impl_->filtered(impl_->topic, kind);
    case FeedKind::library:
        return impl_->filtered(impl_->library, kind);
    default:
        return impl_->filtered(impl_->search, FeedKind::search);
    }
}

std::uint64_t Service::search_revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->search.revision;
}

FeedView Service::search_feed() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->filtered(impl_->search, FeedKind::search);
}

void Service::set_preferences(const Preferences &preferences)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->preferences == preferences)
        return;
    impl_->preferences = preferences;
    // The filtered views changed: every screen rebuilds its cards.
    for (FeedView *view : {&impl_->home, &impl_->subscriptions, &impl_->related, &impl_->history,
                           &impl_->topic, &impl_->library, &impl_->search})
        view->revision++;
}

std::string Service::search_query() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->search_query;
}

std::uint64_t Service::channel_revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->channel.revision;
}

ChannelView Service::channel_view() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->channel;
}

std::uint64_t Service::video_revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->video.revision;
}

VideoView Service::video() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    VideoView view = impl_->video;
    // The frame thread reads the formats for the quality and audio-track
    // menus only: the long stream URLs and cipher strings are dropped from its
    // copy (a one-character url keeps them "playable" for the selector).
    for (Format &format : view.info.formats)
    {
        format.url = format.url.empty() ? std::string() : std::string("-");
        format.cipher_url.clear();
        format.cipher_s.clear();
        format.cipher_sp.clear();
    }
    return view;
}

std::uint64_t Service::login_revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->login.revision;
}

LoginView Service::login() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->login;
}

void Service::inject_feed(FeedKind kind, std::vector<FeedItem> items, std::string continuation)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    FeedView &view = *impl_->slot(kind);
    view.state = Load::ready;
    view.items = std::move(items);
    view.continuation = std::move(continuation);
    view.error.clear();
    view.revision++;
}

void Service::inject_topic(std::string browse_id, std::vector<FeedItem> items)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->topic_id = std::move(browse_id);
    }
    inject_feed(FeedKind::topic, std::move(items));
}

void Service::inject_channels(FeedKind kind, std::vector<ChannelItem> channels)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    FeedView &view = *impl_->slot(kind);
    view.channels = std::move(channels);
    view.revision++;
}

void Service::inject_channel_page(ChannelView view, std::vector<FeedItem> items)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    view.revision = impl_->channel.revision + 1;
    impl_->channel = std::move(view);

    FeedView &feed = impl_->channel_feed;
    feed.state = Load::ready;
    feed.items = std::move(items);
    feed.continuation.clear();
    feed.error.clear();
    feed.revision++;
}

void Service::inject_login(LoginView view)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    view.revision = impl_->login.revision + 1;
    impl_->login = std::move(view);
}

void Service::inject_video(VideoView view, std::unique_ptr<Player> player)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    view.revision = impl_->video.revision + 1;
    impl_->video = std::move(view);
    impl_->player = std::move(player);
    impl_->player_open = impl_->player != nullptr;
}

std::unique_ptr<Player> Service::take_player()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return std::move(impl_->player);
}

bool Service::has_token() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->auth.signed_in();
}

bool Service::signed_in() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->signed_in;
}

std::string Service::last_error() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->last_error;
}

} // namespace yt
