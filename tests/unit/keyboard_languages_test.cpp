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
#include <utility>
#include <vector>

namespace
{

using hui::Action;
using hui::Settings;
using hui::app::is_kana_modifier;
using hui::app::keyboard_language_index;
using hui::app::keyboard_languages;
using hui::app::keyboard_layouts;
using hui::app::KeyboardLanguageSync;
using hui::app::next_kana_form;
using hui::ui::Event;
using hui::ui::Keyboard;
using hui::ui::KeyboardLayout;
using hui::ui::KeyKind;

constexpr int kEnglish = 1;
constexpr int kEnglishAndRussian = 3;
// Bits 2, 3 and 4: Italian, Spanish and Japanese.
constexpr int kItalian = 1 << 2;
constexpr int kSpanish = 1 << 3;
constexpr int kJapanese = 1 << 4;

// The key that types `text` in a layout, or null.
const hui::ui::KeyboardKey *find_key(const KeyboardLayout &layout, std::string_view text)
{
    for (const auto &row : layout.rows)
        for (const auto &key : row)
            if (key.kind == KeyKind::character && key.text == text)
                return &key;
    return nullptr;
}

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

TEST_F(KeyboardLanguages, TheTableKeepsItsOrderAndEveryLanguageFitsItsGrid)
{
    // Settings stores a bit per position: the first two never move.
    const auto languages = keyboard_languages();
    ASSERT_EQ(languages.size(), 5u);
    EXPECT_EQ(keyboard_language_index("EN"), 0);
    EXPECT_EQ(keyboard_language_index("RU"), 1);
    EXPECT_EQ(keyboard_language_index("IT"), 2);
    EXPECT_EQ(keyboard_language_index("ES"), 3);
    EXPECT_EQ(keyboard_language_index("JA"), 4);
    EXPECT_EQ(keyboard_language_index("ZH"), -1);

    const std::vector<KeyboardLayout> layouts = keyboard_layouts((1 << 5) - 1);
    ASSERT_EQ(layouts.size(), 6u) << "five languages, then the symbols";
    for (std::size_t i = 0; i < languages.size(); ++i)
    {
        const KeyboardLayout &layout = layouts[i];
        EXPECT_EQ(layout.language, languages[i].tag);
        for (const auto &row : layout.rows)
        {
            int span = 0;
            for (const auto &key : row)
                span += key.span;
            EXPECT_LE(span, layout.columns) << layout.language;
        }
        EXPECT_EQ(count_kind(layout, KeyKind::language), 1) << layout.language;
        EXPECT_EQ(count_kind(layout, KeyKind::shift), 1) << layout.language;
        EXPECT_EQ(count_kind(layout, KeyKind::backspace), 1) << layout.language;
        EXPECT_EQ(count_kind(layout, KeyKind::done), 1) << layout.language;
    }
}

TEST_F(KeyboardLanguages, ItalianAndSpanishHaveTheirAccentedLettersWithCapitals)
{
    const KeyboardLayout italian = keyboard_layouts(kItalian)[0];
    EXPECT_EQ(italian.language, "IT");
    for (const auto &[small, capital] : {std::pair<std::string_view, std::string_view>{"è", "È"},
                                         {"é", "É"},
                                         {"ò", "Ò"},
                                         {"à", "À"},
                                         {"ù", "Ù"},
                                         {"ì", "Ì"}})
    {
        const auto *key = find_key(italian, small);
        ASSERT_NE(key, nullptr) << small;
        EXPECT_EQ(key->shifted, capital);
    }
    ASSERT_NE(find_key(italian, "'"), nullptr) << "l'acqua";

    const KeyboardLayout spanish = keyboard_layouts(kSpanish)[0];
    EXPECT_EQ(spanish.language, "ES");
    for (const auto &[small, capital] : {std::pair<std::string_view, std::string_view>{"ñ", "Ñ"},
                                         {"á", "Á"},
                                         {"é", "É"},
                                         {"í", "Í"},
                                         {"ó", "Ó"},
                                         {"ú", "Ú"},
                                         {"ü", "Ü"}})
    {
        const auto *key = find_key(spanish, small);
        ASSERT_NE(key, nullptr) << small;
        EXPECT_EQ(key->shifted, capital);
    }
    EXPECT_NE(find_key(spanish, "¿"), nullptr);
}

TEST_F(KeyboardLanguages, JapaneseTypesHiraganaAndShiftTypesKatakana)
{
    const KeyboardLayout japanese = keyboard_layouts(kJapanese)[0];
    EXPECT_EQ(japanese.language, "JA");
    // The gojūon table: a column per consonant, the vowels down it.
    EXPECT_EQ(japanese.rows[0][0].text, "あ");
    EXPECT_EQ(japanese.rows[0][1].text, "か");
    EXPECT_EQ(japanese.rows[1][1].text, "き");
    EXPECT_EQ(japanese.rows[2][9].text, "ん");
    for (const std::string_view kana : {"あ", "か", "し", "つ", "の", "ふ", "ゆ", "ろ", "を", "ん"})
        EXPECT_NE(find_key(japanese, kana), nullptr) << kana;
    EXPECT_EQ(find_key(japanese, "か")->shifted, "カ");
    EXPECT_EQ(find_key(japanese, "ん")->shifted, "ン");
    EXPECT_EQ(find_key(japanese, "ー")->shifted, "ー") << "the long mark is both";
    const auto *modifier = find_key(japanese, hui::app::kKanaModifier);
    ASSERT_NE(modifier, nullptr);
    EXPECT_EQ(modifier->label, "゛゜小");
    EXPECT_TRUE(is_kana_modifier(modifier->text));
    EXPECT_FALSE(is_kana_modifier("か"));

    // Typing it: か, then with shift カ.
    keys_.set_layouts(keyboard_layouts(kJapanese));
    keys_.set_focus(0, 1);
    send(press(Action::confirm));
    send(press(Action::jump_prev)); // L2: shift
    send(press(Action::confirm));
    EXPECT_EQ(text_, "かカ");
}

TEST(KanaModifier, StepsTheLastKanaThroughItsForms)
{
    EXPECT_EQ(next_kana_form("か"), "が");
    EXPECT_EQ(next_kana_form("が"), "か");
    EXPECT_EQ(next_kana_form("は"), "ば");
    EXPECT_EQ(next_kana_form("ば"), "ぱ");
    EXPECT_EQ(next_kana_form("ぱ"), "は");
    EXPECT_EQ(next_kana_form("つ"), "っ");
    EXPECT_EQ(next_kana_form("っ"), "づ");
    EXPECT_EQ(next_kana_form("づ"), "つ");
    EXPECT_EQ(next_kana_form("う"), "ぅ");
    EXPECT_EQ(next_kana_form("ぅ"), "ゔ");
    EXPECT_EQ(next_kana_form("や"), "ゃ");
    // Katakana alike, and only the last character counts.
    EXPECT_EQ(next_kana_form("ラーメンカ"), "ガ");
    EXPECT_EQ(next_kana_form("ホ"), "ボ");
    EXPECT_EQ(next_kana_form("ツ"), "ッ");
    // Nothing to change: no form, not kana, or nothing typed.
    EXPECT_EQ(next_kana_form("な"), "");
    EXPECT_EQ(next_kana_form("ー"), "");
    EXPECT_EQ(next_kana_form("abc"), "");
    EXPECT_EQ(next_kana_form(""), "");
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
