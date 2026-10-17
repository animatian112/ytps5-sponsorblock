// youtube-tv - SponsorBlock: the segment worker and the skip rules.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/sponsorblock.hpp"

#include "yt/http.hpp"
#include "yt/log.hpp"

#include "third_party/sponsorblock/sponsorblock.h"

#include <pthread.h>

#include <condition_variable>
#include <mutex>
#include <utility>

namespace yt::sponsorblock
{
namespace
{

constexpr std::size_t kWorkerStack = 1u << 20; // console threads need an explicit stack
constexpr long kTimeoutS = 10;

sb_action action_for(Mode mode)
{
    switch (mode)
    {
    case Mode::ask:
        return SB_ACT_SHOW_DIALOG;
    case Mode::mark:
        return SB_ACT_DO_NOTHING;
    case Mode::skip:
    default:
        return SB_ACT_SKIP_WITH_TOAST;
    }
}

sb_config make_config(unsigned categories, Mode mode)
{
    sb_config config;
    sb_config_defaults(&config);
    config.categories = categories;
    for (int i = 0; i < SB_CAT_COUNT; ++i)
        config.action[i] = action_for(mode);
    config.skip_once = 1;
    config.min_skip_ms = 1000; // a sub-second "segment" is never worth a seek
    return config;
}

sb_action raw_action(Action action)
{
    switch (action)
    {
    case Action::skip:
        return SB_ACT_SKIP_ONLY;
    case Action::ask:
        return SB_ACT_SHOW_DIALOG;
    case Action::notify:
        return SB_ACT_SKIP_WITH_TOAST;
    case Action::show:
    case Action::off:
    default:
        return SB_ACT_DO_NOTHING;
    }
}

sb_config make_config(const Options &options)
{
    sb_config config;
    sb_config_defaults(&config);
    config.categories = 0;
    for (int i = 0; i < kCategoryCount && i < SB_CAT_COUNT; ++i)
    {
        config.action[i] = raw_action(options.action[i]);
        if (options.action[i] != Action::off)
            config.categories |= 1u << i;
    }
    config.skip_once = options.skip_once ? 1 : 0;
    config.min_skip_ms = options.min_skip_ms;
    config.use_alt_server = options.alt_server ? 1 : 0;
    return config;
}

void *report_main(void *argument)
{
    std::string *url = static_cast<std::string *>(argument);
    const http::Response response = http::post_json(*url, {}, std::string(), kTimeoutS);
    log::line("sponsorblock: viewed -> %ld %s", response.status, response.error.c_str());
    delete url;
    return nullptr;
}

} // namespace

const char *category_id(int index)
{
    return index >= 0 && index < SB_CAT_COUNT ? sb_category_name(1u << index) : "";
}

const char *category_label(int index)
{
    static const char *const kLabels[kCategoryCount] = {
        "Sponsor",          "Unpaid/Self promotion", "Interaction reminder", "Intermission/Intro",
        "Endcards/Credits", "Preview/Recap",         "Non-music section",    "Tangents/Jokes",
        "Highlight",        "Exclusive access"};
    return index >= 0 && index < kCategoryCount ? kLabels[index] : "";
}

void report_viewed(const std::string &uuid, bool alt_server)
{
    if (uuid.empty())
        return;
    auto *url = new std::string(alt_server ? SB_SERVER_ALT : SB_SERVER_MAIN);
    *url += "/api/viewedVideoSponsorTime?UUID=" + uuid;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
    {
        delete url;
        return;
    }
    pthread_attr_setstacksize(&attributes, kWorkerStack);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, &report_main, url) == 0)
        pthread_detach(thread);
    else
        delete url;
    pthread_attr_destroy(&attributes);
}

unsigned categories_for(Scope scope)
{
    switch (scope)
    {
    case Scope::sponsors:
        return SB_CAT_SPONSOR;
    case Scope::promotion:
        return SB_CAT_SPONSOR | SB_CAT_SELFPROMO | SB_CAT_INTERACTION;
    case Scope::all:
    default:
        // SmartTube's default: everything with a range, filler excluded.
        return SB_CAT_SPONSOR | SB_CAT_SELFPROMO | SB_CAT_INTERACTION | SB_CAT_INTRO |
               SB_CAT_OUTRO | SB_CAT_PREVIEW | SB_CAT_MUSIC_OFFTOPIC;
    }
}

// The worker is detached and shares this state, so closing the app never waits
// on a request in flight.
struct Skipper::Worker
{
    struct Shared
    {
        std::mutex mutex;
        std::condition_variable wake;
        bool stopping = false;
        // request
        std::uint64_t want_generation = 0;
        std::string want_video;
        unsigned want_categories = 0;
        sb_config want_config{};
        // answer
        std::uint64_t done_generation = 0;
        std::string done_video;
        std::vector<sb_segment> done_segments;
        unsigned done_categories = 0;
        sb_config done_config{};
        std::string done_status;
    };
    std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    bool started = false;

