// youtube-tv - Headless host renderer: runs the app's screens on the PC and
// writes PNG files, so the layout can be reviewed without a console.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// usage: hui_snapshots <assets dir> <output dir> [screen id|all] [width height]
//
// The app runs with injected fixture data: no network, no player. The service
// is never started, and the video screen draws a synthetic preview frame.

#include "app/app.hpp"
#include "app/screens.hpp"
#include "app/theme.hpp"
#include "audio/mixer.hpp"
#include "core/save_file.hpp"
#include "gfx/gl_program.hpp"
#include "gfx/renderer.hpp"
#include "ui/fonts.hpp"
#include "yt/log.hpp"
#include "yt/service.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/glcorearb.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace
{

constexpr int kFrameWidth = 1920;
constexpr int kFrameHeight = 1080;

bool open_context()
{
    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    EGLDisplay display =
        get_platform_display != nullptr
            ? get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)
            : eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major = 0;
    EGLint minor = 0;
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, &major, &minor) ||
        !eglBindAPI(EGL_OPENGL_API))
        return false;
    const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION,
                                         4,
                                         EGL_CONTEXT_MINOR_VERSION,
                                         5,
                                         EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                         EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                         EGL_NONE};
    EGLContext context =
        eglCreateContext(display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, context_attributes);
    return context != EGL_NO_CONTEXT &&
           eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context);
}

bool load_font(hui::gfx::Renderer &renderer, const std::string &path, hui::gfx::Font *font,
               hui::ui::FontRef *ref)
{
    std::string data;
    if (!hui::save::read_file(path, &data) || !font->load(data))
    {
        std::fprintf(stderr, "cannot load font %s\n", path.c_str());
        return false;
    }
    ref->font = font;
    ref->texture = renderer.batch().create_font_texture(*font);
    return true;
}

std::vector<yt::FeedItem> feed_fixture()
{
    const char *titles[] = {
        "Ad-free: how playback works through InnerTube",
        "PS5 homebrew in 2026 - what is possible",
        "DualSense haptics: rumble, triggers and the light bar",
        "AV1, VP9, AVC: what the software decoder does on the console",
        "A long video about codecs and bitrates, part 3",
        "Tokyo at night in 4K HDR",
        "Twelve recipes in twenty minutes",
        "Road bike service: indexing the gears in nine steps",
        "Rust for the impatient - the first two hours",
        "Mahler 5, third movement, live from the concert hall",
        "Camera comparison: full frame against medium format",
        "How a fridge works (and why it drips)",
    };
    const char *authors[] = {"PS5 Homebrew", "TechChannel",   "Digital Foundry DE", "Code & Coffee",
                             "TravelStuff",  "KitchenStudio", "BikeWorks",          "LearnChannel",
                             "ConcertHall",  "PhotoPractice", "HomeKnowHow",        "MakerBarn"};
    std::vector<yt::FeedItem> items;
    for (int index = 0; index < 12; ++index)
    {
        yt::FeedItem item;
        char id[16];
        std::snprintf(id, sizeof(id), "fixture%04d", index);
        item.video_id = id;
        item.title = titles[index];
        item.author = authors[index];
        item.length_s = 92.0 + index * 137.0;
        item.live = index == 3;
        item.detail = std::to_string(12 + index * 37) + "K views \xE2\x80\xA2 " +
                      std::to_string(index + 1) + " days ago";
        item.watched = index % 4 == 1 ? 0.35f : -1.0f;
        items.push_back(std::move(item));
    }
    return items;
}

// A synthetic preview player so the video screen's layout can be reviewed on
// the PC (there is no decoding host build). It draws colour bars.
class PreviewPlayer final : public yt::Player
{
  public:
    void set_decode_mode(yt::DecodeMode mode) override
    {
        // The preview has no decoder; the switch is accepted and ignored.
        (void)mode;
    }

    bool open(const std::string &video_url, const std::string &audio_url,
              yt::AudioSink *sink) override
    {
        (void)audio_url;
        (void)sink;
        open_ = !video_url.empty();
        clock_ = 0.0;
        return open_;
    }
    void close() override
    {
        open_ = false;
    }
    bool running() const override
    {
        return open_;
    }
    double duration_s() const override
    {
        return 1548.0;
    }
    std::string quality_label() const override
    {
        return "Preview (PC)";
    }
    void update() override
    {
        if (open_ && !paused_)
            clock_ += 1.0 / 60.0;
        // The bars shift slightly every frame, like a real decoder would.
        if (open_)
            paint();
    }
    yt::VideoFrameView poll_frame() override
    {
        yt::VideoFrameView view;
        if (!open_)
            return view;
        view.rgba = pixels_.data();
        view.width = width_;
        view.height = height_;
        view.pts = clock_;
        view.fresh = fresh_;
        fresh_ = false;
        return view;
    }
    double clock_s() const override
    {
        return clock_;
    }
    void seek(double seconds) override
    {
        clock_ = seconds;
    }
    void set_paused(bool paused) override
    {
        paused_ = paused;
    }
    bool paused() const override
    {
        return paused_;
    }
    double buffered_s() const override
    {
        return 12.0;
    }
    bool ended() const override
    {
        return clock_ >= duration_s();
    }
    const std::string &error() const override
    {
        return error_;
    }

