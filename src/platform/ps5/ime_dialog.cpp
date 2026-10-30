// youtube-tv - The PlayStation system keyboard (libSceImeDialog).
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The dialog's parameter block is the PS4 SDK's SceImeDialogParam (the PS5
// kept the library and its calls); offsets are asserted below so a change
// of layout cannot slip in unnoticed.

#include "platform/ps5/ime_dialog.hpp"

#include "yt/log.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C"
{
    int sceSysmoduleLoadModule(std::uint16_t id);
    int sceKernelLoadStartModule(const char *path, std::size_t args, const void *argp,
                                 std::uint32_t flags, const void *option, int *result);
    int sceKernelDlsym(int handle, const char *symbol, void **address);
    int sceKernelGetModuleList(int *handles, std::size_t count, std::size_t *found);
    int sceKernelGetModuleInfo(int handle, void *info);
    int sceUserServiceGetInitialUser(int *user);
    // The SDK's import library for the dialog: the loader binds these at
    // start, as it does the pad and audio (a module loaded by path from the
    // app's sandbox is refused, which is what 1.0.190 tried alone).
    int sceImeDialogInit(const void *param, const void *extended);
    int sceImeDialogGetStatus();
    int sceImeDialogGetResult(void *result);
    int sceImeDialogTerm();
    int sceImeDialogAbort();
}

namespace hui::ps5
{

namespace
{

struct ImeDialogParam
{
    std::int32_t user_id;
    std::int32_t type; // 0 default text
    std::uint64_t supported_languages;
    std::int32_t enter_label; // 2 "Search"
    std::int32_t input_method;
    void *filter;
    std::uint32_t option;
    std::uint32_t max_text_length;
    char16_t *input_text_buffer;
    float pos_x;
    float pos_y;
    std::int32_t horizontal_alignment; // 1 centre
    std::int32_t vertical_alignment;   // 1 centre
    const char16_t *placeholder;
    const char16_t *title;
    std::int8_t reserved[16];
};
static_assert(offsetof(ImeDialogParam, filter) == 24);
static_assert(offsetof(ImeDialogParam, input_text_buffer) == 40);
static_assert(offsetof(ImeDialogParam, placeholder) == 64);
static_assert(sizeof(ImeDialogParam) == 96);

struct ImeDialogResult
{
    std::int32_t end_status; // 0 ok, 1 cancelled, 2 aborted
    std::int8_t reserved[12];
};

using InitFn = int (*)(const ImeDialogParam *, const void *extended);
using StatusFn = int (*)();
using ResultFn = int (*)(ImeDialogResult *);
using TermFn = int (*)();
using CommonInitFn = int (*)();

InitFn g_init = nullptr;
StatusFn g_status = nullptr;
ResultFn g_result = nullptr;
TermFn g_term = nullptr;
TermFn g_abort = nullptr;

constexpr int kStatusRunning = 1;
constexpr int kStatusFinished = 2;
// The library ids of the PS4 SDK's sysmodule list (common dialog, IME dialog).
constexpr std::uint16_t kSysmoduleCommonDialog = 0x0091;
constexpr std::uint16_t kSysmoduleImeDialog = 0x0096;

// Imports the loader could not bind read as null (videodec.cpp found
// libkernel's and libSceSysmodule's left unbound on some titles): every one is
// checked before it is called.
using SysmoduleFn = int (*)(std::uint16_t);
using LoadStartFn = int (*)(const char *, std::size_t, const void *, std::uint32_t, const void *,
                            int *);
using DlsymFn = int (*)(int, const char *, void **);

int load_module(const char *name)
{
    volatile LoadStartFn const load_start = &sceKernelLoadStartModule;
    if (load_start == nullptr)
    {
        yt::log::line("ime: module loading not available");
        return -1;
    }
    const std::string paths[] = {std::string("/system/common/lib/") + name,
                                 std::string("/system/priv/lib/") + name, name};
    for (const std::string &path : paths)
    {
        int started = 0;
        const int handle = load_start(path.c_str(), 0, nullptr, 0, nullptr, &started);
        if (handle >= 0)
        {
            yt::log::line("ime: %s loaded (0x%08x)", path.c_str(), static_cast<unsigned>(handle));
            return handle;
        }
    }
    yt::log::line("ime: %s did not load", name);
    return -1;
}

// ---- finding an entry point --------------------------------------------------------------
// System modules export by NID: the first 8 bytes of SHA-1(name + a fixed
// suffix), reversed, in base64 with '-' for '/' (11 characters). A name a
// module does not answer may still be found by its NID.

void sha1(const unsigned char *data, std::size_t size, unsigned char out[20])
{
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const auto rotl = [](std::uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    std::size_t total = ((size + 8) / 64 + 1) * 64;
    unsigned char block[64];
    for (std::size_t offset = 0; offset < total; offset += 64)
    {
        for (std::size_t i = 0; i < 64; ++i)
        {
            const std::size_t at = offset + i;
            if (at < size)
                block[i] = data[at];
            else if (at == size)
                block[i] = 0x80;
            else if (at >= total - 8)
                block[i] = static_cast<unsigned char>((static_cast<std::uint64_t>(size) * 8) >>
                                                      ((total - 1 - at) * 8));
            else
                block[i] = 0;
        }
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<std::uint32_t>(block[i * 4]) << 24 |
                   static_cast<std::uint32_t>(block[i * 4 + 1]) << 16 |
                   static_cast<std::uint32_t>(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
        for (int i = 16; i < 80; ++i)
            w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (i < 20)
            {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            }
            else if (i < 40)
            {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            }
            else if (i < 60)
            {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            }
            else
            {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[i * 4 + j] = static_cast<unsigned char>(h[i] >> (24 - j * 8));
}

std::string nid_of(const char *name)
{
    static const unsigned char kSuffix[16] = {0x51, 0x8D, 0x64, 0xA6, 0x35, 0xDE, 0xD8, 0xC1,
                                              0xE6, 0xB0, 0x39, 0xB1, 0xC3, 0xE5, 0x52, 0x30};
    std::string input(name);
    input.append(reinterpret_cast<const char *>(kSuffix), sizeof kSuffix);
    unsigned char digest[20];
    sha1(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest);
    unsigned char bytes[8];
    for (int i = 0; i < 8; ++i)
        bytes[i] = digest[7 - i];
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const unsigned char byte : bytes)
    {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= 6)
        {
            bits -= 6;
            out += kAlphabet[(buffer >> bits) & 0x3f];
        }
    }
    if (bits > 0)
        out += kAlphabet[(buffer << (6 - bits)) & 0x3f];
    return out; // 11 characters
}

void *dlsym_one(int handle, const std::string &name)
{
    void *address = nullptr;
    volatile DlsymFn const dlsym_fn = &sceKernelDlsym;
    if (handle < 0 || dlsym_fn == nullptr || dlsym_fn(handle, name.c_str(), &address) != 0)
        return nullptr;
    return address;
}

// The handles of the loaded modules whose name holds `part`.
std::vector<int> modules_named(const char *part)
{
    std::vector<int> out;
    using ListFn = int (*)(int *, std::size_t, std::size_t *);
    using InfoFn = int (*)(int, void *);
    volatile ListFn const list = &sceKernelGetModuleList;
    volatile InfoFn const info = &sceKernelGetModuleInfo;
    if (list == nullptr || info == nullptr)
        return out;
    int handles[512];
    std::size_t count = 0;
    if (list(handles, 512, &count) != 0)
        return out;
    for (std::size_t i = 0; i < count && i < 512; ++i)
    {
        // SceKernelModuleInfo: its size, then the name.
        alignas(8) unsigned char buffer[0x200] = {};
        const std::uint64_t size = 0x160;
        std::memcpy(buffer, &size, sizeof size);
        if (info(handles[i], buffer) != 0)
            continue;
        const char *name = reinterpret_cast<const char *>(buffer + 8);
        if (std::strstr(name, part) != nullptr)
        {
            yt::log::line("ime: module %s handle 0x%08x", name, static_cast<unsigned>(handles[i]));
            out.push_back(handles[i]);
        }
    }
    return out;
}

void *symbol(int handle, const char *name)
{
    const std::string nid = nid_of(name);
    std::vector<int> handles;
    if (handle >= 0)
        handles.push_back(handle);
    for (const int other :
         modules_named(std::strstr(name, "Common") != nullptr ? "CommonDialog" : "ImeDialog"))
        if (other != handle)
            handles.push_back(other);
    for (const int candidate : handles)
    {
        for (const std::string &key : {std::string(name), nid})
        {
            if (void *address = dlsym_one(candidate, key))
            {
                yt::log::line("ime: %s found as %s in 0x%08x", name, key.c_str(),
                              static_cast<unsigned>(candidate));
                return address;
            }
        }
    }
    yt::log::line("ime: %s (%s) not found", name, nid.c_str());
    return nullptr;
}

void to_utf16(const std::string &text, char16_t *out, std::size_t capacity)
{
    std::size_t used = 0;
    for (std::size_t i = 0; i < text.size() && used + 2 < capacity;)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::uint32_t cp = c;
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (extra > 0)
            cp = c & (0x3f >> extra);
        ++i;
        for (; extra > 0 && i < text.size(); --extra, ++i)
            cp = (cp << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            out[used++] = static_cast<char16_t>(0xd800 + (cp >> 10));
            out[used++] = static_cast<char16_t>(0xdc00 + (cp & 0x3ff));
        }
        else
            out[used++] = static_cast<char16_t>(cp);
    }
    out[used] = 0;
}

std::string to_utf8(const char16_t *text)
{
    std::string out;
    for (std::size_t i = 0; text[i] != 0; ++i)
    {
        std::uint32_t cp = text[i];
        if (cp >= 0xd800 && cp < 0xdc00 && text[i + 1] >= 0xdc00 && text[i + 1] < 0xe000)
        {
            cp = 0x10000 + ((cp - 0xd800) << 10) + (text[i + 1] - 0xdc00);
            ++i;
        }
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

} // namespace

bool ImeDialog::load(std::string *error)
{
    if (loaded_)
        return true;
    if (load_failed_)
    {
        *error = "the system keyboard did not load";
        return false;
    }
    load_failed_ = true; // until it works
    {
        // The imports the loader bound (null when it did not).
        volatile InitFn const init = reinterpret_cast<InitFn>(&sceImeDialogInit);
        volatile StatusFn const status = &sceImeDialogGetStatus;
        volatile ResultFn const result = reinterpret_cast<ResultFn>(&sceImeDialogGetResult);
        volatile TermFn const term = &sceImeDialogTerm;
        volatile TermFn const abort_fn = &sceImeDialogAbort;
        yt::log::line("ime: imports init=%d status=%d result=%d term=%d", init != nullptr,
                      status != nullptr, result != nullptr, term != nullptr);
        if (init != nullptr && status != nullptr && result != nullptr && term != nullptr)
        {
            g_init = init;
            g_status = status;
            g_result = result;
            g_term = term;
            g_abort = abort_fn;
            // The common dialog setup, when its module is reachable by name.
            const int common = load_module("libSceCommonDialog.sprx");
            if (auto setup =
                    reinterpret_cast<CommonInitFn>(symbol(common, "sceCommonDialogInitialize")))
                yt::log::line("ime: sceCommonDialogInitialize 0x%08x",
                              static_cast<unsigned>(setup()));
            load_failed_ = false;
            loaded_ = true;
            return true;
        }
    }
    // The sysmodule way first (a module already loaded answers 0), then
    // the files themselves, whose handles give the entry points.
    volatile SysmoduleFn const sysmodule = &sceSysmoduleLoadModule;
    if (sysmodule != nullptr)
        yt::log::line("ime: sysmodule common dialog 0x%08x, ime dialog 0x%08x",
                      static_cast<unsigned>(sysmodule(kSysmoduleCommonDialog)),
                      static_cast<unsigned>(sysmodule(kSysmoduleImeDialog)));
    const int common = load_module("libSceCommonDialog.sprx");
    if (auto init = reinterpret_cast<CommonInitFn>(symbol(common, "sceCommonDialogInitialize")))
        yt::log::line("ime: sceCommonDialogInitialize 0x%08x", static_cast<unsigned>(init()));
    const int ime = load_module("libSceImeDialog.sprx");
    g_init = reinterpret_cast<InitFn>(symbol(ime, "sceImeDialogInit"));
    g_status = reinterpret_cast<StatusFn>(symbol(ime, "sceImeDialogGetStatus"));
    g_result = reinterpret_cast<ResultFn>(symbol(ime, "sceImeDialogGetResult"));
    g_term = reinterpret_cast<TermFn>(symbol(ime, "sceImeDialogTerm"));
    g_abort = reinterpret_cast<TermFn>(symbol(ime, "sceImeDialogAbort"));
    if (g_init == nullptr || g_status == nullptr || g_result == nullptr || g_term == nullptr)
    {
        *error = "the keyboard library was not found (see yt.log, ime:)";
        return false;
    }
    load_failed_ = false;
    loaded_ = true;
    return true;
}

bool ImeDialog::open(const std::string &title, const std::string &text, std::string *error)
{
    std::string ignored;
    if (error == nullptr)
        error = &ignored;
    if (!load(error))
        return false;
    int user = -1;
    if (sceUserServiceGetInitialUser(&user) != 0)
        user = -1;
    to_utf16(text, buffer_, kMaxLength + 1);
    to_utf16(title, title_, 64);
    to_utf16("Search YouTube", placeholder_, 64);
    ImeDialogParam param{};
    param.user_id = user;
    param.type = 0;
    param.enter_label = 2;
    param.max_text_length = kMaxLength;
    param.input_text_buffer = buffer_;
    param.pos_x = 960.0f;
    param.pos_y = 540.0f;
    param.horizontal_alignment = 1;
    param.vertical_alignment = 1;
    param.placeholder = placeholder_;
    param.title = title_;
    const int result = g_init(&param, nullptr);
    yt::log::line("ime: sceImeDialogInit user=0x%08x -> 0x%08x", static_cast<unsigned>(user),
                  static_cast<unsigned>(result));
    if (result != 0)
    {
        char code[16];
        std::snprintf(code, sizeof code, "0x%08x", static_cast<unsigned>(result));
        *error = std::string("the keyboard refused to open (") + code + ")";
        return false;
    }
    open_ = true;
    return true;
}

app::SystemKeyboard::State ImeDialog::poll(std::string *text)
{
    if (!open_)
        return State::failed;
    const int status = g_status();
    if (status == kStatusRunning)
        return State::running;
    if (status != kStatusFinished)
    {
        yt::log::line("ime: status %d, closing", status);
        g_term();
        open_ = false;
        return State::failed;
    }
    ImeDialogResult result{};
    const int got = g_result(&result);
    g_term();
    open_ = false;
    yt::log::line("ime: finished (result 0x%08x, end %d)", static_cast<unsigned>(got),
                  result.end_status);
    if (got != 0 || result.end_status != 0)
        return State::cancelled;
    if (text != nullptr)
        *text = to_utf8(buffer_);
    return State::done;
}

void ImeDialog::abort()
{
    if (!open_)
        return;
    if (g_abort != nullptr)
        g_abort();
    g_term();
    open_ = false;
    yt::log::line("ime: aborted");
}

} // namespace hui::ps5