    static void *main(void *argument)
    {
        auto *handle = static_cast<std::shared_ptr<Shared> *>(argument);
        std::shared_ptr<Shared> state = std::move(*handle);
        delete handle;
        std::uint64_t served = 0;
        for (;;)
        {
            std::string video;
            unsigned categories = 0;
            sb_config config{};
            std::uint64_t generation = 0;
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                state->wake.wait(lock, [&]
                                 { return state->stopping || state->want_generation != served; });
                if (state->stopping)
                    return nullptr;
                generation = served = state->want_generation;
                video = state->want_video;
                categories = state->want_categories;
                config = state->want_config;
            }
            std::vector<sb_segment> found;
            std::string status = fetch(video, config, &found);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done_generation = generation;
            state->done_video = std::move(video);
            state->done_segments = std::move(found);
            state->done_categories = categories;
            state->done_config = config;
            state->done_status = std::move(status);
        }
    }

    static std::string fetch(const std::string &video, const sb_config &config,
                             std::vector<sb_segment> *out)
    {
        char url[1024];
        if (sb_build_url(url, sizeof(url), &config, nullptr, video.c_str()) < 0)
            return "error: bad video id";
        const http::Response response = http::get(url, {}, kTimeoutS);
        if (!response.ok())
        {
            log::line("sponsorblock: %s: %s", video.c_str(), response.error.c_str());
            return "error: " + response.error;
        }
        if (response.status == 404)
        {
            log::line("sponsorblock: %s: no segments", video.c_str());
            return "none";
        }
        if (response.status != 200)
        {
            log::line("sponsorblock: %s: HTTP %ld", video.c_str(), response.status);
            return "error: HTTP " + std::to_string(response.status);
        }
        sb_segment_list list{};
        if (sb_parse_segments(response.body.data(), response.body.size(), video.c_str(), &list) !=
            0)
        {
            log::line("sponsorblock: %s: unreadable answer", video.c_str());
            return "error: bad answer";
        }
        out->assign(list.items, list.items + list.count);
        sb_segment_list_free(&list);
        log::line("sponsorblock: %s: %zu segments", video.c_str(), out->size());
        return out->empty() ? "none" : "ok";
    }

    void request(const std::string &video, const sb_config &config)
    {
        if (!started)
        {
            pthread_attr_t attributes;
            if (pthread_attr_init(&attributes) == 0)
            {
                pthread_attr_setstacksize(&attributes, kWorkerStack);
                auto *handle = new std::shared_ptr<Shared>(shared);
                pthread_t thread;
                if (pthread_create(&thread, &attributes, &Worker::main, handle) == 0)
                {
                    pthread_detach(thread);
                    started = true;
                }
                else
                {
                    delete handle;
                }
                pthread_attr_destroy(&attributes);
            }
            if (!started)
                log::line("sponsorblock: no worker thread");
        }
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->want_video = video;
        shared->want_categories = config.categories;
        shared->want_config = config;
        ++shared->want_generation;
        shared->wake.notify_one();
    }

    ~Worker()
    {
        std::lock_guard<std::mutex> lock(shared->mutex);
        shared->stopping = true;
        shared->wake.notify_one();
    }
};

Skipper::Skipper() : worker_(std::make_unique<Worker>())
{
}

Skipper::~Skipper()
{
    clear();
}

void Skipper::clear()
{
    if (tracker_ != nullptr)
    {
        sb_tracker_free(static_cast<sb_tracker *>(tracker_));
        delete static_cast<sb_tracker *>(tracker_);
        tracker_ = nullptr;
    }
    if (!segments_.empty() || !status_.empty())
        ++revision_;
    segments_.clear();
    status_.clear();
    video_id_.clear();
}

void Skipper::load(const std::string &video_id, Scope scope)
{
    clear();
    video_id_ = video_id;
    if (video_id.empty() || !sb_valid_video_id(video_id.c_str()))
        return;
    per_category_ = false;
    worker_->request(video_id, make_config(categories_for(scope), Mode::skip));
    std::lock_guard<std::mutex> lock(worker_->shared->mutex);
    adopted_generation_ = worker_->shared->want_generation - 1;
}

void Skipper::load(const std::string &video_id, const Options &options)
{
    clear();
    video_id_ = video_id;
    per_category_ = true;
    options_ = options;
    if (video_id.empty() || !sb_valid_video_id(video_id.c_str()))
        return;
    const sb_config config = make_config(options);
    if (config.categories == 0)
        return; // every category off: nothing to ask for
    worker_->request(video_id, config);
    std::lock_guard<std::mutex> lock(worker_->shared->mutex);
    adopted_generation_ = worker_->shared->want_generation - 1;
}

double Skipper::highlight_s() const
{
    for (const Segment &segment : segments_)
        if (segment.action == "poi" || segment.category == "poi_highlight")
            return segment.start_s;
    return -1.0;
}

std::string Skipper::whole_video_label() const
{
    for (const Segment &segment : segments_)
        if (segment.action == "full")
            return segment.label;
    return {};
}

