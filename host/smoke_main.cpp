// youtube-tv - Live smoke test for the network layer (manual, needs internet).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: yt_smoke [video id]
//        yt_smoke clients [video id]   - per-client playback outcome
//        yt_smoke channel [channel id] - header, tabs and two tab counts
//        yt_smoke login                - the device flow stays in flight
//
// 1. Opens a device code (the code the TV would show) and prints it.
// 2. Fetches the player response for the video and lists the adaptive
//    formats it found - no token, anonymous, like a fresh install.
// 3. Prints which stream the codec switches select, so the selection logic is
//    proven against real formats, not only against the test fixtures.
//
// `clients` prints why the six-client player chain answered or not, so a
// console-side failure can be attributed to one client. `channel` proves a
// channel page and one tab switch against the live service.
//
// Nothing is stored: the auth file lives in the temporary directory and the
// device flow is never polled.

#include "yt/auth.hpp"
#include "yt/codec.hpp"
#include "yt/innertube.hpp"
#include "yt/service.hpp"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>

namespace
{

void print_format(const yt::Format &format)
{
    std::printf("  itag %-4d %-5s %4dx%-5d %8lld bps", format.itag, yt::codec_label(format.codec),
                format.width, format.height, format.bitrate);
    if (format.sample_rate > 0)
        std::printf("  %d Hz", format.sample_rate);
    std::printf("\n");
}

void print_pick(const char *what, const yt::Format *format)
{
    if (format == nullptr)
    {
        std::printf("selected(%s): none\n", what);
        return;
    }
    std::printf("selected(%s): itag %d %s %dp\n", what, format->itag,
                yt::codec_label(format->codec), format->height);
}

} // namespace

// `yt_smoke login` drives the real service through the device flow without a
// phone: it proves the flow stays in flight (the bug that aborted every
// sign-in after 200 ms made the state fall back to idle immediately) and that
// cancelling answers within a second.
int login_flow_probe()
{
    yt::Service service("/tmp/yt_smoke_service_auth.json", "", nullptr);
    service.start();
    service.request_sign_in();

    std::string code;
    for (int waited = 0; waited < 12000; waited += 200)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const yt::LoginView view = service.login();
        if (!view.user_code.empty())
            code = view.user_code;
        if (view.state == yt::Load::failed)
        {
            std::printf("login FAILED: %s\n", view.error.c_str());
            service.stop();
            return 1;
        }
        if (!code.empty() && waited > 4000)
            break; // the flow is up and stayed up: that is the regression
    }
    const yt::LoginView during = service.login();
    std::printf("login: state=%d user_code=%s verification=%s\n", static_cast<int>(during.state),
                during.user_code.c_str(), during.verification_url.c_str());
    if (code.empty() || during.state != yt::Load::loading)
    {
        std::printf("login FAILED: the flow did not stay in flight (state=%d, code=%s)\n",
                    static_cast<int>(during.state), code.c_str());
        service.stop();
        return 1;
    }

    service.cancel_sign_in();
    for (int waited = 0; waited < 4000; waited += 200)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (service.login().state == yt::Load::idle)
        {
            std::printf("login: cancel answered after %d ms\n", waited + 200);
            service.stop();
            std::printf("login smoke OK\n");
            return 0;
        }
    }
    std::printf("login FAILED: cancel was not answered\n");
    service.stop();
    return 1;
}

