// youtube-tv - Sign-in: OAuth 2.0 device flow (TV code and QR code).
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The public YouTube-TV client. begin() starts the flow, poll() is called on
// the interval the server returns, and the refresh token is kept in
// token_path (a small "v":1 JSON written through the kit's atomic writer) so
// the next launch can refresh silently.

#include "yt/auth.hpp"

#include "core/save_file.hpp"
#include "yt/http.hpp"

#include "third_party/yyjson/yyjson.h"

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>
#include <vector>

namespace yt
{

namespace
{

constexpr const char *kDeviceCodeUrl = "https://oauth2.googleapis.com/device/code";
constexpr const char *kTokenUrl = "https://oauth2.googleapis.com/token";
constexpr const char *kScopeEncoded = "https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fyoutube";
// Google revokes a TV client id when it rotates the app; the live pair is
// published in the TV app's own bundle (the page the TV loads). Fetching it
// is how the maintained third-party clients stay signed in, and it is the
// only way a device flow can still start.
constexpr const char *kDevicePageUrl = "https://www.youtube.com/tv";
constexpr const char *kTvUserAgent = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version";
// Used while the device page cannot be reached; the pair last seen in the
// bundle on 2026-10-10.
constexpr const char *kFallbackClientId =
    "861556708454-d6dlm3lh05idd8npek18k6be8ba3oc68.apps.googleusercontent.com";
constexpr const char *kFallbackClientSecret = "SboVhoG9s0rNafixCSGGKXAT";

// One "<script id=\"base-js\" src=\"...\">" entry of the TV page.
std::string base_script_url(const std::string &html)
{
    const std::string marker = "id=\"base-js\"";
    const std::size_t id = html.find(marker);
    if (id == std::string::npos)
        return {};
    std::size_t src = html.find("src=\"", id);
    if (src == std::string::npos)
        return {};
    src += 5;
    const std::size_t end = html.find('"', src);
    if (end == std::string::npos)
        return {};
    std::string url = html.substr(src, end - src);
    if (url.compare(0, 2, "//") == 0)
        url = "https:" + url;
    else if (url.compare(0, 1, "/") == 0)
        url = "https://www.youtube.com" + url;
    return url;
}

// "clientId:\"<id>\",<anything>:\"<secret>\"" inside the bundle.
bool client_identity(const std::string &bundle, std::string *id, std::string *secret)
{
    const std::string marker = "clientId:\"";
    const std::size_t at = bundle.find(marker);
    if (at == std::string::npos)
        return false;
    const std::size_t id_begin = at + marker.size();
    const std::size_t id_end = bundle.find('"', id_begin);
    if (id_end == std::string::npos)
        return false;
    const std::size_t secret_key = bundle.find(":\"", id_end);
    if (secret_key == std::string::npos)
        return false;
    const std::size_t secret_begin = secret_key + 2;
    const std::size_t secret_end = bundle.find('"', secret_begin);
    if (secret_end == std::string::npos)
        return false;
    const std::string found_id = bundle.substr(id_begin, id_end - id_begin);
    const std::string found_secret = bundle.substr(secret_begin, secret_end - secret_begin);
    if (!found_id.empty() && !found_secret.empty())
    {
        *id = found_id;
        *secret = found_secret;
    }
    return !found_id.empty() && !found_secret.empty();
}

std::string string_field(yyjson_val *obj, const char *key)
{
    yyjson_val *value = yyjson_obj_get(obj, key);
    if (yyjson_is_str(value))
    {
        const char *text = yyjson_get_str(value);
        if (text != nullptr)
            return text;
    }
    return {};
}

long long integer_field(yyjson_val *obj, const char *key, long long fallback)
{
    yyjson_val *value = yyjson_obj_get(obj, key);
    if (yyjson_is_num(value))
        return static_cast<long long>(yyjson_get_num(value));
    if (yyjson_is_str(value))
    {
        const char *text = yyjson_get_str(value);
        if (text != nullptr && *text != '\0')
            return std::strtoll(text, nullptr, 10);
    }
    return fallback;
}

// application/x-www-form-urlencoded escaping for the values we generate.
std::string url_encode_form(std::string_view text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (char ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        if (unreserved)
            out += ch;
        else
        {
            out += '%';
            out += hex[(byte >> 4) & 0x0f];
            out += hex[byte & 0x0f];
        }
    }
    return out;
}

void append_json_string(std::string &out, std::string_view text)
{
    out += '"';
    for (char ch : text)
    {
        const unsigned char byte = static_cast<unsigned char>(ch);
        switch (ch)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20)
            {
                static const char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(byte >> 4) & 0x0f];
                out += hex[byte & 0x0f];
            }
            else
                out += ch;
            break;
        }
    }
    out += '"';
}

