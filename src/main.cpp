// youtube-tv - Application entry point.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Opens the display, controller and audio (with one extra stream slot for the
// video's sound), then runs the app every frame: read input, update, play the
// sounds it asked for, draw, present. Lifecycle markers and frame pacing are
// logged for hardware runs.

#include "app/app.hpp"
#include "app/emoji_textures.hpp"
#include "app/icons.hpp"
#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/stream_ring.hpp"
#include "core/frame_stats.hpp"
#include "core/input.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "core/version.hpp"
#include "gfx/renderer.hpp"
#include "platform/ps5/audio_out.hpp"
#include "platform/ps5/display_egl.hpp"
#include "platform/ps5/pad.hpp"
#include "platform/ps5/system.hpp"
#include "ui/fonts.hpp"
#include "yt/log.hpp"
#include "yt/service.hpp"

#include <GL/glcorearb.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace hui::ps5
{
// src/platform/ps5/videodec_probe.cpp: how many decoder imports the loader
// bound (0 when the console keeps the library from homebrew titles).
std::size_t report_decoder_imports();
} // namespace hui::ps5

namespace
{

constexpr const char *kAssets = "/app0/assets";
// The folder the app was started from: the install folder holds yt-token.json
// (the token backup a user carries over a re-upload) next to the app files.
constexpr const char *kInstallRoot = "/app0";
constexpr const char *kDataRoot = "/download0/hui";
constexpr const char *kDevRoot = "/download0/hui/dev";
constexpr const char *kRequestPath = "/app0/dev/request.txt";
constexpr double kCollectSeconds = 240.0;
constexpr int kCaptureWidth = 960;
constexpr int kCaptureHeight = 540;

// The video's sound goes into the mixer's second stream slot; the player
// writes decoded 48 kHz stereo float frames into it. write/buffered/drain run
// on the frame thread (the player's own thread) while Mixer::render_chunk
// reads the ring on the audio thread; the ring keeps one writer per index, so
// the two sides never touch the same step.
class RingSink final : public yt::AudioSink
{
  public:
    explicit RingSink(hui::audio::StreamRing &ring) : ring_(ring)
    {
    }
    std::size_t write(const float *stereo, std::size_t frames) override
    {
        const std::size_t written = ring_.write(stereo, frames);
        // The first batch at once, then one line a second at most: the ring's
        // write and read counts name where it stood when yt.log stops around a
        // crash, without logging every decoded batch.
        const std::int64_t now = hui::sys::monotonic_us();
        if (now - last_ring_log_us_ >= 1000000)
        {
            last_ring_log_us_ = now;
            yt::log::line("ffmpeg: audio ring write=%llu read=%llu buffered=%zu",
                          static_cast<unsigned long long>(ring_.total_written()),
                          static_cast<unsigned long long>(ring_.total_read()), ring_.available());
        }
        return written;
    }
    std::size_t buffered() const override
    {
        return ring_.available();
    }
    void drain() override
    {
        ring_.drain();
    }
    void set_paused(bool paused) override
    {
        ring_.set_paused(paused);
    }