// `yt_smoke clients`: one player() call runs the whole six-client chain. The
// chain records each client's outcome into last_error(), so printing it tells
// a console-side failure apart from a network-wide bot gate.
int clients_probe(const std::string &video_id)
{
    yt::InnerTube tube;
    yt::VideoInfo info;
    const bool played = tube.player(video_id, &info);

    std::printf("clients: video=%s played=%s status=%ld\n", video_id.c_str(),
                played ? "ok" : "FAILED", tube.last_status());

    // The per-client outcome is collected by the chain; print it verbatim,
    // one "clients:" line per reported line so the orchestrator can grep all.
    const std::string &error = tube.last_error();
    for (std::size_t start = 0; start < error.size();)
    {
        const std::size_t end = error.find('\n', start);
        const std::size_t length = end == std::string::npos ? error.size() - start : end - start;
        if (length > 0)
            std::printf("clients: %s\n", error.substr(start, length).c_str());
        if (end == std::string::npos)
            break;
        start = end + 1;
    }

    if (played)
    {
        std::printf("clients: formats=%zu\n", info.formats.size());
        for (std::size_t index = 0; index < info.formats.size() && index < 3; ++index)
        {
            const yt::Format &format = info.formats[index];
            std::printf("clients: format %zu itag=%d codec=%s height=%d\n", index + 1, format.itag,
                        yt::codec_label(format.codec), format.height);
        }
    }
    return played ? 0 : 1;
}

// `yt_smoke channel`: one channel page (header + tabs + selected tab items)
// then a switch to the second tab, proving both browse shapes live.
int channel_probe(const std::string &channel_id)
{
    yt::InnerTube tube;
    yt::ChannelPage page;
    if (!tube.channel_page(channel_id, &page))
    {
        std::printf("channel: FAILED id=%s status=%ld error=%s\n", channel_id.c_str(),
                    tube.last_status(), tube.last_error().c_str());
        return 1;
    }

    std::printf("channel: id=%s name=%s tabs=%zu first_tab_items=%zu\n", channel_id.c_str(),
                page.channel.name.c_str(), page.tabs.size(), page.items.size());
    for (std::size_t index = 0; index < page.tabs.size(); ++index)
        std::printf("channel: tab %zu %s%s\n", index, page.tabs[index].title.c_str(),
                    page.tabs[index].selected ? " selected" : "");

    if (page.tabs.size() < 2)
    {
        std::printf("channel: FAILED only %zu tab(s), cannot switch\n", page.tabs.size());
        return 1;
    }

    yt::FeedPage second;
    if (!tube.channel_tab(channel_id, page.tabs[1].params, &second))
    {
        std::printf("channel: FAILED tab[1]=%s status=%ld error=%s\n", page.tabs[1].title.c_str(),
                    tube.last_status(), tube.last_error().c_str());
        return 1;
    }
    std::printf("channel: tab[1] %s items=%zu\n", page.tabs[1].title.c_str(), second.items.size());
    return 0;
}