std::vector<http::Header> form_headers()
{
    return {http::Header{"Content-Type", "application/x-www-form-urlencoded"}};
}

void set_error(std::string *error, const std::string &message)
{
    if (error != nullptr)
        *error = message;
}

void clear_error(std::string *error)
{
    if (error != nullptr)
        error->clear();
}

} // namespace

struct Auth::Impl
{
    std::string token_path;
    std::string access_token;
    std::string refresh_token;
    std::string client_id; // the pair this session signs in with (scraped)
    std::string client_secret;
    // The pair the current refresh token was issued to. A grant only refreshes
    // with the client it was issued to, and the scraped pair changes when
    // YouTube rotates its TV app: an account linked in another session (the
    // second or third one) failed every refresh after a rotation and looked
    // signed out. Kept beside each token on disk.
    std::string token_client_id;
    std::string token_client_secret;
    struct Known
    {
        std::string token;
        std::string id;
        std::string secret;
    };
    std::vector<Known> known; // <token file>.clients: token TAB id TAB secret
    bool identity_tried = false;

    std::string clients_path() const
    {
        return token_path.empty() ? std::string() : token_path + ".clients";
    }

    void load_known()
    {
        known.clear();
        std::string data;
        if (clients_path().empty() || !hui::save::read_file(clients_path(), &data, 64u << 10))
            return;
        std::size_t at = 0;
        while (at < data.size())
        {
            std::size_t end = data.find('\n', at);
            if (end == std::string::npos)
                end = data.size();
            const std::string line = data.substr(at, end - at);
            at = end + 1;
            const std::size_t a = line.find('\t');
            const std::size_t b = a == std::string::npos ? a : line.find('\t', a + 1);
            if (b == std::string::npos)
                continue;
            known.push_back({line.substr(0, a), line.substr(a + 1, b - a - 1), line.substr(b + 1)});
        }
    }

    void save_known() const
    {
        if (clients_path().empty())
            return;
        std::string data;
        for (const Known &entry : known)
            data += entry.token + '\t' + entry.id + '\t' + entry.secret + '\n';
        (void)hui::save::write_atomic(clients_path(), data);
    }

    // Records which client the token belongs to (at most 8 tokens).
    void remember_client(const std::string &token, const std::string &id, const std::string &secret)
    {
        if (token.empty() || id.empty())
            return;
        token_client_id = id;
        token_client_secret = secret;
        for (Known &entry : known)
        {
            if (entry.token == token)
            {
                if (entry.id == id && entry.secret == secret)
                    return;
                entry.id = id;
                entry.secret = secret;
                save_known();
                return;
            }
        }
        known.push_back({token, id, secret});
        if (known.size() > 8)
            known.erase(known.begin());
        save_known();
    }

    // The recorded client of the current token, when the token file had none.
    void lookup_client()
    {
        if (!token_client_id.empty())
            return;
        for (const Known &entry : known)
        {
            if (entry.token == refresh_token)
            {
                token_client_id = entry.id;
                token_client_secret = entry.secret;
                return;
            }
        }
    }

