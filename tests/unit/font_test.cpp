// ps5-homebrew-ui - Baked font loading and layout tests.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/save_file.hpp"
#include "gfx/font.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

using hui::gfx::Align;
using hui::gfx::Font;
using hui::gfx::GlyphQuad;

// A face of its own for what the baked ones cannot show: one square glyph per
// code point (given in rising order), each `advance` wide at `pixel_size`.
std::string small_face(std::initializer_list<std::uint32_t> codepoints, float pixel_size,
                       float advance)
{
    namespace ff = hui::gfx::font_format;
    ff::Header header{};
    header.magic = ff::kMagic;
    header.version = ff::kVersion;
    header.atlas_width = 8;
    header.atlas_height = 8;
    header.pixel_size = pixel_size;
    header.sdf_range = 2.0f;
    header.ascent = pixel_size * 0.8f;
    header.descent = -pixel_size * 0.2f;
    header.glyph_count = static_cast<std::uint32_t>(codepoints.size());
    std::string data(reinterpret_cast<const char *>(&header), sizeof(header));
    for (const std::uint32_t codepoint : codepoints)
    {
        const ff::Glyph glyph{codepoint, 0, 0, 4, 4, 0.0f, -4.0f, advance};
        data.append(reinterpret_cast<const char *>(&glyph), sizeof(glyph));
    }
    data.append(64, '\x80');
    return data;
}