// `yt_smoke variants` asks every client for the same video and prints one line
// per answering client: that is the list the player walks when a stream URL is
// refused by googlevideo (403).
int variants_probe(const std::string &video_id)
{
    yt::InnerTube tube;
    tube.set_timeout(12);
    std::vector<yt::VideoInfo> variants;
    std::string error;
    const bool any = tube.player_variants(video_id, &variants, &error);
    std::printf("variants: video=%s clients=%zu error=%s\n", video_id.c_str(), variants.size(),
                error.c_str());
    for (std::size_t index = 0; index < variants.size(); ++index)
    {
        const yt::VideoInfo &info = variants[index];
        yt::CodecChoice choice;
        const yt::Format *pick = yt::select_video_format(info.formats, choice);
        std::printf("variants: [%zu] client=%s formats=%zu pick=%s %dp\n", index,
                    info.client.c_str(), info.formats.size(),
                    pick != nullptr ? yt::codec_label(pick->codec) : "none",
                    pick != nullptr ? pick->height : 0);
    }
    return any ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::string(argv[1]) == "login")
        return login_flow_probe();
    if (argc > 1 && std::string(argv[1]) == "clients")
        return clients_probe(argc > 2 ? argv[2] : "aqz-KE-bpKQ");
    if (argc > 1 && std::string(argv[1]) == "channel")
        return channel_probe(argc > 2 ? argv[2] : "UCm9COMxXKm05BQWNv-IpyPg");
    if (argc > 1 && std::string(argv[1]) == "variants")
        return variants_probe(argc > 2 ? argv[2] : "aqz-KE-bpKQ");

    const std::string video_id = argc > 1 ? argv[1] : "aqz-KE-bpKQ";

    // 1. The device code the sign-in screen would show.
    yt::Auth auth("/tmp/yt_smoke_auth.json");
    const yt::DeviceCode code = auth.begin();
    if (!code.ok())
    {
        std::printf("device code FAILED: %s\n", code.error.c_str());
    }
    else
    {
        std::printf("device code: user_code=%s verification_url=%s interval=%ds\n",
                    code.user_code.c_str(), code.verification_url.c_str(), code.interval_s);
        if (code.user_code.find('-') == std::string::npos || code.user_code.size() < 8)
            std::printf("warning: unexpected user_code shape\n");
    }

    // 2. A live search: the parser against today's real response shape.
    yt::InnerTube tube;
    yt::FeedPage page;
    const bool searched = tube.search("ps5 homebrew", &page);
    std::printf("search: %s, %zu items, continuation=%s\n", searched ? "ok" : "FAILED",
                page.items.size(), page.continuation.empty() ? "none" : "yes");
    for (std::size_t index = 0; index < page.items.size() && index < 5; ++index)
    {
        const yt::FeedItem &item = page.items[index];
        std::printf("  item %zu: %s | %s | %s | %s\n", index + 1, item.video_id.c_str(),
                    item.title.c_str(), item.author.c_str(), item.thumbnail_url.c_str());
    }

    // 3. The player response of one video, anonymously.
    yt::VideoInfo info;
    if (!tube.player(video_id, &info))
    {
        const std::string error = info.error.empty() ? tube.last_error() : info.error;
        if (error == "login_required" && code.ok())
        {
            std::printf("smoke OK (search and device flow live; YouTube refused signed-out "
                        "playback for this network - the console signs in before it plays)\n");
            return 0;
        }
        std::printf("player FAILED (status %ld): %s\n", tube.last_status(), error.c_str());
        return 1;
    }
    std::printf("player: %s | %s | %.0f s | %zu formats\n", info.video_id.c_str(),
                info.title.c_str(), info.length_s, info.formats.size());
    for (const yt::Format &format : info.formats)
        print_format(format);

    int avc = 0;
    int vp9 = 0;
    int av1 = 0;
    int opus = 0;
    for (const yt::Format &format : info.formats)
    {
        avc += format.codec == yt::Codec::avc ? 1 : 0;
        vp9 += format.codec == yt::Codec::vp9 ? 1 : 0;
        av1 += format.codec == yt::Codec::av1 ? 1 : 0;
        opus += format.codec == yt::Codec::opus ? 1 : 0;
    }
    std::printf("codecs: avc=%d vp9=%d av1=%d opus=%d\n", avc, vp9, av1, opus);

    // 4. The codec switches pick from exactly these formats.
    yt::CodecChoice choice;
    print_pick("all codecs, 1080p cap", yt::select_video_format(info.formats, choice));
    choice.vp9 = false;
    print_pick("VP9 off", yt::select_video_format(info.formats, choice));
    choice.av1 = false;
    print_pick("VP9+AV1 off", yt::select_video_format(info.formats, choice));
    choice.avc = false;
    print_pick("VP9+AV1+AVC off", yt::select_video_format(info.formats, choice));
    print_pick("audio", yt::select_audio_format(info.formats));

    if (!searched || page.items.empty())
    {
        std::printf("smoke FAILED: the live search returned no parsable items\n");
        return 1;
    }
    const bool ok = avc > 0 && vp9 > 0 && !info.formats.empty();
    if (ok)
    {
        std::printf("smoke OK\n");
        return 0;
    }
    // YouTube answers the bot gate to some networks/IPs for every client; the
    // parsing itself is covered by the fixtures, and the console signs in
    // before it plays. Keep that case apart from a broken pipeline.
    const std::string error = info.error.empty() ? tube.last_error() : info.error;
    if (error == "login_required" && code.ok())
    {
        std::printf("smoke OK (search and device flow live; YouTube refused signed-out playback "
                    "for this network - the console signs in before it plays)\n");
        return 0;
    }
    std::printf("smoke FAILED: no formats and no bot gate (error '%s')\n", error.c_str());
    return 1;
}