  private:
    hui::audio::StreamRing &ring_;
    std::int64_t last_ring_log_us_ = 0;
};

bool open_display(hui::ps5::Display &display, int *resolution)
{
    using hui::Settings;
    const Settings::Resolution &mode = Settings::kResolutions[*resolution];
    if (display.open(mode.width, mode.height))
    {
        hui::sys::log("[YTV] display mode %s %dx%d", mode.label, display.width(), display.height());
        return true;
    }
    if (*resolution == 0)
        return false;
    hui::sys::log("[YTV] display mode %s failed, using 1080p", mode.label);
    *resolution = 0;
    return display.open(Settings::kResolutions[0].width, Settings::kResolutions[0].height);
}

bool load_font(hui::gfx::Renderer &renderer, const char *name, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    const std::string path = std::string(kAssets) + "/fonts/" + name;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        hui::sys::log("[YTV] font %s failed: %s", name, font->error().c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

// The frame as drawn, bottom row first, as a 24-bit BMP (tour pictures).
bool save_picture(const std::string &path, int width, int height)
{
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::uint32_t row = (static_cast<std::uint32_t>(width) * 3 + 3) & ~3u;
    const std::uint32_t size = 54 + row * static_cast<std::uint32_t>(height);
    unsigned char header[54] = {'B', 'M'};
    const auto put = [&](int at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
            header[at + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, size);
    put(10, 54);
    put(14, 40);
    put(18, static_cast<std::uint32_t>(width));
    put(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put(34, size - 54);
    std::FILE *file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
        return false;
    bool ok = std::fwrite(header, 1, sizeof(header), file) == sizeof(header);
    std::vector<unsigned char> line(row);
    for (int y = 0; ok && y < height; ++y)
    {
        const unsigned char *in = pixels.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x)
        {
            line[static_cast<std::size_t>(x) * 3 + 0] = in[x * 4 + 2];
            line[static_cast<std::size_t>(x) * 3 + 1] = in[x * 4 + 1];
            line[static_cast<std::size_t>(x) * 3 + 2] = in[x * 4 + 0];
        }
        ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
    }
    return std::fclose(file) == 0 && ok;
}

} // namespace

int main()
{
    using namespace hui;
    sys::log("[YTV] entry");
    ps5::report_decoder_imports();
    const bool storage = save::ensure_directory(kDataRoot);
    save::ensure_directory(kDevRoot);
    yt::log::open("/download0/hui/dev/yt.log");
    sys::log("[YTV] storage dir=%d", storage ? 1 : 0);
    yt::log::line("app: start version=%s build=%s",
                  read_content_version("/app0/sce_sys/param.json").c_str(), yt::log::build_stamp());

    const Settings saved = app::App::load_settings(kDataRoot);
    int resolution = ps5::Display::supports_display_modes() ? saved.resolution : 0;
    ps5::Display display;
    if (!open_display(display, &resolution))
    {
        sys::log("[YTV] fatal: display open failed");
        sys::park();
    }

    gfx::Renderer renderer;
    gfx::Font regular;
    gfx::Font semibold;
    gfx::Font display_font;
    gfx::Font mono;
    ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, "google-sans-regular.huifont", &regular, &fonts.regular) ||
        !load_font(renderer, "google-sans-medium.huifont", &semibold, &fonts.semibold) ||
        !load_font(renderer, "google-sans-bold.huifont", &display_font, &fonts.display) ||
        !load_font(renderer, "dejavu-sans-mono.huifont", &mono, &fonts.mono))
    {
        sys::log("[YTV] fatal: renderer init failed");
        sys::park();
    }

    ps5::Pad pad;
    pad.open();
    InputTracker tracker;

    audio::Mixer mixer;
    audio::StreamRing video_ring(1u << 16); // ~1.3 s of stereo audio
    mixer.attach_stream(1, &video_ring);
    mixer.set_stream_gain(0, 0.0f, 0.0f); // no music in this app
    mixer.set_stream_gain(1, Settings::gain(saved.video_volume), 0.0f);
    ps5::AudioOut audio_out;
    audio_out.start(mixer);
    audio::SoundBank sounds;
    const auto bank = sounds.load(std::string(kAssets) + "/audio/sfx");
    sys::log("[YTV] sounds files=%d rejected=%d", bank.files, bank.rejected);
    // A sound pack of the user's own: WAVs named like the cues ("select_01.wav",
    // "focus_01.wav", ...) in /download0/hui/sfx replace the packaged ones.
    const auto own = sounds.load_overrides(std::string(kDataRoot) + "/sfx", audio::SoundSet::glass);
    if (own.files > 0 || own.rejected > 0)
        yt::log::line("app: own sounds %d (rejected %d)", own.files, own.rejected);

    yt::log::line("app: display %dx%d, audio %s", display.width(), display.height(),
                  audio_out.errors() == 0 ? "up" : "degraded");
    RingSink sink(video_ring);
    // The install folder (/app0) holds the token backup a user can carry over
    // a re-upload; the sandbox copy lives in /download0 and survives reboots.
    yt::Service service(std::string(kDataRoot) + "/auth.json", kInstallRoot, &sink);
    app::init_icons(renderer.batch(), std::string(kAssets) + "/icons");
    app::init_emoji(renderer.batch(), std::string(kAssets) + "/emoji/twemoji.pack");
    app::App application(fonts, saved, kDataRoot, renderer.batch(), service);
    const std::string version = read_content_version("/app0/sce_sys/param.json");
    application.set_version(version.empty() ? std::string(yt::log::build_stamp())
                                            : version + "  " + yt::log::build_stamp());
    application.start();
    sys::log("[YTV] version %s screens=%zu", version.empty() ? "unknown" : version.c_str(),
             application.screen_count());

    // A tour request turns this launch into an unattended validation run:
    // every screen is shown with scripted input, pictures and a report are
    // written to kDevRoot, and the app closes itself a while later.
    std::unique_ptr<app::App> tour_owner; // the app itself, kept alive for the tour
    (void)tour_owner;
    bool touring = false;
    save::ensure_directory(kDevRoot);
    const std::string handled_path = std::string(kDevRoot) + "/handled.txt";
    const std::string report_path = std::string(kDevRoot) + "/tour/report.txt";
    bool quit_requested = false;
    const auto poll_request = [&]()
    {
        std::string request;
        if (!save::read_file(kRequestPath, &request, 256))
            return;
        char verb[16] = {};
        char argument[64] = {};
        char token[64] = {};
        if (std::sscanf(request.c_str(), "%15s %63s %63s", verb, argument, token) != 3)
            return;
        std::string handled;
        save::read_file(handled_path, &handled, 128);
        if (handled == token)
            return;
        save::write_atomic(handled_path, token);
        if (std::strcmp(verb, "quit") == 0)
        {
            quit_requested = true;
        }
        else if (std::strcmp(verb, "tour") == 0 && !touring)
        {
            save::ensure_directory(std::string(kDevRoot) + "/tour");
            std::remove(report_path.c_str());
            const bool all = std::strcmp(argument, "all") == 0;
            application.begin_tour(all ? std::string() : std::string(argument));
            touring = true;
            sys::log("[YTV] tour requested: %s token=%s", argument, token);
        }
    };
    poll_request();
    double collect_wait = -1.0;

    std::uint64_t frames = 0;
    FrameStats stats;
    double fps_seconds = 0.0;
    int fps_frames = 0;
    PadSample samples[64];
    gfx::Canvas capture;
    std::int64_t last_frame_start = sys::monotonic_us();
    bool splash_hidden = false;
    for (;;)
    {
        const std::int64_t now = hui::sys::monotonic_us();
        float dt = frames == 0 ? 1.0f / 60.0f : static_cast<float>(now - last_frame_start) / 1e6f;
        last_frame_start = now;
        if (dt > 0.05f)
            dt = 0.05f;
        const std::size_t count = pad.read(samples);
        InputFrame input = tracker.update(std::span<const PadSample>(samples, count),
                                          static_cast<std::uint64_t>(now));
        if (touring)
        {
            input = application.tour_step(dt);
            if (!application.tour_capture().empty())
            {
                if (capture.texture() == 0)
                    capture.create(kCaptureWidth, kCaptureHeight, 1);
            }
        }

        application.telemetry().push(dt * 1000.0f);
        application.telemetry().uptime += dt;
        application.update(input, dt);
        if (application.take_settings_changed())
        {
            const Settings &settings = application.settings();
            // The video's decoded audio is mixed on the music bus (slot 1); the
            // bus stays open and the stream's own gain carries the volume, so
            // muting the bus here would silence every codec.
            mixer.set_bus_gain(audio::Bus::music, 1.0f);
            mixer.set_bus_gain(audio::Bus::sfx, Settings::gain(settings.sfx_volume));
            mixer.set_bus_gain(audio::Bus::ui, Settings::gain(settings.ui_volume));
            mixer.set_stream_gain(1, Settings::gain(settings.video_volume), 0.2f);
            InputSettings input_settings = tracker.settings();
            input_settings.swap_confirm = settings.swap_confirm;
            tracker.set_settings(input_settings);
        }

        const ui::Feedback &feedback = application.feedback();
        for (const audio::CueEvent &event : feedback.cues)
            sounds.play(mixer, application.sound_set(), event);
        const Settings &settings = application.settings();
        if (settings.haptics && feedback.rumble_strength > 0.0f)
            pad.rumble(feedback.rumble_strength, feedback.rumble_seconds);
        pad.tick(dt);
        if (settings.light_bar)
        {
            const gfx::Color accent = application.accent();
            pad.set_light_bar(static_cast<std::uint8_t>(accent.r * 255.0f),
                              static_cast<std::uint8_t>(accent.g * 255.0f),
                              static_cast<std::uint8_t>(accent.b * 255.0f));
        }

        application.compose(renderer);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        renderer.present(0, display.width(), display.height());
        if (!splash_hidden)
        {
            splash_hidden = true;
            sys::hide_splash_screen();
        }

        if (touring && !application.tour_capture().empty())
        {
            capture.bind();
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            renderer.present(capture.framebuffer(), kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, capture.framebuffer());
            const std::string path =
                std::string(kDevRoot) + "/tour/" + application.tour_capture() + ".bmp";
            const bool ok = save_picture(path, kCaptureWidth, kCaptureHeight);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            sys::log("[YTV] tour picture %s ok=%d", application.tour_capture().c_str(), ok ? 1 : 0);
            application.tour_capture_done();
            last_frame_start = sys::monotonic_us();
        }
        display.swap();

        ++frames;
        const std::int64_t after = sys::monotonic_us();
        const float frame_ms = static_cast<float>(after - now) / 1000.0f;
        if (touring)
        {
            application.tour_record(frame_ms, renderer.last_draw_calls(),
                                    renderer.last_instances());
            if (application.tour_finished() && collect_wait < 0.0)
            {
                std::string report;
                for (const std::string &line : application.tour_report())
                    report += line + "\n";
                save::write_atomic(report_path, report);
                collect_wait = 0.0;
                sys::log("[YTV] tour finished, %zu lines", application.tour_report().size());
            }
        }
        stats.add(static_cast<double>(frame_ms));
        fps_seconds += static_cast<double>(dt);
        ++fps_frames;
        if (fps_seconds >= 0.5)
        {
            application.telemetry().fps = static_cast<float>(fps_frames / fps_seconds);
            application.telemetry().average_ms = static_cast<float>(stats.mean_ms());
            fps_seconds = 0.0;
            fps_frames = 0;
        }
        (void)after;
        if (collect_wait >= 0.0)
            collect_wait += static_cast<double>(dt);
        if (frames % 60 == 0)
        {
            poll_request();
            if (quit_requested || collect_wait > kCollectSeconds || application.wants_quit())
            {
                sys::log("[YTV] closing: %s",
                         quit_requested ? "quit requested"
                                        : (collect_wait > kCollectSeconds ? "tour window over"
                                                                          : "user request"));
                pad.close();
                sys::quit();
            }
        }
        if (stats.count() == 600)
        {
            char summary[160];
            stats.format(summary, sizeof(summary));
            sys::log("[YTV] %s draws=%zu shapes=%zu", summary, renderer.last_draw_calls(),
                     renderer.last_instances());
            sys::log("[YTV] audio grains=%llu errors=%llu voices=%d underruns=%llu",
                     static_cast<unsigned long long>(audio_out.grains()),
                     static_cast<unsigned long long>(audio_out.errors()), mixer.active_voices(),
                     static_cast<unsigned long long>(mixer.stream_underruns()));
            // The same in yt.log while the count grows: gaps in the video's
            // sound and how slow the frames were around them.
            static unsigned long long last_underruns = 0;
            const unsigned long long underruns = mixer.stream_underruns();
            if (underruns != last_underruns)
            {
                yt::log::line("audio: %llu new gaps in 10 s (%s)", underruns - last_underruns,
                              summary);
                last_underruns = underruns;
            }
            stats.reset();
        }
    }
}
