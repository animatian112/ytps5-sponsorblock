// youtube-tv - YouTube's own "n" and signature code, run in QuickJS.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yt/js_solver.hpp"

#include "core/save_file.hpp"
#include "yt/http.hpp"
#include "yt/log.hpp"

#include "../../third_party/quickjs/quickjs.h"

#include <pthread.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <utility>

namespace yt
{

namespace
{

// The player is parsed by a recursive-descent parser: its own thread, with a
// stack far beyond the service worker's.
constexpr std::size_t kJsStack = 32u << 20;
constexpr std::size_t kJsStackLimit = kJsStack - (2u << 20);
constexpr std::size_t kJsMemoryLimit = 256u << 20;
constexpr std::size_t kMaxPlayerBytes = 12u << 20;
constexpr const char *kPlayerUserAgent = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version";

// Runs `work` on a thread of its own with the big stack, and waits for it.
void run_with_big_stack(const std::function<void()> &work)
{
    struct Box
    {
        const std::function<void()> *work;
    } box{&work};
    pthread_attr_t attributes;
    bool threaded = false;
    pthread_t thread{};
    if (pthread_attr_init(&attributes) == 0)
    {
        pthread_attr_setstacksize(&attributes, kJsStack);
        threaded = pthread_create(
                       &thread, &attributes,
                       [](void *argument) -> void *
                       {
                           (*static_cast<Box *>(argument)->work)();
                           return nullptr;
                       },
                       &box) == 0;
        pthread_attr_destroy(&attributes);
    }
    if (threaded)
        pthread_join(thread, nullptr);
    else
        work(); // no thread: on this one, with QuickJS's own stack check
}

std::string file_key(const std::string &url)
{
    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : url)
    {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    char name[40];
    std::snprintf(name, sizeof name, "player-%016llx.js", static_cast<unsigned long long>(hash));
    return name;
}

std::string exception_text(JSContext *context)
{
    JSValue exception = JS_GetException(context);
    std::string text = "exception";
    if (const char *message = JS_ToCString(context, exception))
    {
        text = message;
        JS_FreeCString(context, message);
    }
    JS_FreeValue(context, exception);
    if (text.size() > 300)
        text.resize(300);
    return text;
}

} // namespace

std::string url_n_value(const std::string &url)
{
    // Query form: "...&n=VALUE&..." (or "?n=").
    for (std::size_t at = url.find("n="); at != std::string::npos; at = url.find("n=", at + 2))
    {
        if (at == 0 || (url[at - 1] != '&' && url[at - 1] != '?'))
            continue;
        const std::size_t start = at + 2;
        const std::size_t end = url.find('&', start);
        return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    // Path form: ".../n/VALUE/...".
    const std::size_t at = url.find("/n/");
    if (at != std::string::npos)
    {
        const std::size_t start = at + 3;
        const std::size_t end = url.find('/', start);
        if (end != std::string::npos)
            return url.substr(start, end - start);
    }
    return {};
}

std::string url_with_n(const std::string &url, const std::string &value)
{
    for (std::size_t at = url.find("n="); at != std::string::npos; at = url.find("n=", at + 2))
    {
        if (at == 0 || (url[at - 1] != '&' && url[at - 1] != '?'))
            continue;
        const std::size_t start = at + 2;
        std::size_t end = url.find('&', start);
        if (end == std::string::npos)
            end = url.size();
        return url.substr(0, start) + value + url.substr(end);
    }
    const std::size_t at = url.find("/n/");
    if (at != std::string::npos)
    {
        const std::size_t start = at + 3;
        const std::size_t end = url.find('/', start);
        if (end != std::string::npos)
            return url.substr(0, start) + value + url.substr(end);
    }
    return url;
}

struct JsSolver::Impl
{
    std::string solver_dir;
    std::string cache_dir;
    JSRuntime *runtime = nullptr;
    JSContext *context = nullptr;
    bool solver_loaded = false;
    std::string player;                         // the player readied
    std::map<std::string, std::string> answers; // challenge cache ("n:..." / "s:...")

    ~Impl()
    {
        if (context != nullptr)
            JS_FreeContext(context);
        if (runtime != nullptr)
            JS_FreeRuntime(runtime);
    }

    // Evaluates `code` as a global script; false with *error on an exception.
    bool eval(const std::string &code, const char *name, std::string *error,
              std::string *result = nullptr)
    {
        JSValue value = JS_Eval(context, code.c_str(), code.size(), name, JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(value))
        {
            *error = std::string(name) + ": " + exception_text(context);
            return false;
        }
        if (result != nullptr)
        {
            if (const char *text = JS_ToCString(context, value))
            {
                *result = text;
                JS_FreeCString(context, text);
            }
        }
        JS_FreeValue(context, value);
        return true;
    }

    bool ensure_engine(std::string *error)
    {
        if (runtime == nullptr)
        {
            runtime = JS_NewRuntime();
            if (runtime == nullptr)
            {
                *error = "no JS runtime";
                return false;
            }
            JS_SetMemoryLimit(runtime, kJsMemoryLimit);
            context = JS_NewContext(runtime);
            if (context == nullptr)
            {
                *error = "no JS context";
                return false;
            }
        }
        JS_UpdateStackTop(runtime);
        JS_SetMaxStackSize(runtime, kJsStackLimit);
        if (solver_loaded)
            return true;
        std::string lib;
        std::string core;
        if (!hui::save::read_file(solver_dir + "/yt.solver.lib.js", &lib, 4u << 20) ||
            !hui::save::read_file(solver_dir + "/yt.solver.core.js", &core, 1u << 20))
        {
            *error = "solver scripts missing in " + solver_dir;
            return false;
        }
        if (!eval(lib, "yt.solver.lib.js", error) ||
            !eval("globalThis.meriyah = lib.meriyah; globalThis.astring = lib.astring;", "glue",
                  error) ||
            !eval(core, "yt.solver.core.js", error))
            return false;
        solver_loaded = true;
        return true;
    }

    // Runs the preprocessed player: the n and sig functions land in __r.
    bool load_prepared(const std::string &prepared, std::string *error)
    {
        JSValue global = JS_GetGlobalObject(context);
        JS_SetPropertyStr(context, global, "__prepared",
                          JS_NewStringLen(context, prepared.data(), prepared.size()));
        JS_FreeValue(context, global);
        const bool ok = eval("var __r = {n: null, sig: null};"
                             "Function('_result', __prepared)(__r);"
                             "__prepared = undefined;",
                             "prepared player", error);
        JS_RunGC(runtime);
        return ok;
    }

    bool preprocess(const std::string &player_js, std::string *prepared, std::string *error)
    {
        JSValue global = JS_GetGlobalObject(context);
        JS_SetPropertyStr(context, global, "__player",
                          JS_NewStringLen(context, player_js.data(), player_js.size()));
        JS_FreeValue(context, global);
        const bool ok = eval("(function () {"
                             "  var out = jsc({type: 'player', player: __player, requests: [],"
                             "                 output_preprocessed: true});"
                             "  __player = undefined;"
                             "  return out.preprocessed_player || '';"
                             "})()",
                             "preprocess", error, prepared);
        JS_RunGC(runtime);
        if (ok && prepared->empty())
        {
            *error = "the solver found nothing in the player";
            return false;
        }
        return ok;
    }

    std::string call(const char *which, const std::string &challenge, std::string *error)
    {
        const std::string key = std::string(which) + ":" + challenge;
        if (const auto found = answers.find(key); found != answers.end())
            return found->second;
        std::string out;
        run_with_big_stack(
            [&]
            {
                JS_UpdateStackTop(runtime);
                JSValue global = JS_GetGlobalObject(context);
                JSValue holder = JS_GetPropertyStr(context, global, "__r");
                JSValue function = JS_GetPropertyStr(context, holder, which);
                if (!JS_IsFunction(context, function))
                    *error = std::string("no ") + which + " function in this player";
                else
                {
                    JSValue argument = JS_NewStringLen(context, challenge.data(), challenge.size());
                    JSValue value = JS_Call(context, function, JS_UNDEFINED, 1, &argument);
                    JS_FreeValue(context, argument);
                    if (JS_IsException(value))
                        *error = std::string(which) + ": " + exception_text(context);
                    else if (const char *text = JS_ToCString(context, value))
                    {
                        out = text;
                        JS_FreeCString(context, text);
                    }
                    JS_FreeValue(context, value);
                }
                JS_FreeValue(context, function);
                JS_FreeValue(context, holder);
                JS_FreeValue(context, global);
            });
        if (!out.empty())
        {
            if (answers.size() > 256)
                answers.clear();
            answers[key] = out;
        }
        return out;
    }
};

JsSolver::JsSolver(std::string solver_dir, std::string cache_dir) : impl_(std::make_unique<Impl>())
{
    impl_->solver_dir = std::move(solver_dir);
    impl_->cache_dir = std::move(cache_dir);
}

JsSolver::~JsSolver() = default;

std::string JsSolver::cache_name(const std::string &player_url)
{
    return file_key(player_url);
}

const std::string &JsSolver::player_url() const
{
    return impl_->player;
}

bool JsSolver::prepare(const std::string &player_url, const std::string &player_js,
                       std::string *error)
{
    std::string ignored;
    if (error == nullptr)
        error = &ignored;
    if (player_url.empty())
    {
        *error = "no player URL";
        return false;
    }
    if (player_url == impl_->player)
        return true;
    const auto started = std::chrono::steady_clock::now();
    const std::string cached =
        impl_->cache_dir.empty() ? std::string() : impl_->cache_dir + "/" + file_key(player_url);
    bool ok = false;
    bool from_cache = false;
    std::string js = player_js;
    run_with_big_stack(
        [&]
        {
            if (!impl_->ensure_engine(error))
                return;
            std::string prepared;
            if (!cached.empty() && hui::save::read_file(cached, &prepared, 24u << 20))
            {
                from_cache = impl_->load_prepared(prepared, error);
                if (from_cache)
                {
                    ok = true;
                    return;
                }
                std::remove(cached.c_str()); // a damaged copy: made again below
            }
            if (js.empty())
            {
                http::Request request;
                request.url = player_url;
                request.headers = {{"User-Agent", kPlayerUserAgent}};
                request.timeout_s = 20;
                request.max_bytes = kMaxPlayerBytes;
                const http::Response response = http::perform(request);
                if (!response.error.empty() || !response.http_ok())
                {
                    *error = "player JS: " + (response.error.empty()
                                                  ? "HTTP " + std::to_string(response.status)
                                                  : response.error);
                    return;
                }
                js = response.body;
            }
            if (!impl_->preprocess(js, &prepared, error) || !impl_->load_prepared(prepared, error))
                return;
            if (!cached.empty())
            {
                (void)hui::save::ensure_directory(impl_->cache_dir);
                (void)hui::save::write_atomic(cached, prepared);
            }
            ok = true;
        });
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!ok)
    {
        log::line("js: player %s not readied in %.1f s: %s", player_url.c_str(), seconds,
                  error->c_str());
        return false;
    }
    log::line("js: player %s ready in %.1f s (%s)", player_url.c_str(), seconds,
              from_cache ? "kept copy" : "parsed");
    impl_->player = player_url;
    impl_->answers.clear();
    return true;
}

std::string JsSolver::solve_n(const std::string &challenge, std::string *error)
{
    std::string ignored;
    return impl_->call("n", challenge, error != nullptr ? error : &ignored);
}

std::string JsSolver::solve_sig(const std::string &challenge, std::string *error)
{
    std::string ignored;
    return impl_->call("sig", challenge, error != nullptr ? error : &ignored);
}

std::string JsSolver::transform_url(const std::string &url, std::string *error)
{
    const std::string n = url_n_value(url);
    if (n.empty())
        return url;
    const std::string solved = solve_n(n, error);
    if (solved.empty())
        return {};
    return url_with_n(url, solved);
}

} // namespace yt
