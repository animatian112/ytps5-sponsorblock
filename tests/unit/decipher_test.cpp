// youtube-tv - The signature/"n" descrambler and the cipher-only format parser.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The player JS comes in two shapes. A plain build names its helpers directly
// from the "signature"/"n" setters. Today's TV build obfuscates them: a
// ";".split(";") string table, an operation table of splice/reverse/swap, and
// one dispatcher that drives them. A helper only counts as the throttle when
// an "n" call site reaches it; the TV excerpt's dispatcher is reached from the
// cipher setter, so its "n" value is left alone. Both are pinned here without
// a network.

#include "yt/decipher.hpp"
#include "yt/innertube.hpp"

#include "core/save_file.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

const char *const kPlayerJs = R"JS(
var jZ={Xq:function(a,b){a.splice(0,b)},Mv:function(a,b){var c=a[0];a[0]=a[b%a.length];a[b%a.length]=c}};
var ns=function(a){a=a.split("");jZ.Xq(a,2);a=a.reverse();jZ.Mv(a,3);a=a.slice(1);return a.join("")};
var Th=function(a){a=a.split("");a=a.slice(1);a=a.reverse();return a.join("")};
function url(a){a.set("signature",encodeURIComponent(ns(decodeURIComponent(a.get("s")))));a.set("n",Th(a.get("n")))}
)JS";

std::string fixture(const char *name)
{
    std::string data;
    const std::string path = std::string(HUI_SOURCE_DIR) + "/tests/fixtures/" + name;
    EXPECT_TRUE(hui::save::read_file(path, &data)) << path;
    return data;
}

TEST(Decipher, DescramblesTheSignatureAndTheThrottle)
{
    yt::Descrambler descrambler;
    std::string error;
    ASSERT_TRUE(descrambler.prepare_js(kPlayerJs, &error)) << error;
    EXPECT_TRUE(descrambler.ready());

    const std::string url =
        "https://rr1---sn-example.googlevideo.com/videoplayback?itag=137&n=abcdef";

    // splice(0,2) + reverse + swap(0, 3) + slice(1).
    const std::string first = descrambler.descramble(url, "abcdef", "sig", &error);
    EXPECT_EQ(first,
              "https://rr1---sn-example.googlevideo.com/videoplayback?itag=137&n=fedcb&sig=edf")
        << error;

    const std::string second = descrambler.descramble(url, "0123456789", "sig", &error);
    EXPECT_EQ(second,
              "https://rr1---sn-example.googlevideo.com/videoplayback?itag=137&n=fedcb&sig=8795432")
        << error;
}

TEST(Decipher, RefusesAnUnknownOperationWithItsText)
{
    const char *const hostile = R"JS(
var ns=function(a){a=a.split("");a=a.map(a);return a.join("")};
function url(a){a.set("signature",ns(a.get("s")));a.set("n",ns(a.get("n")))}
)JS";
    yt::Descrambler descrambler;
    std::string error;
    EXPECT_FALSE(descrambler.prepare_js(hostile, &error));
    EXPECT_NE(error.find("unsupported"), std::string::npos) << error;
    EXPECT_NE(error.find(".map("), std::string::npos) << error;
    EXPECT_FALSE(descrambler.ready());
}

TEST(Decipher, ExtractsTheObfuscatedTvSignatureHelperAndLeavesTheThrottleAlone)
{
    yt::Descrambler descrambler;
    std::string error;
    ASSERT_TRUE(descrambler.prepare_js(fixture("player_js_tv_excerpt.js"), &error)) << error;
    EXPECT_TRUE(descrambler.ready());

    // The excerpt's dispatcher is reached from the cipher/signature setter, not
    // from an "n" call site, and the real build's n path is a VM this subset
    // cannot run. No throttle helper is proven, so descramble must hand the URL
    // back byte-for-byte, the path segment included.
    const std::string url =
        "https://rr1---sn-example.googlevideo.com/n/abcdefghij/videoplayback?itag=137";
    std::string path_error;
    const std::string out = descrambler.descramble(url, "", "", &path_error);
    EXPECT_TRUE(path_error.empty()) << path_error;
    EXPECT_EQ(out, url);

    // ready() is true because the signature helper was extracted; had the
    // excerpt named no helper at all, prepare_js() would have failed above.
}