    // Fetches the TV page and its bundle once; falls back to the last known
    // pair. Cached for the process lifetime.
    void ensure_identity()
    {
        if (identity_tried)
            return;
        identity_tried = true;

        const auto page = http::get(kDevicePageUrl, {{"User-Agent", kTvUserAgent}}, 20);
        if (page.http_ok())
        {
            const std::string bundle_url = base_script_url(page.body);
            if (!bundle_url.empty())
            {
                const auto bundle = http::get(bundle_url, {{"User-Agent", kTvUserAgent}}, 30);
                if (bundle.http_ok())
                    (void)client_identity(bundle.body, &client_id, &client_secret);
            }
        }
        if (client_id.empty() || client_secret.empty())
        {
            client_id = kFallbackClientId;
            client_secret = kFallbackClientSecret;
        }
    }
    std::string error;

    void load()
    {
        access_token.clear();
        refresh_token.clear();
        token_client_id.clear();
        token_client_secret.clear();
        load_known();
        std::string data;
        if (!hui::save::read_file(token_path, &data) || data.empty())
            return;
        yyjson_doc *doc = yyjson_read(data.data(), data.size(), 0);
        if (doc == nullptr)
            return;
        yyjson_val *root = yyjson_doc_get_root(doc);
        yyjson_val *token = yyjson_obj_get(root, "refresh_token");
        if (yyjson_is_str(token))
        {
            const char *text = yyjson_get_str(token);
            if (text != nullptr)
                refresh_token = text;
        }
        token_client_id = string_field(root, "client_id");
        token_client_secret = string_field(root, "client_secret");
        yyjson_doc_free(doc);
        lookup_client();
    }

    std::string token_json() const
    {
        if (refresh_token.empty())
            return {};
        std::string json = "{\"v\":1,\"refresh_token\":";
        append_json_string(json, refresh_token);
        if (!token_client_id.empty())
        {
            json += ",\"client_id\":";
            append_json_string(json, token_client_id);
            json += ",\"client_secret\":";
            append_json_string(json, token_client_secret);
        }
        json += '}';
        return json;
    }

    void persist() const
    {
        if (token_path.empty() || refresh_token.empty())
            return;
        (void)hui::save::write_atomic(token_path, token_json());
    }
};

Auth::Auth(std::string token_path) : impl_(new Impl())
{
    impl_->token_path = std::move(token_path);
    impl_->load();
}

Auth::~Auth()
{
    delete impl_;
}

DeviceCode Auth::begin()
{
    DeviceCode code;

    impl_->ensure_identity();
    std::string body = "client_id=";
    body += impl_->client_id;
    body += "&scope=";
    body += kScopeEncoded;

    http::Response response = http::post_json(kDeviceCodeUrl, form_headers(), body);
    if (!response.error.empty())
    {
        code.error = response.error;
        impl_->error = response.error;
        return code;
    }

    yyjson_doc *doc = yyjson_read(response.body.data(), response.body.size(), 0);
    yyjson_val *root = doc != nullptr ? yyjson_doc_get_root(doc) : nullptr;
    const bool usable = response.status >= 200 && response.status < 300 && yyjson_is_obj(root);

    if (usable)
    {
        code.device_code = string_field(root, "device_code");
        code.user_code = string_field(root, "user_code");
        code.verification_url = string_field(root, "verification_url");
        const long long interval = integer_field(root, "interval", 5);
        code.interval_s = interval > 0 ? static_cast<int>(interval) : 5;
        if (code.device_code.empty() || code.user_code.empty())
            code.error = "invalid response";
    }
    else
    {
        std::string detail = string_field(root, "error_description");
        if (detail.empty())
            detail = string_field(root, "error");
        if (detail.empty())
            detail = "http " + std::to_string(response.status);
        code.error = detail;
    }

    impl_->error = code.error;
    if (doc != nullptr)
        yyjson_doc_free(doc);
    return code;
}