const Font &inter()
{
    static Font font = []
    {
        Font loaded;
        std::string data;
        EXPECT_TRUE(
            hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/google-sans-medium.huifont", &data));
        EXPECT_TRUE(loaded.load(data)) << loaded.error();
        return loaded;
    }();
    return font;
}

TEST(Font, LoadsAsciiAndSymbols)
{
    const Font &font = inter();
    for (char c = ' '; c < 127; ++c)
        EXPECT_TRUE(font.has_glyph(static_cast<std::uint32_t>(c))) << c;
    EXPECT_TRUE(font.has_glyph(0x2022));
    EXPECT_GT(font.ascent(40), 30.0f);
    EXPECT_GT(font.descent(40), 5.0f);
    EXPECT_GT(font.line_height(40), font.ascent(40));
}

TEST(Font, RejectsCorruptData)
{
    Font font;
    EXPECT_FALSE(font.load("short"));
    std::string data;
    ASSERT_TRUE(
        hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/google-sans-regular.huifont", &data));
    EXPECT_FALSE(font.load(data.substr(0, data.size() - 1)));
    data[0] = 'X';
    EXPECT_FALSE(font.load(data));
}

TEST(Font, MeasureScalesLinearlyAndKerns)
{
    const Font &font = inter();
    const float at20 = font.measure("Light Up", 20);
    const float at40 = font.measure("Light Up", 40);
    EXPECT_GT(at20, 0.0f);
    EXPECT_NEAR(at40, at20 * 2.0f, 0.01f);
    // stb_truetype only reads legacy/simple kerning; pairs never widen text.
    EXPECT_LE(font.measure("AV", 40), font.measure("A", 40) + font.measure("V", 40));
    EXPECT_FLOAT_EQ(font.measure("", 40), 0.0f);
}

TEST(Font, LayoutAlignsAndSkipsSpaces)
{
    const Font &font = inter();
    std::vector<GlyphQuad> quads;
    const float width = font.layout("A B", 100.0f, 50.0f, 32.0f, Align::left, quads);
    ASSERT_EQ(quads.size(), 2u);
    // Quads include the distance-field margin around each glyph.
    EXPECT_GE(quads[0].x0, 100.0f - font.sdf_range(32.0f) - 1.0f);
    EXPECT_LT(quads[0].x0, 100.0f);
    EXPECT_LT(quads[0].y0, 50.0f); // glyphs rise above the baseline
    EXPECT_GT(quads[1].x0, quads[0].x1 - 20.0f);
    for (const GlyphQuad &q : quads)
    {
        EXPECT_GE(q.u0, 0.0f);
        EXPECT_LE(q.u1, 1.0f);
        EXPECT_LT(q.u0, q.u1);
        EXPECT_LT(q.v0, q.v1);
    }
    std::vector<GlyphQuad> centred;
    font.layout("A B", 100.0f, 50.0f, 32.0f, Align::center, centred);
    EXPECT_NEAR(centred[0].x0, quads[0].x0 - width * 0.5f, 0.01f);
    std::vector<GlyphQuad> right;
    font.layout("A B", 100.0f, 50.0f, 32.0f, Align::right, right);
    EXPECT_NEAR(right[0].x0, quads[0].x0 - width, 0.01f);
}

TEST(Font, UnknownCharactersFallBackToQuestionMark)
{
    const Font &font = inter();
    // U+E000, a private-use code point no face (and no emoji) has.
    EXPECT_FLOAT_EQ(font.measure("\xEE\x80\x80", 30), font.measure("?", 30));
    EXPECT_FLOAT_EQ(font.measure("\xFF", 30), font.measure("?", 30)); // invalid byte
}

TEST(Font, AsksItsFallbacksForWhatItLacks)
{
    Font font = inter(); // a copy: fallbacks belong to one font
    Font other;
    ASSERT_TRUE(other.load(small_face({0x4e16, 0x754c}, 16.0f, 16.0f))) << other.error();
    const std::string mixed = "A\xE4\xB8\x96"; // "A" and U+4E16
    EXPECT_FALSE(font.has_glyph(0x4e16));
    const float before = font.measure(mixed, 32);
    EXPECT_FLOAT_EQ(before, font.measure("A?", 32));

    font.add_fallback(&other, 7);
    EXPECT_TRUE(font.has_glyph(0x4e16));
    // The glyph keeps its own face's metrics: 16 wide at 16, so 32 at 32.
    EXPECT_NEAR(font.measure(mixed, 32), font.measure("A", 32) + 32.0f, 0.01f);
    std::vector<GlyphQuad> quads;
    font.layout(mixed, 0.0f, 0.0f, 32.0f, Align::left, quads);
    ASSERT_EQ(quads.size(), 2u);
    EXPECT_EQ(quads[0].texture, 0u);
    EXPECT_FLOAT_EQ(quads[0].range, font.sdf_range(32.0f));
    EXPECT_EQ(quads[1].texture, 7u);
    EXPECT_FLOAT_EQ(quads[1].range, other.sdf_range(32.0f));
    EXPECT_NEAR(quads[1].x1 - quads[1].x0, 8.0f, 0.01f); // 4 atlas pixels at twice the size
    // What no face has is still the font's own question mark.
    EXPECT_FLOAT_EQ(font.measure("\xEE\x80\x80", 30), inter().measure("?", 30));
    // A fallback is never asked for what the font has itself.
    EXPECT_FLOAT_EQ(font.measure("Light Up", 40), inter().measure("Light Up", 40));

    font.clear_fallbacks();
    EXPECT_FALSE(font.has_glyph(0x4e16));
    EXPECT_FLOAT_EQ(font.measure(mixed, 32), before);
}

// The Chinese, Japanese and Korean face src/main.cpp hangs behind every other
// face. It is far bigger than the others (one 4096 atlas), hence the limit.
TEST(Font, TheCjkFaceDrawsJapaneseChineseAndKoreanTitles)
{
    std::string data;
    ASSERT_TRUE(hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/noto-sans-cjk.huifont", &data,
                                     32u << 20));
    Font cjk;
    ASSERT_TRUE(cjk.load(data)) << cjk.error();
    EXPECT_EQ(cjk.atlas_width(), 4096);

    Font font = inter();
    // Without it, Japanese is the row of question marks users reported (#20).
    EXPECT_FALSE(font.has_glyph(0x65E5)); // 日
    font.add_fallback(&cjk, 9);

    // Titles as YouTube sends them: hiragana, katakana, kanji, full-width
    // punctuation, then Simplified and Traditional Chinese and Hangul.
    for (const std::string_view title :
         {"\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E\xE3\x81\xAE\xE3\x82\xBF\xE3\x82\xA4\xE3\x83\x88\xE3"
          "\x83\xAB\xE3\x80\x90\xE5\x85\xAC\xE5\xBC\x8F\xE3\x80\x91", // 日本語のタイトル【公式】
          "\xE3\x83\xA9\xE3\x83\xBC\xE3\x83\xA1\xE3\x83\xB3\xE3\x80\x81\xE9\xA3\x9F\xE3\x81\xB9"
          "\xE3\x81\xBE\xE3\x81\x99\xE3\x80\x82",             // ラーメン、食べます。
          "\xE5\x91\xA8\xE6\x9D\xB0\xE4\xBC\xA6",             // 周杰伦
          "\xE7\xB9\x81\xE9\xAB\x94\xE4\xB8\xAD\xE6\x96\x87", // 繁體中文
          "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4"})            // 한국어
    {
        std::size_t i = 0;
        while (i < title.size())
        {
            const std::uint32_t code = hui::gfx::next_codepoint(title, &i);
            EXPECT_TRUE(font.has_glyph(code)) << "U+" << std::hex << code;
        }
        // Each character is drawn from the CJK atlas, none as '?'.
        std::vector<GlyphQuad> quads;
        font.layout(title, 0.0f, 0.0f, 32.0f, Align::left, quads);
        ASSERT_FALSE(quads.empty());
        for (const GlyphQuad &q : quads)
            EXPECT_EQ(q.texture, 9u);
        EXPECT_GT(font.measure(title, 32), font.measure("?", 32) * 2.0f);
    }
    // Latin letters in a mixed title still come from the face itself.
    std::vector<GlyphQuad> mixed;
    font.layout("A\xE6\x97\xA5", 0.0f, 0.0f, 32.0f, Align::left, mixed);
    ASSERT_EQ(mixed.size(), 2u);
    EXPECT_EQ(mixed[0].texture, 0u);
    EXPECT_EQ(mixed[1].texture, 9u);
}

TEST(Font, DecodesUtf8)
{
    std::size_t index = 0;
    const std::string text = "a\xC3\x97\xE2\x80\xA2";
    EXPECT_EQ(hui::gfx::next_codepoint(text, &index), 'a');
    EXPECT_EQ(hui::gfx::next_codepoint(text, &index), 0xD7u);
    EXPECT_EQ(hui::gfx::next_codepoint(text, &index), 0x2022u);
    EXPECT_EQ(index, text.size());
}

TEST(Font, WrapsAtWordsAndNewlines)
{
    const Font &font = inter();
    const auto lines =
        font.wrap("Connect all the islands with a network of bridges.\nNext", 30, 300);
    ASSERT_GE(lines.size(), 3u);
    for (const std::string &line : lines)
        EXPECT_LE(font.measure(line, 30), 300.0f + 1e-3f) << line;
    EXPECT_EQ(lines.back(), "Next");
}

TEST(Font, BreaksAWordWiderThanTheLine)
{
    const Font &font = inter();
    const std::string address = "https://example.org/a/very/long/address/without/any/space";
    const auto lines = font.wrap("See " + address + " now", 30, 300);
    ASSERT_GE(lines.size(), 3u);
    EXPECT_EQ(lines.front(), "See");
    std::string joined;
    for (const std::string &line : lines)
    {
        EXPECT_LE(font.measure(line, 30), 300.0f + 1e-3f) << line;
        for (const char c : line)
        {
            if (c != ' ')
                joined += c;
        }
    }
    // Nothing is lost or repeated where the word was cut.
    EXPECT_EQ(joined, "See" + address + "now");
    // A code point wider than the line still gets a line: the text is never dropped.
    const auto narrow = font.wrap("Wide", 30, 1.0f);
    ASSERT_EQ(narrow.size(), 4u);
    EXPECT_EQ(narrow.front(), "W");
    EXPECT_EQ(narrow.back(), "e");
}

} // namespace