Decision Skipper::poll(double position_s, double duration_s, bool playing)
{
    adopt();
    Decision decision;
    if (tracker_ == nullptr || !playing)
        return decision;
    sb_decision raw{};
    const int kind =
        sb_tracker_poll(static_cast<sb_tracker *>(tracker_), static_cast<long>(position_s * 1000.0),
                        static_cast<long>(duration_s * 1000.0), 1.0, &raw);
    if (kind == SB_DECIDE_NONE || raw.segment == nullptr)
        return decision;
    decision.kind = kind == SB_DECIDE_ASK ? Kind::ask : Kind::skip;
    decision.target_s = static_cast<double>(raw.seek_to_ms) / 1000.0;
    decision.label = sb_category_label(raw.segment->category);
    for (int i = 0; i < kCategoryCount; ++i)
        if (raw.segment->category == (1u << i))
            decision.label = category_label(i);
    decision.silent = raw.action == SB_ACT_SKIP_ONLY;
    decision.uuid = raw.segment->uuid;
    return decision;
}

std::uint64_t Skipper::revision() const
{
    return revision_;
}

void Skipper::adopt()
{
    if (video_id_.empty() || tracker_ != nullptr)
        return;
    std::vector<sb_segment> found;
    unsigned categories = 0;
    sb_config adopted_config{};
    {
        Worker::Shared &shared = *worker_->shared;
        std::lock_guard<std::mutex> lock(shared.mutex);
        if (shared.done_generation != shared.want_generation ||
            shared.done_generation == adopted_generation_ || shared.done_video != video_id_)
            return;
        adopted_generation_ = shared.done_generation;
        found = shared.done_segments;
        categories = shared.done_categories;
        adopted_config = shared.done_config;
        status_ = shared.done_status;
    }
    segments_.clear();
    for (const sb_segment &raw : found)
    {
        Segment segment;
        segment.start_s = static_cast<double>(raw.start_ms) / 1000.0;
        segment.end_s = static_cast<double>(raw.end_ms) / 1000.0;
        segment.category = raw.category_name;
        segment.action = raw.action_type;
        segment.uuid = raw.uuid;
        for (int i = 0; i < kCategoryCount; ++i)
            if (raw.category == (1u << i))
                segment.index = i;
        segment.label =
            segment.index >= 0 ? category_label(segment.index) : sb_category_label(raw.category);
        segments_.push_back(std::move(segment));
    }
    sb_segment_list list{found.data(), found.size()};
    const sb_config config =
        per_category_ ? adopted_config : make_config(categories, tracker_mode_);
    auto *tracker = new sb_tracker;
    if (sb_tracker_init(tracker, &config, &list) != 0)
    {
        delete tracker;
        status_ = "error: out of memory";
        return;
    }
    tracker_ = tracker;
    ++revision_;
}

Decision Skipper::poll(double position_s, double duration_s, bool playing, Mode mode)
{
    if (mode != tracker_mode_ && tracker_ != nullptr)
    {
        // The setting changed mid-video: rebuild the rules from the same list.
        auto *tracker = static_cast<sb_tracker *>(tracker_);
        sb_config config = tracker->cfg;
        for (int i = 0; i < SB_CAT_COUNT; ++i)
            config.action[i] = action_for(mode);
        sb_segment_list list = tracker->segs;
        sb_tracker fresh;
        if (sb_tracker_init(&fresh, &config, &list) == 0)
        {
            sb_tracker_free(tracker);
            *tracker = fresh;
        }
    }
    tracker_mode_ = mode;
    adopt();

    Decision decision;
    if (tracker_ == nullptr || !playing || mode == Mode::mark)
        return decision;
    sb_decision raw{};
    const int kind =
        sb_tracker_poll(static_cast<sb_tracker *>(tracker_), static_cast<long>(position_s * 1000.0),
                        static_cast<long>(duration_s * 1000.0), 1.0, &raw);
    if (kind == SB_DECIDE_NONE || raw.segment == nullptr)
        return decision;
    decision.kind = kind == SB_DECIDE_ASK ? Kind::ask : Kind::skip;
    decision.target_s = static_cast<double>(raw.seek_to_ms) / 1000.0;
    decision.label = sb_category_label(raw.segment->category);
    return decision;
}

Decision Skipper::skip_now(double position_s, double duration_s)
{
    Decision decision;
    if (tracker_ == nullptr)
        return decision;
    sb_decision raw{};
    if (sb_tracker_manual(static_cast<sb_tracker *>(tracker_),
                          static_cast<long>(position_s * 1000.0),
                          static_cast<long>(duration_s * 1000.0), &raw) != SB_DECIDE_SKIP ||
        raw.segment == nullptr)
        return decision;
    decision.kind = Kind::skip;
    decision.target_s = static_cast<double>(raw.seek_to_ms) / 1000.0;
    decision.label = sb_category_label(raw.segment->category);
    return decision;
}

} // namespace yt::sponsorblock