  private:
    void paint()
    {
        static const std::uint8_t bars[8][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255},
                                                {0, 255, 0},     {255, 0, 255}, {255, 0, 0},
                                                {0, 0, 255},     {16, 16, 16}};
        for (int y = 0; y < height_; ++y)
        {
            for (int x = 0; x < width_; ++x)
            {
                const int bar = x * 8 / width_;
                std::uint8_t *pixel =
                    pixels_.data() + (static_cast<std::size_t>(y) * width_ + x) * 4;
                const float shade = 0.55f + 0.45f * (1.0f - static_cast<float>(y) / height_);
                pixel[0] = static_cast<std::uint8_t>(bars[bar][0] * shade);
                pixel[1] = static_cast<std::uint8_t>(bars[bar][1] * shade);
                pixel[2] = static_cast<std::uint8_t>(bars[bar][2] * shade);
                pixel[3] = 255;
            }
        }
        fresh_ = true;
    }

    bool open_ = true; // injected ready: the screen shows the preview at once
    bool paused_ = false;
    bool fresh_ = true;
    double clock_ = 0.0;
    int width_ = 1280;
    int height_ = 720;
    std::vector<std::uint8_t> pixels_ =
        std::vector<std::uint8_t>(static_cast<std::size_t>(1280 * 720 * 4), 0);
    std::string error_;
};

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: %s <assets dir> <output dir> [screen id|all] [width height]\n",
                     argv[0]);
        return 2;
    }
    const std::string assets = argv[1];
    const std::string output = argv[2];
    const std::string only = argc > 3 && std::string(argv[3]) != "all" ? argv[3] : "";
    const int width = argc > 5 ? std::atoi(argv[4]) : kFrameWidth;
    const int height = argc > 5 ? std::atoi(argv[5]) : kFrameHeight;

    if (!open_context())
    {
        std::fprintf(stderr, "no surfaceless EGL OpenGL 4.5 context\n");
        return 1;
    }
    std::fprintf(stderr, "GL %s / %s\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                 reinterpret_cast<const char *>(glGetString(GL_RENDERER)));
    hui::gfx::set_glsl_prefix("#version 450 core\n");

    hui::gfx::Renderer renderer;
    hui::gfx::Font regular;
    hui::gfx::Font semibold;
    hui::gfx::Font display;
    hui::gfx::Font mono;
    hui::ui::Fonts fonts;
    if (!renderer.init() ||
        !load_font(renderer, assets + "/fonts/google-sans-regular.huifont", &regular,
                   &fonts.regular) ||
        !load_font(renderer, assets + "/fonts/google-sans-medium.huifont", &semibold,
                   &fonts.semibold) ||
        !load_font(renderer, assets + "/fonts/google-sans-bold.huifont", &display,
                   &fonts.display) ||
        !load_font(renderer, assets + "/fonts/dejavu-sans-mono.huifont", &mono, &fonts.mono))
        return 1;

    const std::string data_root = output + "/.data";
    hui::save::ensure_directory(output);
    hui::save::ensure_directory(data_root);
    std::remove((data_root + "/settings.bin").c_str());

    // The mixer is a sink for the decoded audio; nothing starts an audio
    // thread on the PC, so the ring just collects whatever a player writes.
    hui::audio::Mixer mixer;
    hui::audio::StreamRing ring(1u << 14);
    mixer.attach_stream(1, &ring);

    yt::Service service(data_root + "/auth.json", assets, nullptr);
    const std::vector<yt::FeedItem> items = feed_fixture();
    // The search results carry channels next to the videos.
    yt::FeedPage search_page;
    search_page.items = items;
    search_page.channels = {{"UCfixture0001", "PS5 Homebrew", {}, "128K subscribers"},
                            {"UCfixture0002", "Digital Foundry DE", {}, "874K subscribers"},
                            {"UCfixture0003", "Code & Coffee", {}, "41K subscribers"}};
    // Home as SmartTube shows it: rows per shelf.
    std::vector<yt::FeedItem> shelves = items;
    for (std::size_t i = 0; i < shelves.size(); ++i)
        shelves[i].shelf = i < 6 ? "Recommended" : (i < 9 ? "Trending" : "Music");
    service.inject_feed(yt::FeedKind::home, shelves, "continuation-fixture");
    service.inject_feed(yt::FeedKind::history, items, {});
    service.inject_feed(yt::FeedKind::subscriptions, items, {});
    service.inject_channels(yt::FeedKind::search, search_page.channels);
    // The channel page the search screen opens: the first card's id must match
    // so the screen keeps this page instead of requesting one (there is no
    // worker in the snapshot run).
    yt::ChannelView channel_page;
    channel_page.state = yt::Load::ready;
    channel_page.channel.channel_id = search_page.channels[0].channel_id;
    channel_page.channel.name = search_page.channels[0].name;
    channel_page.channel.subscribers = search_page.channels[0].subscribers;
    channel_page.tabs = {{"Home", "EghmZWF0dXJlZPIGBAoCEgA%3D", true},
                         {"Videos", "EgZ2aWRlb3PyBgQKAjoA", false},
                         {"Live", "EgVsaXZlc_IGBAoCSgA%3D", false}};
    channel_page.selected_tab = 0;
    service.inject_channel_page(channel_page, items);
    service.inject_feed(yt::FeedKind::search, items, {});
    service.inject_feed(yt::FeedKind::related, items, {});

    yt::LoginView login;
    login.state = yt::Load::loading;
    login.user_code = "JZC-NSJ-SJHY";
    login.verification_url = "https://www.google.com/device";
    service.inject_login(login);

    yt::VideoView video;
    video.state = std::getenv("YT_SNAPSHOT_FAIL") != nullptr ? yt::Load::failed : yt::Load::ready;
    video.error = std::getenv("YT_SNAPSHOT_FAIL") != nullptr ? "timeout" : std::string();
    // A real timeout carries the seconds it waited; the screen shows the log
    // tail from three seconds on.
    video.waiting_s = std::getenv("YT_SNAPSHOT_FAIL") != nullptr ? 21.0f : 0.0f;
    video.info.video_id = "fixture0000";
    video.info.title = "Ad-free: how playback works through InnerTube";
    video.info.author = "PS5 Homebrew";
    video.info.length_s = 1548.0;
    if (std::getenv("YT_SNAPSHOT_FAIL") != nullptr)
    {
        yt::log::line("video: request id=aqz-KE-bpKQ cap=1080 codecs avc=1 vp9=1 av1=1");
        yt::log::line("video: player info ok id=aqz-KE-bpKQ title=fixture formats=5 length=1548");
        yt::log::line("video: selected itag=248 1920x1080 VP9 audio=251");
        yt::log::line("video: open begin");
        yt::log::line("video: watchdog timeout after 20 s (stage=Decoder)");
        service.inject_video(video, nullptr);
    }
    else
    {
        service.inject_video(video, std::make_unique<PreviewPlayer>());
    }

    hui::app::App app(fonts, hui::app::App::load_settings(data_root), data_root, renderer.batch(),
                      service);
    app.set_version("host");
    app.begin_tour(only);

    GLuint framebuffer = 0;
    GLuint color = 0;
    glGenFramebuffers(1, &framebuffer);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return 1;

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
    stbi_flip_vertically_on_write(1);
    bool ok = true;
    std::size_t pictures = 0;
    for (int frame = 0; frame < 20000 && !app.tour_finished(); ++frame)
    {
        const hui::InputFrame input = app.tour_step(1.0f / 60.0f);
        app.update(input, 1.0f / 60.0f);
        if (app.tour_capture().empty())
        {
            // Frames without a picture still record their draw lists, so lazy
            // text layout and thumbnail lookups stay on the frame's budget.
            app.rehearse();
            continue;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        app.compose(renderer);
        renderer.present(framebuffer, width, height);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (std::size_t i = 3; i < pixels.size(); i += 4)
            pixels[i] = 255;
        const std::string path = output + "/" + app.tour_capture() + ".png";
        ok = stbi_write_png(path.c_str(), width, height, 4, pixels.data(), width * 4) != 0 && ok;
        std::fprintf(stderr, "wrote %s: %zu shapes, %zu draw calls\n", path.c_str(),
                     renderer.last_instances(), renderer.last_draw_calls());
        ++pictures;
        app.tour_capture_done();
    }
    if (pictures == 0)
    {
        std::fprintf(stderr, "no pictures: unknown screen id '%s'?\n", only.c_str());
        return 1;
    }
    for (const std::string &line : app.tour_report())
        std::fprintf(stderr, "%s\n", line.c_str());
    return ok ? 0 : 1;
}