Poll Auth::poll(const DeviceCode &code, std::string *error)
{
    impl_->ensure_identity();
    std::string body = "client_id=";
    body += impl_->client_id;
    body += "&client_secret=";
    body += impl_->client_secret;
    body += "&device_code=";
    body += url_encode_form(code.device_code);
    body += "&grant_type=urn:ietf:params:oauth:grant-type:device_code";

    http::Response response = http::post_json(kTokenUrl, form_headers(), body);
    if (!response.error.empty())
    {
        impl_->error = response.error;
        set_error(error, response.error);
        return Poll::failed;
    }

    yyjson_doc *doc = yyjson_read(response.body.data(), response.body.size(), 0);
    yyjson_val *root = doc != nullptr ? yyjson_doc_get_root(doc) : nullptr;

    Poll result = Poll::failed;
    const std::string failure = string_field(root, "error");
    if (!failure.empty())
    {
        const std::string detail = string_field(root, "error_description");
        const std::string message = detail.empty() ? failure : detail;
        if (failure == "authorization_pending")
        {
            result = Poll::pending;
            clear_error(error);
        }
        else if (failure == "slow_down")
        {
            result = Poll::slow_down;
            clear_error(error);
        }
        else if (failure == "access_denied")
        {
            result = Poll::denied;
            set_error(error, message);
        }
        else if (failure == "expired_token")
        {
            result = Poll::expired;
            set_error(error, message);
        }
        else
        {
            set_error(error, message);
        }
    }
    else if (response.status >= 200 && response.status < 300 && yyjson_is_obj(root))
    {
        const std::string access = string_field(root, "access_token");
        if (access.empty())
        {
            set_error(error, "no access token");
        }
        else
        {
            impl_->access_token = access;
            const std::string refresh = string_field(root, "refresh_token");
            if (!refresh.empty())
                impl_->refresh_token = refresh;
            // This grant belongs to the client that asked for it.
            impl_->remember_client(impl_->refresh_token, impl_->client_id, impl_->client_secret);
            impl_->error.clear();
            impl_->persist();
            clear_error(error);
            result = Poll::done;
        }
    }
    else if (response.status >= 200 && response.status < 300)
    {
        set_error(error, "invalid response");
    }
    else
    {
        set_error(error, "http " + std::to_string(response.status));
    }

    if (result == Poll::failed)
        impl_->error = error != nullptr ? *error : std::string();
    if (doc != nullptr)
        yyjson_doc_free(doc);
    return result;
}

bool Auth::refresh(std::string *error)
{
    if (impl_->refresh_token.empty())
    {
        impl_->error = "not signed in";
        set_error(error, impl_->error);
        return false;
    }

    // The token's own client first, then this session's scraped pair, then
    // the last known one: a grant issued before YouTube rotated its TV app
    // still refreshes with the pair it was issued to.
    impl_->ensure_identity();
    std::vector<std::pair<std::string, std::string>> clients;
    const auto add = [&](const std::string &id, const std::string &secret)
    {
        if (id.empty())
            return;
        for (const auto &client : clients)
            if (client.first == id)
                return;
        clients.emplace_back(id, secret);
    };
    add(impl_->token_client_id, impl_->token_client_secret);
    add(impl_->client_id, impl_->client_secret);
    add(kFallbackClientId, kFallbackClientSecret);

    bool done = false;
    bool every_grant_refused = true;
    for (const auto &[id, secret] : clients)
    {
        std::string body = "grant_type=refresh_token&refresh_token=";
        body += url_encode_form(impl_->refresh_token);
        body += "&client_id=";
        body += id;
        body += "&client_secret=";
        body += secret;

        http::Response response = http::post_json(kTokenUrl, form_headers(), body);
        if (!response.error.empty())
        {
            // The network, not the grant: keep the token, try again later.
            impl_->error = response.error;
            set_error(error, impl_->error);
            return false;
        }

        yyjson_doc *doc = yyjson_read(response.body.data(), response.body.size(), 0);
        yyjson_val *root = doc != nullptr ? yyjson_doc_get_root(doc) : nullptr;
        const std::string access = string_field(root, "access_token");
        done = response.status >= 200 && response.status < 300 && !access.empty();
        if (done)
        {
            impl_->access_token = access;
            const std::string refresh = string_field(root, "refresh_token");
            if (!refresh.empty())
                impl_->refresh_token = refresh;
            impl_->remember_client(impl_->refresh_token, id, secret);
            impl_->error.clear();
            // The server may hand out a new refresh token with the access one;
            // leaving the old one on disk would sign the next launch (and the
            // next restore) out with a grant the server no longer knows.
            // poll() persists for the same reason.
            impl_->persist();
            clear_error(error);
            yyjson_doc_free(doc);
            break;
        }
        const std::string code = string_field(root, "error");
        std::string detail = string_field(root, "error_description");
        if (detail.empty())
            detail = code;
        if (detail.empty())
            detail = access.empty() ? "no access token" : "http " + std::to_string(response.status);
        impl_->error = detail;
        set_error(error, detail);
        if (doc != nullptr)
            yyjson_doc_free(doc);
        // A refusal that names the client or the grant may be the wrong
        // client for this grant: the next pair gets its turn. Anything else
        // (a temporary refusal, a portal) keeps the token as it is.
        if (code != "invalid_grant" && code != "unauthorized_client" && code != "invalid_client")
            return false;
        if (code != "invalid_grant")
            every_grant_refused = false;
    }

    // Only a grant every client called dead ("invalid_grant") is beyond
    // retry: the account is signed out - memory and file - and the app ends
    // in the sign-in state instead of one that will never work again. A 401 /
    // invalid_client names a stale client pair, not a dead grant, and keeps
    // the token (RFC 6749 reserves it for the client).
    if (!done && every_grant_refused)
    {
        if (!impl_->token_path.empty())
            std::remove(impl_->token_path.c_str());
        impl_->access_token.clear();
        impl_->refresh_token.clear();
    }
    return done;
}

