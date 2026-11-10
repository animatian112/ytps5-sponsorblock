// ps5-homebrew-ui - Arabic shaping and bidirectional font regressions.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/save_file.hpp"
#include "gfx/emoji.hpp"
#include "gfx/font.hpp"
#include "gfx/text_layout.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
using hui::gfx::text_layout::visual;

TEST(TextLayout, KeepsLatinAndOrdersHebrewAndNumbers)
{
    EXPECT_EQ(visual("YouTube 123 (test)"), "YouTube 123 (test)");
    EXPECT_EQ(visual("שלום"), "םולש");
    EXPECT_EQ(visual("שלום 123"), "123 םולש");
    EXPECT_EQ(visual("שלום (YouTube 123)"), "(YouTube 123) םולש");
    EXPECT_EQ(visual("Title: שלום 123"), "Title: 123 םולש");
    EXPECT_EQ(visual(""), "");
}

TEST(TextLayout, JoinsArabicAndFormsLamAlef)
{
    // Seen initial, lam-alef final ligature, meem isolated, in visual order.
    EXPECT_EQ(visual("سلام"), "\uFEE1\uFEFC\uFEB3");
    EXPECT_EQ(visual("لا"), "\uFEFB");
    EXPECT_EQ(visual("ببب"), "\uFE90\uFE92\uFE91");
    EXPECT_EQ(visual("ب\u200Cب"), "\uFE8F\uFE8F"); // ZWNJ prevents joining
    EXPECT_EQ(visual("ب\u200Dب"), "\uFE90\uFE91"); // ZWJ participates, never draws
    EXPECT_EQ(visual("سلام ١٢٣"), "١٢٣ \uFEE1\uFEFC\uFEB3");
}

TEST(TextLayout, HonorsAndHidesDirectionControls)
{
    EXPECT_EQ(visual("abc \u2067שלום\u2069 xyz"), "abc םולש xyz");
    EXPECT_EQ(visual("\u200Fשלום\u200E"), "םולש");
    EXPECT_EQ(visual("\u061C"), "");
    EXPECT_EQ(visual("\u202Babc\u202C"), "abc");
}

class RtlFont : public ::testing::Test
{
  protected:
    hui::gfx::Font font;
    hui::gfx::Font fallback;
    void SetUp() override
    {
        std::string data;
        ASSERT_TRUE(hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/google-sans-regular.huifont",
                                         &data));
        ASSERT_TRUE(font.load(data));
        ASSERT_TRUE(
            hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/dejavu-sans-rtl.huifont", &data));
        ASSERT_TRUE(fallback.load(data));
        font.add_fallback(&fallback, 7);
        ASSERT_TRUE(
            hui::save::read_file(HUI_SOURCE_DIR "/assets/emoji/twemoji.pack", &data, 16u << 20));
        ASSERT_TRUE(hui::gfx::emoji::load(std::move(data)));
    }
};

TEST_F(RtlFont, BakedFontCoversShapedArabicPersianAndHebrew)
{
    for (const std::string text :
         {"مرحبا بالعالم", "السَّلَامُ عَلَيْكُمْ", "فارسی اردو", "שלום עולם", "سلام ١٢٣"})
    {
        const auto shaped = visual(text);
        for (std::size_t i = 0; i < shaped.size();)
        {
            const auto cp = hui::gfx::next_codepoint(shaped, &i);
            EXPECT_TRUE(font.has_glyph(cp)) << std::hex << cp;
        }
    }
}

TEST_F(RtlFont, LayoutUsesFallbackAndAgreesWithMeasurement)
{
    using hui::gfx::Align;
    std::vector<hui::gfx::GlyphQuad> left, right;
    const float width = font.layout("سلام", 100, 50, 32, Align::left, left);
    EXPECT_FLOAT_EQ(width, font.measure("سلام", 32));
    EXPECT_FLOAT_EQ(font.layout("سلام", 100, 50, 32, Align::right, right), width);
    ASSERT_EQ(left.size(), 3u);
    ASSERT_EQ(right.size(), left.size());
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        EXPECT_EQ(left[i].texture, 7u);
        EXPECT_NEAR(right[i].x0, left[i].x0 - width, 0.001f);
    }
    EXPECT_NEAR(font.measure("سلام", 64), 2 * width, 0.001f);
    EXPECT_NEAR(font.measure("سلام", 32, 2), width + 4, 0.001f);
}

TEST_F(RtlFont, EmojiSequencesStayIntactInRtlText)
{
    const std::string family = "👨‍👩‍👧";
    EXPECT_EQ(visual("سلام " + family + " 🇯🇵 👍🏽"), "👍🏽 🇯🇵 " + family + " \uFEE1\uFEFC\uFEB3");
    std::vector<hui::gfx::GlyphQuad> quads;
    font.layout("سلام " + family, 0, 0, 32, hui::gfx::Align::left, quads);
    ASSERT_EQ(quads.size(), 4u);
    EXPECT_NE(quads.front().emoji, 0);
    EXPECT_EQ(hui::gfx::text_layout::next_boundary(family + "X", 0), family.size());
    EXPECT_EQ(hui::gfx::text_layout::next_boundary("بَX", 0), std::string("بَ").size());
}

TEST_F(RtlFont, FitAndWrapKeepLogicalTextAndRespectWidth)
{
    const std::string text = "مرحبا بالعالم שלום YouTube 123";
    const float limit = font.measure("مرحبا بالعالم", 32);
    const auto fitted = font.fit(text, 32, limit);
    ASSERT_FALSE(fitted.empty());
    EXPECT_LE(font.measure(fitted, 32), limit);
    EXPECT_EQ(fitted.substr(fitted.size() - 3), "…");
    EXPECT_EQ(text.find(fitted.substr(0, fitted.size() - 3)), 0u);
    const auto lines = font.wrap(text, 32, limit);
    ASSERT_GT(lines.size(), 1u);
    std::string joined;
    for (const auto &line : lines)
    {
        EXPECT_LE(font.measure(line, 32), limit + 0.001f);
        if (!joined.empty())
            joined += ' ';
        joined += line;
    }
    EXPECT_EQ(joined, text);
    const auto narrow = font.wrap("بَبَ", 32, 1);
    ASSERT_EQ(narrow.size(), 2u);
    EXPECT_EQ(narrow[0], "بَ");
    EXPECT_EQ(narrow[1], "بَ");
}

TEST(TextLayout, InvalidUtf8NeverProducesNonUnicodeScalars)
{
    for (const std::string text :
         {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF8\x80\x80", "\xE2\x82"})
    {
        for (std::size_t i = 0; i < text.size();)
        {
            const auto cp = hui::gfx::next_codepoint(text, &i);
            EXPECT_LE(cp, 0x10FFFFu);
            EXPECT_FALSE(cp >= 0xD800 && cp <= 0xDFFF);
            EXPECT_NE(cp, 0x2Fu); // overlong slash must not decode as '/'
        }
    }
}
} // namespace
