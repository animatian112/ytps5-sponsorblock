// youtube-tv - Subtitles: json3 timed text.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/captions.hpp"

#include "yt/http.hpp"
#include "yt/log.hpp"

#include "third_party/yyjson/yyjson.h"

#include <pthread.h>

#include <mutex>
#include <utility>

namespace yt::captions
{

bool parse_json3(std::string_view body, std::vector<Cue> *out)
{
    out->clear();
    yyjson_doc *doc = yyjson_read(body.data(), body.size(), 0);
    if (doc == nullptr)
        return false;
    yyjson_val *events = yyjson_obj_get(yyjson_doc_get_root(doc), "events");
    if (!yyjson_is_arr(events))
    {
        yyjson_doc_free(doc);
        return false;
    }
    for (std::size_t i = 0, n = yyjson_arr_size(events); i < n; ++i)
    {
        yyjson_val *event = yyjson_arr_get(events, i);
        yyjson_val *segs = yyjson_obj_get(event, "segs");
        if (!yyjson_is_arr(segs))
            continue;
        std::string text;
        for (std::size_t j = 0, m = yyjson_arr_size(segs); j < m; ++j)
        {
            yyjson_val *utf8 = yyjson_obj_get(yyjson_arr_get(segs, j), "utf8");
            if (yyjson_is_str(utf8))
                text += yyjson_get_str(utf8);
        }
        // Auto-generated tracks send bare newlines to scroll their window.
        if (text.find_first_not_of(" \n") == std::string::npos)
            continue;
        while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
            text.pop_back();
        Cue cue;
        cue.start_s = yyjson_get_num(yyjson_obj_get(event, "tStartMs")) / 1000.0;
        cue.end_s = cue.start_s + yyjson_get_num(yyjson_obj_get(event, "dDurationMs")) / 1000.0;
        cue.text = std::move(text);
        out->push_back(std::move(cue));
    }
    yyjson_doc_free(doc);
    return true;
}

struct Loader::Shared
{
    std::mutex mutex;
    std::uint64_t generation = 0;
    std::vector<Cue> cues;
    std::string status;
};

namespace
{

struct Job
{
    std::shared_ptr<Loader::Shared> shared;
    std::uint64_t generation = 0;
    std::string url;
};

void *fetch(void *argument)
{
    std::unique_ptr<Job> job(static_cast<Job *>(argument));
    std::string url = job->url;
    url += url.find('?') == std::string::npos ? "?fmt=json3" : "&fmt=json3";
    const http::Response response = http::get(url, {}, 15);
    std::vector<Cue> cues;
    std::string status;
    if (!response.ok())
        status = "error: " + response.error;
    else if (response.status != 200)
        status = "error: HTTP " + std::to_string(response.status);
    else if (response.body.empty())
        status = "error: empty answer";
    else if (!parse_json3(response.body, &cues))
        status = "error: unreadable answer";
    else
        status = "ok";
    log::line("captions: %s (%zu cues, %zu bytes)", status.c_str(), cues.size(),
              response.body.size());
    std::lock_guard<std::mutex> lock(job->shared->mutex);
    if (job->generation != job->shared->generation)
        return nullptr;
    job->shared->cues = std::move(cues);
    job->shared->status = std::move(status);
    return nullptr;
}

} // namespace

Loader::Loader() : shared_(std::make_shared<Shared>())
{
}

Loader::~Loader()
{
    clear();
}

void Loader::clear()
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    ++shared_->generation;
    shared_->cues.clear();
    shared_->status.clear();
}

void Loader::load(const std::string &track_url)
{
    auto *job = new Job;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        job->generation = ++shared_->generation;
        shared_->cues.clear();
        shared_->status = "loading";
    }
    job->shared = shared_;
    job->url = track_url;
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0)
    {
        delete job;
        return;
    }
    (void)pthread_attr_setstacksize(&attributes, 1u << 20);
    pthread_t thread;
    if (pthread_create(&thread, &attributes, &fetch, job) == 0)
        pthread_detach(thread);
    else
        delete job;
    pthread_attr_destroy(&attributes);
}

std::string Loader::text_at(double position_s)
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    std::string out;
    for (const Cue &cue : shared_->cues)
    {
        if (cue.start_s > position_s)
            break;
        if (position_s < cue.end_s)
        {
            if (!out.empty())
                out += '\n';
            out += cue.text;
        }
    }
    return out;
}

std::string Loader::status()
{
    std::lock_guard<std::mutex> lock(shared_->mutex);
    return shared_->status;
}

} // namespace yt::captions
