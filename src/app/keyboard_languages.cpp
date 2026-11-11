// youtube-tv - The languages the on-screen keyboard types.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/keyboard_languages.hpp"

#include "core/settings.hpp"
#include "gfx/font.hpp"

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

// The code points of a UTF-8 string, and back.
std::u32string decode(std::string_view text)
{
    std::u32string out;
    std::size_t i = 0;
    while (i < text.size())
        out.push_back(gfx::next_codepoint(text, &i));
    return out;
}

std::string encode(std::u32string_view codes)
{
    std::string out;
    for (const char32_t code : codes)
    {
        if (code < 0x80)
            out += static_cast<char>(code);
        else if (code < 0x800)
        {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else if (code < 0x10000)
        {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return out;
}

// Hiragana (U+3041 to U+3096) move 0x60 up to their katakana; the rest stay.
std::string katakana(std::string_view text)
{
    std::u32string codes = decode(text);
    for (char32_t &code : codes)
        if (code >= 0x3041 && code <= 0x3096)
            code += 0x60;
    return encode(codes);
}

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

// Italian: QWERTY with the accented vowels Italian writes (è é ò à ù ì) on
// keys of their own, and the apostrophe of l'acqua by the digits.
KeyboardLayout italian(bool with_globe)
{
    KeyboardLayout layout;
    layout.name = "abc";
    layout.language = "IT";
    layout.columns = 12;
    layout.add_row("1234567890'");
    layout.rows.back().push_back(wide(KeyKind::backspace, 1));
    layout.add_row("qwertyuiopèé");
    layout.add_row("asdfghjklòàù");
    add_shift_row(layout, "zxcvbnm,.ì?");
    add_bottom_row(layout, with_globe);
    return layout;
}

// Spanish: QWERTY with ñ where the Spanish keyboard has it, the acute vowels
// and ü, and the opening question mark.
KeyboardLayout spanish(bool with_globe)
{
    KeyboardLayout layout;
    layout.name = "abc";
    layout.language = "ES";
    layout.columns = 12;
    layout.add_row("1234567890¿");
    layout.rows.back().push_back(wide(KeyKind::backspace, 1));
    layout.add_row("qwertyuiopáé");
    layout.add_row("asdfghjklñíó");
    add_shift_row(layout, "zxcvbnm,.úü");
    add_bottom_row(layout, with_globe);
    return layout;
}

// Hiragana in the order of the gojūon table, a column per consonant (あかさ
// たなはまやらわ) read down the vowels, as Japanese TV keyboards lay it out.
// Shift types the same key in katakana. The ゛゜小 key changes the kana before
// it, as on a phone: か to が, は to ば to ぱ, つ to っ to づ.
KeyboardLayout japanese(bool with_globe)
{
    KeyboardLayout layout;
    layout.name = "あいう";
    layout.language = "JA";
    layout.columns = 12;
    layout.add_row("あかさたなはまやらわー");
    layout.rows.back().push_back(wide(KeyKind::backspace, 1));
    layout.add_row("いきしちにひみゆりを、。");
    layout.add_row("うくすつぬふむよるん？！");
    add_shift_row(layout, "えけせてねへめ「れ」");
    layout.add_row("おこそとのほも・ろ〜");
    KeyboardKey modifier;
    modifier.text = std::string(kKanaModifier);
    modifier.label = "゛゜小";
    layout.rows.back().push_back(std::move(modifier));
    for (auto &row : layout.rows)
        for (KeyboardKey &key : row)
            if (key.kind == KeyKind::character && key.shifted.empty())
                key.shifted = katakana(key.text);
    add_bottom_row(layout, with_globe);
    return layout;
}

// Settings::keyboard_languages has a bit per entry, in this order: append only.
constexpr KeyboardLanguage kLanguages[] = {
    {"English", "EN", english}, {"Russian", "RU", russian},   {"Italian", "IT", italian},
    {"Spanish", "ES", spanish}, {"Japanese", "JA", japanese},
};

// The forms the ゛゜小 key steps a kana through, each cycle in order (the last
// goes back to the first). Hiragana; katakana are the same 0x60 higher.
constexpr std::string_view kKanaCycles[] = {
    "かが", "きぎ", "くぐ",   "けげ", "こご", "さざ",   "しじ",   "すず",   "せぜ",   "そぞ",
    "ただ", "ちぢ", "つっづ", "てで", "とど", "はばぱ", "ひびぴ", "ふぶぷ", "へべぺ", "ほぼぽ",
    "あぁ", "いぃ", "うぅゔ", "えぇ", "おぉ", "やゃ",   "ゆゅ",   "よょ",   "わゎ",
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

bool is_kana_modifier(std::string_view typed)
{
    return typed == kKanaModifier;
}

std::string next_kana_form(std::string_view text)
{
    const std::u32string codes = decode(text);
    if (codes.empty())
        return {};
    char32_t last = codes.back();
    const bool katakana_form = last >= 0x30A1 && last <= 0x30F6;
    if (katakana_form)
        last -= 0x60;
    for (const std::string_view cycle : kKanaCycles)
    {
        const std::u32string forms = decode(cycle);
        for (std::size_t i = 0; i < forms.size(); ++i)
        {
            if (forms[i] != last)
                continue;
            char32_t next = forms[(i + 1) % forms.size()];
            if (katakana_form)
                next += 0x60;
            return encode(std::u32string_view(&next, 1));
        }
    }
    return {};
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
