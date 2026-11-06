// youtube-tv - Tests: the keyboard's languages, the globe key and their setting.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/keyboard_languages.hpp"
#include "component_fixture.hpp"
#include "core/settings.hpp"
#include "ui/components/keyboard.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace
{

using hui::Action;
using hui::Settings;
using hui::app::keyboard_layouts;
using hui::app::KeyboardLanguageSync;
using hui::ui::Event;
using hui::ui::Keyboard;
using hui::ui::KeyboardLayout;
using hui::ui::KeyKind;

constexpr int kEnglish = 1;
constexpr int kEnglishAndRussian = 3;
constexpr int kEnglishAndThai = 5;
constexpr int kAll = 7;

int count_kind(const KeyboardLayout &layout, KeyKind kind)
{
    int count = 0;
    for (const auto &row : layout.rows)
        for (const auto &key : row)
            count += key.kind == kind ? 1 : 0;
    return count;
}

class KeyboardLanguages : public hui::testing::ComponentFixture
{
  protected:
    KeyboardLanguages()
    {
        keys_.set_bounds({300.0f, 640.0f, 1320.0f, 410.0f});
        keys_.style.bindings = hui::ui::KeyboardBindings::standard();
        keys_.on_text = [this](std::string_view utf8) { text_.append(utf8); };
    }

    Event send(const hui::InputFrame &input)
    {
        feedback_.clear();
        const Event event = keys_.handle(input, feedback_);
        for (int i = 0; i < 30; ++i)
            keys_.update(kFrame);
        return event;
    }

    Keyboard keys_;
    std::string text_;
};

TEST_F(KeyboardLanguages, EachLanguageIsOneLayoutOfLettersThenTheSymbols)
{
    const std::vector<KeyboardLayout> layouts = keyboard_layouts(kEnglishAndRussian);
    ASSERT_EQ(layouts.size(), 3u);
    EXPECT_EQ(layouts[0].language, "EN");
    EXPECT_EQ(layouts[1].language, "RU");
    EXPECT_TRUE(layouts[2].language.empty());

    for (const KeyboardLayout &layout : layouts)
    {
        // Five rows, none wider than the grid, one shift / space / Done each
        // on the letters.
        ASSERT_EQ(layout.rows.size(), 5u) << layout.name;
        for (const auto &row : layout.rows)
        {
            int span = 0;
            for (const auto &key : row)
                span += key.span;
            EXPECT_LE(span, layout.columns) << layout.name;
        }
        if (layout.language.empty())
            continue;
        EXPECT_EQ(layout.rows[4].size(), 4u) << "symbols, globe, space, Done";
        EXPECT_EQ(count_kind(layout, KeyKind::language), 1) << layout.name;
        EXPECT_EQ(count_kind(layout, KeyKind::shift), 1) << layout.name;
        EXPECT_EQ(count_kind(layout, KeyKind::backspace), 1) << layout.name;
        EXPECT_EQ(layout.rows[3][0].kind, KeyKind::shift) << layout.name;
    }

    // The shift row types its letters, with capitals.
    EXPECT_EQ(layouts[0].rows[3][1].text, "z");
    EXPECT_EQ(layouts[0].rows[3][1].shifted, "Z");
    EXPECT_EQ(layouts[0].rows[3].size(), 11u);
    EXPECT_EQ(layouts[1].rows[3][1].text, "я");
    EXPECT_EQ(layouts[1].rows[3][1].shifted, "Я");
    EXPECT_EQ(layouts[1].rows[3].size(), 12u);
    EXPECT_EQ(layouts[1].rows[1][0].text, "й");
    EXPECT_EQ(layouts[1].rows[1][0].shifted, "Й");
    EXPECT_EQ(layouts[1].rows[2][11].text, "ё");
    EXPECT_EQ(layouts[1].rows[2][11].shifted, "Ё");
    EXPECT_TRUE(layouts[1].rows[0][10].shifted.empty()) << "'-' has no capital";
}

TEST_F(KeyboardLanguages, OneLanguageHasNoGlobeAndNoneMeansEnglish)
{
    for (const int mask : {kEnglish, 0})
    {
        const std::vector<KeyboardLayout> layouts = keyboard_layouts(mask);
        ASSERT_EQ(layouts.size(), 2u);
        EXPECT_EQ(layouts[0].language, "EN");
        EXPECT_EQ(count_kind(layouts[0], KeyKind::language), 0);
        EXPECT_EQ(layouts[0].rows[4].size(), 3u) << "symbols, space, Done";
    }
}

TEST_F(KeyboardLanguages, L3SwitchesTheLanguageAndR2ComesBackToIt)
{
    keys_.set_layouts(keyboard_layouts(kEnglishAndRussian));
    keys_.set_layout(0);
    EXPECT_EQ(keys_.language(), "EN");

    send(press(Action::l3));
    EXPECT_EQ(keys_.layout(), 1);
    EXPECT_EQ(keys_.language(), "RU");
    keys_.set_focus(1, 0);
    EXPECT_EQ(send(press(Action::confirm)), Event::changed);
    send(press(Action::jump_prev)); // L2: shift
    EXPECT_EQ(send(press(Action::confirm)), Event::changed);
    EXPECT_EQ(text_, "йЙ");

    // R2: the symbols, and again: back to Russian, not English.
    send(press(Action::jump_next));
    EXPECT_EQ(keys_.layout(), 2);
    EXPECT_EQ(keys_.language(), "RU");
    send(press(Action::jump_next));
    EXPECT_EQ(keys_.layout(), 1);

    // L3 again: English.
    send(press(Action::l3));
    EXPECT_EQ(keys_.language(), "EN");
}

TEST_F(KeyboardLanguages, TheGlobeKeySwitchesTooAndStaysUnderTheFocus)
{
    keys_.set_layouts(keyboard_layouts(kEnglishAndRussian));
    keys_.set_layout(0);
    keys_.set_focus(4, 1);
    ASSERT_EQ(keys_.focused().kind, KeyKind::language);
    send(press(Action::confirm));
    EXPECT_EQ(keys_.language(), "RU");
    EXPECT_EQ(keys_.focused().kind, KeyKind::language);

    // It draws (the globe and the language it leads to).
    hui::ui::Canvas canvas = this->canvas();
    keys_.draw(canvas);
    expect_drawn("keyboard with a globe key");
}

TEST_F(KeyboardLanguages, SyncOpensOnTheLastLanguageAndRemembersASwitch)
{
    Settings settings;
    settings.keyboard_languages = kEnglishAndRussian;
    settings.keyboard_language = 1;
    KeyboardLanguageSync sync;
    EXPECT_FALSE(sync.sync(keys_, settings));
    EXPECT_EQ(keys_.layouts().size(), 3u);
    EXPECT_EQ(keys_.language(), "RU");
    EXPECT_FALSE(sync.sync(keys_, settings)) << "nothing changed";

    send(press(Action::l3));
    EXPECT_TRUE(sync.sync(keys_, settings));
    EXPECT_EQ(settings.keyboard_language, 0);

    // Russian turned off in Settings: English letters and the symbols.
    settings.keyboard_language = 1;
    settings.keyboard_languages = kEnglish;
    sync.sync(keys_, settings);
    EXPECT_EQ(keys_.layouts().size(), 2u);
    EXPECT_EQ(keys_.layout(), 0);
    EXPECT_EQ(keys_.language(), "EN");
}

TEST_F(KeyboardLanguages, ThaiIsKedmaneeWithItsOwnShiftLayer)
{
    const std::vector<KeyboardLayout> layouts = keyboard_layouts(kEnglishAndThai);
    ASSERT_EQ(layouts.size(), 3u);
    const KeyboardLayout &thai = layouts[1];
    EXPECT_EQ(thai.language, "TH");
    EXPECT_EQ(thai.columns, 13);
    ASSERT_EQ(thai.rows.size(), 5u);
    // 12 + backspace, 13, 11, shift + 10, then symbols, globe, space, Done.
    EXPECT_EQ(thai.rows[0].size(), 13u);
    EXPECT_EQ(thai.rows[0].back().kind, KeyKind::backspace);
    EXPECT_EQ(thai.rows[1].size(), 13u);
    EXPECT_EQ(thai.rows[2].size(), 11u);
    EXPECT_EQ(thai.rows[3].size(), 11u);
    EXPECT_EQ(thai.rows[3][0].kind, KeyKind::shift);
    EXPECT_EQ(count_kind(thai, KeyKind::language), 1);
    for (const auto &row : thai.rows)
    {
        int span = 0;
        for (const auto &key : row)
        {
            span += key.span;
            if (key.kind == KeyKind::character)
                EXPECT_FALSE(key.shifted.empty()) << key.text << " has a shifted character";
        }
        EXPECT_LE(span, thai.columns);
    }
    // Where a Thai typist expects them: ก on D, ่ on J, ฟ on A, ๑ over 2.
    EXPECT_EQ(thai.rows[2][2].text, "ก");
    EXPECT_EQ(thai.rows[2][6].text, "่");
    EXPECT_EQ(thai.rows[2][0].text, "ฟ");
    EXPECT_EQ(thai.rows[2][0].shifted, "ฤ");
    EXPECT_EQ(thai.rows[0][1].shifted, "๑");
    EXPECT_EQ(thai.rows[1][1].shifted, "\"");
}

TEST_F(KeyboardLanguages, L3WalksThroughEveryLanguage)
{
    keys_.set_layouts(keyboard_layouts(kAll));
    keys_.set_layout(0);
    ASSERT_EQ(keys_.layouts().size(), 4u);
    std::vector<std::string> seen{keys_.language()};
    for (int i = 0; i < 3; ++i)
    {
        send(press(Action::l3));
        seen.push_back(keys_.language());
    }
    EXPECT_EQ(seen, (std::vector<std::string>{"EN", "RU", "TH", "EN"}));

    // Thai types its letter, and with shift the other one on that key.
    send(press(Action::l3));
    send(press(Action::l3));
    ASSERT_EQ(keys_.language(), "TH");
    keys_.set_focus(2, 2);
    send(press(Action::confirm));
    send(press(Action::jump_prev));
    send(press(Action::confirm));
    EXPECT_EQ(text_, "กฏ");

    // A key that types a mark draws (on its dotted circle).
    keys_.set_focus(2, 6);
    hui::ui::Canvas canvas = this->canvas();
    keys_.draw(canvas);
    expect_drawn("Thai keyboard");
}

TEST(KeyboardLanguageSettings, SurviveTheSaveFile)
{
    Settings settings;
    settings.keyboard_languages = kEnglishAndRussian;
    settings.keyboard_language = 1;
    Settings loaded;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &loaded));
    EXPECT_EQ(loaded.keyboard_languages, kEnglishAndRussian);
    EXPECT_EQ(loaded.keyboard_language, 1);
    EXPECT_EQ(Settings{}.keyboard_languages, kEnglish) << "English only by default";
}

} // namespace