const std::string &Auth::access_token() const
{
    return impl_->access_token;
}

bool Auth::signed_in() const
{
    return !impl_->refresh_token.empty();
}

void Auth::sign_out()
{
    if (!impl_->token_path.empty())
        std::remove(impl_->token_path.c_str());
    impl_->access_token.clear();
    impl_->refresh_token.clear();
    impl_->token_client_id.clear();
    impl_->token_client_secret.clear();
    impl_->error.clear();
}

const std::string &Auth::refresh_token() const
{
    return impl_->refresh_token;
}

const std::string &Auth::token_path() const
{
    return impl_->token_path;
}

std::string Auth::token_json() const
{
    return impl_->token_json();
}

bool Auth::adopt_token_json(std::string_view json, std::string *error)
{
    clear_error(error);
    if (json.empty())
    {
        set_error(error, "empty token file");
        return false;
    }
    yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr)
    {
        set_error(error, "token file is not JSON");
        return false;
    }
    yyjson_val *root = yyjson_doc_get_root(doc);
    const std::string token = string_field(root, "refresh_token");
    const std::string id = string_field(root, "client_id");
    const std::string secret = string_field(root, "client_secret");
    yyjson_doc_free(doc);
    if (token.empty())
    {
        set_error(error, "token file has no refresh_token");
        return false;
    }
    impl_->refresh_token = token;
    impl_->access_token.clear();
    impl_->token_client_id = id;
    impl_->token_client_secret = secret;
    impl_->lookup_client(); // a remembered account: the client it was linked with
    impl_->persist();
    return true;
}

std::string prefilled_verification_url(const std::string &verification_url,
                                       const std::string &user_code)
{
    std::string url =
        verification_url.empty() ? std::string("https://www.google.com/device") : verification_url;
    if (url.find("://") == std::string::npos)
        url = "https://" + url;
    if (user_code.empty())
        return url;
    url += url.find('?') == std::string::npos ? '?' : '&';
    url += "user_code=";
    for (const char c : user_code)
    {
        // Codes are letters, digits and dashes; anything else is escaped.
        const bool plain =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (plain)
        {
            url += c;
            continue;
        }
        static const char kHex[] = "0123456789ABCDEF";
        const auto byte = static_cast<unsigned char>(c);
        url += '%';
        url += kHex[byte >> 4];
        url += kHex[byte & 15];
    }
    return url;
}

} // namespace yt