TEST(Decipher, ThrottlesTheQueryAndThePathOnlyWhenTheNHelperIsProven)
{
    // A build whose "n" setter reaches an interpretable helper: only then does
    // descramble touch the throttle value, in whichever shape it is served.
    const char *const throttled = R"JS(
var nt=function(a){a=a.split("");a=a.reverse();a=a.slice(1);return a.join("")};
function fix(u){u.set("n",nt(u.get("n")))}
)JS";
    yt::Descrambler descrambler;
    std::string error;
    ASSERT_TRUE(descrambler.prepare_js(throttled, &error)) << error;

    const std::string url =
        "https://rr1---sn-example.googlevideo.com/n/abcdef/videoplayback?itag=137&n=abcdef";
    std::string throttle_error;
    const std::string out = descrambler.descramble(url, "", "", &throttle_error);
    ASSERT_TRUE(throttle_error.empty()) << throttle_error;
    // reverse("abcdef") = "fedcba", then slice(1) = "edcba": both the query
    // parameter and the path segment carry the transformed value.
    EXPECT_EQ(out,
              "https://rr1---sn-example.googlevideo.com/n/edcba/videoplayback?itag=137&n=edcba");
}

TEST(Decipher, PlayerResponseKeepsACipheredFormatAndTheJsUrl)
{
    yt::VideoInfo info;
    ASSERT_TRUE(yt::parse_video_info(fixture("player_cipher.json"), &info)) << info.error;
    ASSERT_EQ(info.formats.size(), 3u);

    // The cipher-only video format is kept: base URL, s/sp and the marker.
    const yt::Format *ciphered = nullptr;
    for (const yt::Format &format : info.formats)
    {
        if (format.ciphered)
        {
            ciphered = &format;
            break; // the first ciphered format is the one described below
        }
    }
    ASSERT_NE(ciphered, nullptr);
    EXPECT_EQ(ciphered->itag, 137);
    EXPECT_EQ(ciphered->codec, yt::Codec::avc);
    EXPECT_EQ(ciphered->url,
              "https://rr1---sn-example.googlevideo.com/videoplayback?itag=137&n=abcdef");
    EXPECT_EQ(ciphered->cipher_url, ciphered->url);
    EXPECT_EQ(ciphered->cipher_s, "abcdef");
    EXPECT_EQ(ciphered->cipher_sp, "sig");

    // A plain format next to it stays plain.
    EXPECT_FALSE(info.formats[2].ciphered);
    EXPECT_EQ(info.formats[2].codec, yt::Codec::opus);

    EXPECT_EQ(info.player_js_url,
              "https://www.youtube.com/s/player/abc123/tv-player-ias.vflset/tv-player-ias.js");

    EXPECT_EQ(yt::format_summary(info.formats),
              "3 formats: 1 avc, 1 vp9, 0 av1, 1 other, 1 plain, 2 ciphered, max 1080p, "
              "host rr1---sn-example.googlevideo.com");

    // The parsed cipher and the trimmed player JS descramble end to end.
    yt::Descrambler descrambler;
    std::string error;
    ASSERT_TRUE(descrambler.prepare_js(kPlayerJs, &error)) << error;
    EXPECT_EQ(
        descrambler.descramble(ciphered->url, ciphered->cipher_s, ciphered->cipher_sp, &error),
        "https://rr1---sn-example.googlevideo.com/videoplayback?itag=137&n=fedcb&sig=edf")
        << error;
}

TEST(Decipher, ReadsTheLegacyCipherFieldAndBuildsTheJsHashUrl)
{
    const std::string json =
        R"({"playabilityStatus":{"status":"OK"},"assets":{"jsUrlHash":"hash7"},)"
        R"("streamingData":{"adaptiveFormats":[{"itag":248,)"
        R"("cipher":"url=https%3A%2F%2Fx.googlevideo.com%2Fv%3Fitag%3D248&s=zz&sp=signature",)"
        R"("mimeType":"video/webm; codecs=\"vp9\"","width":1920,"height":1080}]}})";
    yt::VideoInfo info;
    ASSERT_TRUE(yt::parse_video_info(json, &info)) << info.error;
    ASSERT_EQ(info.formats.size(), 1u);
    EXPECT_TRUE(info.formats[0].ciphered);
    EXPECT_EQ(info.formats[0].url, "https://x.googlevideo.com/v?itag=248");
    EXPECT_EQ(info.formats[0].cipher_s, "zz");
    EXPECT_EQ(info.formats[0].cipher_sp, "signature");
    EXPECT_EQ(info.player_js_url,
              "https://www.youtube.com/s/player/hash7/tv-player-ias.vflset/tv-player-ias.js");
}

} // namespace
