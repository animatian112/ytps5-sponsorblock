// youtube-tv - Right-to-left text tests: Arabic shaping and display order.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/bidi.hpp"

#include "core/save_file.hpp"
#include "gfx/emoji.hpp"
#include "gfx/font.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

using hui::gfx::arabic_form;
using hui::gfx::JoinForm;
using hui::gfx::visual_line;

// The tests write text as code points (U"..."), which reads far better than
// escaped UTF-8 for Arabic; these convert both ways.
std::string utf8(std::u32string_view text)
{
    std::string out;
    for (const char32_t c : text)
    {
        if (c < 0x80)
        {
            out.push_back(static_cast<char>(c));
        }
        else if (c < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else if (c < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (c >> 18)));
            out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::u32string codepoints(std::string_view text)
{
    std::u32string out;
    for (std::size_t i = 0; i < text.size();)
        out.push_back(static_cast<char32_t>(hui::gfx::next_codepoint(text, &i)));
    return out;
}

// What the renderer is handed, left to right, for one logical line.
std::u32string visual(std::u32string_view logical)
{
    const std::string text = utf8(logical);
    std::string storage;
    return codepoints(visual_line(text, &storage));
}

} // namespace

TEST(Bidi, LeftToRightTextIsLeftAloneWithoutACopy)
{
    // Latin, accents, Cyrillic, CJK and emoji: the byte scan turns them
    // all down, so the very same view comes back.
    for (const std::string &text :
         {std::string("Never Gonna Give You Up (Official Video)"), utf8(U"Caf\u00E9 \u00E0 Paris"),
          utf8(U"\u041F\u0440\u0438\u0432\u0435\u0442"), utf8(U"\u65E5\u672C\u8A9E"),
          utf8(U"Best day \U0001F602\U0001F525"), std::string()})
    {
        std::string storage = "untouched";
        const std::string_view out = visual_line(text, &storage);
        EXPECT_EQ(out.data(), text.data());
        EXPECT_EQ(out.size(), text.size());
        EXPECT_EQ(storage, "untouched");
    }
}

TEST(Bidi, ArabicLettersJoinAndReadRightToLeft)
{
    // مرحبا, "hello", the word from #6 that showed as ????: meem joins
    // reh, which joins nothing after it; hah starts again and joins beh,
    // which joins the final alef.
    EXPECT_EQ(visual(U"\u0645\u0631\u062D\u0628\u0627"),
              U"\uFE8E\uFE92\uFEA3\uFEAE\uFEE3"); // alef final ... meem initial, drawn last
    // A letter on its own keeps its isolated shape.
    EXPECT_EQ(visual(U"\u0628"), U"\uFE8F");
    // Two words: each shaped on its own, the second to the left of the first.
    EXPECT_EQ(visual(U"\u0628\u0628 \u0628\u0628"), U"\uFE90\uFE91 \uFE90\uFE91");
}

TEST(Bidi, LamAlefBecomesItsLigature)
{
    // لا on its own, and سلام ("peace"): seen initial, then lam-alef in its
    // final form, then meem alone (the ligature joins nothing after it).
    EXPECT_EQ(visual(U"\u0644\u0627"), U"\uFEFB");
    EXPECT_EQ(visual(U"\u0633\u0644\u0627\u0645"), U"\uFEE1\uFEFC\uFEB3");
    // With hamza and madda on the alef.
    EXPECT_EQ(visual(U"\u0644\u0623"), U"\uFEF7");
    EXPECT_EQ(visual(U"\u0644\u0622"), U"\uFEF5");
}

TEST(Bidi, VowelMarksDoNotBreakJoiningAndComeBeforeTheirLetter)
{
    // بَب: beh, fatha, beh. The fatha is looked through, so the first beh
    // still joins the second. Reversed, the mark is ahead of its letter,
    // where a zero-width mark is drawn over the glyph that follows it.
    EXPECT_EQ(visual(U"\u0628\u064E\u0628"), U"\uFE90\u064E\uFE91");
}

TEST(Bidi, PersianLettersHaveTheirShapes)
{
    // پ (peh) and گ (gaf): Persian letters from Presentation Forms-A.
    EXPECT_EQ(visual(U"\u067E\u06AF"), U"\uFB93\uFB58");
    EXPECT_EQ(arabic_form(0x06CC, JoinForm::medial), 0xFBFFu);
}

TEST(Bidi, NumbersInArabicKeepTheirOwnOrder)
{
    // "عدد 2024": the digits stay 2-0-2-4, to the left of the word.
    const std::u32string out = visual(U"\u0639\u062F\u062F 2024");
    EXPECT_EQ(out, U"2024 \uFEA9\uFEAA\uFECB"); // dal joins nothing after it
    // A number with separators stays one number: "1,000.5".
    EXPECT_EQ(visual(U"\u0628 1,000.5"), U"1,000.5 \uFE8F");
    // Arabic-Indic digits too.
    EXPECT_EQ(visual(U"\u0628 \u0661\u0662"), U"\u0661\u0662 \uFE8F");
}

TEST(Bidi, MixedTitlesKeepEachRunInItsDirection)
{
    // A Latin title with an Arabic word: the paragraph is left to right,
    // the Latin stays put and only the Arabic run turns round.
    EXPECT_EQ(visual(U"Live \u0628\u0628 now"), U"Live \uFE90\uFE91 now");
    // An Arabic title with a Latin name: right to left, the name intact.
    EXPECT_EQ(visual(U"\u0628\u0628 PS5 \u0628"), U"\uFE8F PS5 \uFE90\uFE91");
    // A number after Arabic in a Latin line goes left of the Arabic (UAX #9).
    EXPECT_EQ(visual(U"abc \u0628 12"), U"abc 12 \uFE8F");
}

TEST(Bidi, BracketsFaceTheRightWay)
{
    EXPECT_EQ(visual(U"(\u0628\u0628)"), U"(\uFE90\uFE91)");
    EXPECT_EQ(visual(U"\u0628 [HD]"), U"[HD] \uFE8F");
}

TEST(Bidi, EmojiSequencesStayWhole)
{
    // The emoji pack is what knows the sequences, as in the app.
    std::string pack;
    ASSERT_TRUE(hui::save::read_file(std::string(HUI_SOURCE_DIR) + "/assets/emoji/twemoji.pack",
                                     &pack, 16u << 20));
    ASSERT_TRUE(hui::gfx::emoji::load(std::move(pack)));
    // A family emoji (ZWJ sequence) inside Arabic: reordered as one piece.
    const std::u32string family = U"\U0001F468\u200D\U0001F469\u200D\U0001F467";
    const std::u32string out = visual(U"\u0628 " + family + U" \u0628");
    EXPECT_EQ(out, U"\uFE8F " + family + U" \uFE8F");
}

TEST(Bidi, HebrewReadsRightToLeft)
{
    // שלום: no shaping, only the order.
    EXPECT_EQ(visual(U"\u05E9\u05DC\u05D5\u05DD"), U"\u05DD\u05D5\u05DC\u05E9");
}

TEST(Bidi, ArabicFormFallsBackSensibly)
{
    // Alef joins only the letter before it: asked for a medial form it
    // gives its final one, for an initial one its isolated one.
    EXPECT_EQ(arabic_form(0x0627, JoinForm::medial), 0xFE8Eu);
    EXPECT_EQ(arabic_form(0x0627, JoinForm::initial), 0xFE8Du);
    // Not an Arabic letter: itself.
    EXPECT_EQ(arabic_form('A', JoinForm::final_form), static_cast<std::uint32_t>('A'));
    EXPECT_TRUE(hui::gfx::is_rtl(0x0628));
    EXPECT_TRUE(hui::gfx::is_rtl(0x05D0));
    EXPECT_FALSE(hui::gfx::is_rtl('A'));
    EXPECT_FALSE(hui::gfx::is_rtl(0x0E01)); // Thai is left to right
}
