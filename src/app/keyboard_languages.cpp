// youtube-tv - The languages the on-screen keyboard types.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/keyboard_languages.hpp"

#include "core/settings.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace hui::app
{

namespace
{

using ui::KeyboardKey;
using ui::KeyboardLayout;
using ui::KeyKind;

KeyboardKey wide(KeyKind kind, int span)
{
    KeyboardKey key;
    key.kind = kind;
    key.span = span;
    return key;
}

// A row that starts with the shift key, then a key per character.
void add_shift_row(KeyboardLayout &layout, std::string_view characters)
{
    KeyboardLayout keys;
    keys.add_row(characters);
    std::vector<KeyboardKey> row{wide(KeyKind::shift, 1)};
    for (KeyboardKey &key : keys.rows.back())
        row.push_back(std::move(key));
    layout.add_row(std::move(row));
}

// The UTF-8 characters of a string, one string each.
std::vector<std::string> characters(std::string_view text)
{
    std::vector<std::string> out;
    for (std::size_t at = 0; at < text.size();)
    {
        std::size_t next = at + 1;
        while (next < text.size() && (static_cast<unsigned char>(text[next]) & 0xc0) == 0x80)
            ++next;
        out.emplace_back(text.substr(at, next - at));
        at = next;
    }
    return out;
}

// Keys for a script whose shift layer is not capitals: plain[i] types what
// it shows, shifted[i] what it types with shift.
std::vector<KeyboardKey> paired_keys(std::string_view plain, std::string_view shifted)
{
    const std::vector<std::string> lower = characters(plain);
    const std::vector<std::string> upper = characters(shifted);
    std::vector<KeyboardKey> keys;
    for (std::size_t i = 0; i < lower.size(); ++i)
    {
        KeyboardKey key;
        key.text = lower[i];
        if (i < upper.size())
            key.shifted = upper[i];
        keys.push_back(std::move(key));
    }
    return keys;
}

// The bottom row of every language: symbols, the globe (with more than one
// language), space and Done across the grid's width.
void add_bottom_row(KeyboardLayout &layout, bool with_globe)
{
    const int globe = with_globe ? 2 : 0;
    std::vector<KeyboardKey> row{wide(KeyKind::layout, 2)};
    if (with_globe)
        row.push_back(wide(KeyKind::language, globe));
    row.push_back(wide(KeyKind::space, layout.columns - 2 - globe - 3));
    row.push_back(wide(KeyKind::done, 3));
    layout.add_row(std::move(row));
}

// The letters the Search keyboard always had: QWERTY on 11 columns.
KeyboardLayout english(bool with_globe)
{
    KeyboardLayout layout;
    layout.name = "abc";
    layout.language = "EN";
    layout.columns = 11;
    layout.add_row("1234567890");
    layout.rows.back().push_back(wide(KeyKind::backspace, 1));
    layout.add_row("qwertyuiop-");
    layout.add_row("asdfghjkl'&");
    layout.rows[2][9].shifted = "\"";
    add_shift_row(layout, "zxcvbnm,.?");
    add_bottom_row(layout, with_globe);
    return layout;
}

// ЙЦУКЕН, the Russian typewriter layout, on 12 columns so all 33 letters
// have a key of their own.
KeyboardLayout russian(bool with_globe)
{
    KeyboardLayout layout;
    layout.name = "абв";
    layout.language = "RU";
    layout.columns = 12;
    layout.add_row("1234567890-");
    layout.rows.back().push_back(wide(KeyKind::backspace, 1));
    layout.add_row("йцукенгшщзхъ");
    layout.add_row("фывапролджэё");
    add_shift_row(layout, "ячсмитьбю,.");
    add_bottom_row(layout, with_globe);
    return layout;
}

// Kedmanee (TIS 820-2538), the Thai typewriter layout, key for key: the
// number row and the three letter rows, each key with its shifted character.
// 13 columns, as on the typewriter; Arabic digits are on the symbols.
KeyboardLayout thai(bool with_globe)
{
    KeyboardLayout layout;
    layout.name = "กขค";
    layout.language = "TH";
    layout.columns = 13;
    layout.add_row(paired_keys("ๅ/-ภถุึคตจขช", "+๑๒๓๔ู฿๕๖๗๘๙"));
    layout.rows.back().push_back(wide(KeyKind::backspace, 1));
    layout.add_row(paired_keys("ๆไำพะัีรนยบลฃ", "๐\"ฎฑธํ๊ณฯญฐ,ฅ"));
    layout.add_row(paired_keys("ฟหกดเ้่าสวง", "ฤฆฏโฌ็๋ษศซ."));
    std::vector<KeyboardKey> shift_row{wide(KeyKind::shift, 1)};
    for (KeyboardKey &key : paired_keys("ผปแอิืทมใฝ", "()ฉฮฺ์?ฒฬฦ"))
        shift_row.push_back(std::move(key));
    layout.add_row(std::move(shift_row));
    add_bottom_row(layout, with_globe);
    return layout;
}

// Settings::keyboard_languages has a bit per entry, in this order: append only.
constexpr KeyboardLanguage kLanguages[] = {
    {"English", "EN", english},
    {"Russian", "RU", russian},
    {"Thai", "TH", thai},
};

} // namespace

std::span<const KeyboardLanguage> keyboard_languages()
{
    return kLanguages;
}

int keyboard_language_index(std::string_view tag)
{
    for (std::size_t i = 0; i < std::size(kLanguages); ++i)
        if (tag == kLanguages[i].tag)
            return static_cast<int>(i);
    return -1;
}

std::vector<KeyboardLayout> keyboard_layouts(int mask)
{
    const int count = static_cast<int>(std::size(kLanguages));
    int shown = 0;
    for (int i = 0; i < count; ++i)
        shown += (mask >> i) & 1;
    if (shown == 0)
    {
        mask = 1; // English
        shown = 1;
    }
    std::vector<KeyboardLayout> layouts;
    for (int i = 0; i < count; ++i)
        if ((mask >> i) & 1)
            layouts.push_back(kLanguages[i].letters(shown > 1));
    KeyboardLayout symbols = KeyboardLayout::symbols();
    symbols.name = "@#:";
    layouts.push_back(std::move(symbols));
    return layouts;
}

bool KeyboardLanguageSync::sync(ui::Keyboard &keys, Settings &settings)
{
    if (settings.keyboard_languages != mask_)
    {
        mask_ = settings.keyboard_languages;
        keys.set_layouts(keyboard_layouts(mask_));
        keys.set_shift(ui::KeyboardShift::off);
        const auto languages = keyboard_languages();
        const int last = settings.keyboard_language;
        if (last < 0 || last >= static_cast<int>(languages.size()) ||
            !keys.set_language(languages[static_cast<std::size_t>(last)].tag))
            keys.set_layout(0);
        return false;
    }
    const int index = keyboard_language_index(keys.language());
    if (index < 0 || index == settings.keyboard_language)
        return false;
    settings.keyboard_language = index;
    return true;
}

} // namespace hui::app
