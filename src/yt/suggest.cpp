// youtube-tv - Search suggestions: YouTube's autocomplete for what is typed.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/suggest.hpp"

#include "yt/http.hpp"
#include "yt/log.hpp"

#include "third_party/yyjson/yyjson.h"

#include <pthread.h>

#include <condition_variable>
#include <mutex>
#include <utility>

namespace yt
{

std::vector<std::string> parse_suggestions(std::string_view json)
{
    std::vector<std::string> out;
    // Some clients wrap the array in a callback: start at the first '['.
    const std::size_t open = json.find('[');
    if (open == std::string_view::npos)
        return out;
    json.remove_prefix(open);
    if (const std::size_t close = json.rfind(']'); close != std::string_view::npos)
        json = json.substr(0, close + 1);
    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
        return out;
    yyjson_val *list = yyjson_arr_get(yyjson_doc_get_root(doc), 1);
    for (std::size_t i = 0, n = yyjson_arr_size(list); i < n && out.size() < 10; ++i)
    {
        yyjson_val *entry = yyjson_arr_get(list, i);
        // ["text", 0, [...]] in the youtube client's wording, "text" in firefox's.
        if (yyjson_is_arr(entry))
            entry = yyjson_arr_get(entry, 0);
        if (yyjson_is_str(entry) && *yyjson_get_str(entry) != '\0')
            out.emplace_back(yyjson_get_str(entry));
    }
    yyjson_doc_free(doc);
    return out;
}

namespace
{

std::string encode(std::string_view text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.' ||
            byte == '~')
            out += ch;
        else
        {
            out += '%';
            out += hex[byte >> 4];
            out += hex[byte & 15];
        }
    }
    return out;
}

} // namespace

struct Suggest::Impl
{
    mutable std::mutex mutex;
    std::condition_variable wake;
    bool quit = false;
    bool started = false;
    std::string wanted;   // the newest query asked for
    bool pending = false; // ... and not answered yet
    std::string answered; // the query of `answer`
    std::vector<std::string> answer;
    std::uint64_t revision = 0;
    std::string hl = "en";
    std::string gl = "US";

    void run()
    {
        for (;;)
        {
            std::string query;
            std::string url;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return quit || pending; });
                if (quit)
                    return;
                pending = false;
                query = wanted;
                url = "https://suggestqueries-clients6.youtube.com/complete/search?client=youtube"
                      "&ds=yt&xhr=t&hl=" +
                      encode(hl) + "&gl=" + encode(gl) + "&q=" + encode(query);
            }
            const http::Response response = http::get(url, {}, 6);
            std::vector<std::string> found;
            if (response.http_ok())
                found = parse_suggestions(response.body);
            else
                log::line("suggest: %ld %s", response.status, response.error.c_str());
            std::lock_guard<std::mutex> lock(mutex);
            if (query != wanted)
                continue; // typed on meanwhile: the newer query is asked next
            answered = query;
            answer = std::move(found);
            revision++;
        }
    }
};

namespace
{

void *suggest_main(void *argument)
{
    std::unique_ptr<std::shared_ptr<Suggest::Impl>> impl(
        static_cast<std::shared_ptr<Suggest::Impl> *>(argument));
    (*impl)->run();
    return nullptr;
}

} // namespace

Suggest::Suggest() : impl_(std::make_shared<Impl>())
{
}

Suggest::~Suggest()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->quit = true;
    impl_->wake.notify_all();
}

void Suggest::set_region(std::string hl, std::string gl)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->hl = std::move(hl);
    impl_->gl = std::move(gl);
}

void Suggest::ask(const std::string &query)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (query == impl_->wanted && (impl_->pending || impl_->answered == query))
        return;
    impl_->wanted = query;
    if (query.empty())
    {
        impl_->pending = false;
        impl_->answered.clear();
        impl_->answer.clear();
        impl_->revision++;
        return;
    }
    impl_->pending = true;
    if (!impl_->started)
    {
        // The thread starts with the first query: no thread for a session
        // that never searches.
        impl_->started = true;
        pthread_attr_t attributes;
        if (pthread_attr_init(&attributes) == 0)
        {
            pthread_attr_setstacksize(&attributes, 512u << 10);
            auto *argument = new std::shared_ptr<Impl>(impl_);
            pthread_t thread;
            if (pthread_create(&thread, &attributes, &suggest_main, argument) == 0)
                pthread_detach(thread);
            else
            {
                delete argument;
                impl_->started = false;
            }
            pthread_attr_destroy(&attributes);
        }
    }
    impl_->wake.notify_all();
}

std::vector<std::string> Suggest::answer(std::string *query) const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (query != nullptr)
        *query = impl_->answered;
    return impl_->answer;
}

std::uint64_t Suggest::revision() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->revision;
}

} // namespace yt
