// youtube-tv - Right-to-left text: Arabic letter shapes and display order.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/bidi.hpp"

#include "gfx/emoji.hpp"
#include "gfx/font.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace hui::gfx
{

namespace
{

// One Arabic letter and its presentation forms; 0 where it has none. A
// letter with initial and medial forms joins on both sides ("dual"); one with
// only a final form joins the letter before it but never the one after
// (alef, dal, reh, waw...); hamza joins nothing. Generated from the Unicode
// decompositions of U+FB50-U+FDFF and U+FE70-U+FEFF, keeping the letters
// whose every form the Arabic face (DejaVu Sans) draws. A letter missing here
// (heh goal, yeh barree...) is drawn as itself and breaks the joining.
struct Forms
{
    std::uint16_t letter, isolated, final_form, initial, medial;
};

constexpr std::array<Forms, 68> kForms{{
    {0x0621, 0xFE80, 0x0000, 0x0000, 0x0000}, // hamza
    {0x0622, 0xFE81, 0xFE82, 0x0000, 0x0000}, // alef with madda above
    {0x0623, 0xFE83, 0xFE84, 0x0000, 0x0000}, // alef with hamza above
    {0x0624, 0xFE85, 0xFE86, 0x0000, 0x0000}, // waw with hamza above
    {0x0625, 0xFE87, 0xFE88, 0x0000, 0x0000}, // alef with hamza below
    {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C}, // yeh with hamza above
    {0x0627, 0xFE8D, 0xFE8E, 0x0000, 0x0000}, // alef
    {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92}, // beh
    {0x0629, 0xFE93, 0xFE94, 0x0000, 0x0000}, // teh marbuta
    {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98}, // teh
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C}, // theh
    {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0}, // jeem
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4}, // hah
    {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8}, // khah
    {0x062F, 0xFEA9, 0xFEAA, 0x0000, 0x0000}, // dal
    {0x0630, 0xFEAB, 0xFEAC, 0x0000, 0x0000}, // thal
    {0x0631, 0xFEAD, 0xFEAE, 0x0000, 0x0000}, // reh
    {0x0632, 0xFEAF, 0xFEB0, 0x0000, 0x0000}, // zain
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4}, // seen
    {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8}, // sheen
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC}, // sad
    {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0}, // dad
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4}, // tah
    {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8}, // zah
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC}, // ain
    {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0}, // ghain
    {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4}, // feh
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8}, // qaf
    {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC}, // kaf
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0}, // lam
    {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4}, // meem
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8}, // noon
    {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC}, // heh
    {0x0648, 0xFEED, 0xFEEE, 0x0000, 0x0000}, // waw
    {0x0649, 0xFEEF, 0xFEF0, 0xFBE8, 0xFBE9}, // alef maksura
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4}, // yeh
    {0x0679, 0xFB66, 0xFB67, 0xFB68, 0xFB69}, // tteh
    {0x067A, 0xFB5E, 0xFB5F, 0xFB60, 0xFB61}, // tteheh
    {0x067B, 0xFB52, 0xFB53, 0xFB54, 0xFB55}, // beeh
    {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59}, // peh
    {0x067F, 0xFB62, 0xFB63, 0xFB64, 0xFB65}, // teheh
    {0x0680, 0xFB5A, 0xFB5B, 0xFB5C, 0xFB5D}, // beheh
    {0x0683, 0xFB76, 0xFB77, 0xFB78, 0xFB79}, // nyeh
    {0x0684, 0xFB72, 0xFB73, 0xFB74, 0xFB75}, // dyeh
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D}, // tcheh
    {0x0687, 0xFB7E, 0xFB7F, 0xFB80, 0xFB81}, // tcheheh
    {0x0688, 0xFB88, 0xFB89, 0x0000, 0x0000}, // ddal
    {0x068C, 0xFB84, 0xFB85, 0x0000, 0x0000}, // dahal
    {0x068D, 0xFB82, 0xFB83, 0x0000, 0x0000}, // ddahal
    {0x068E, 0xFB86, 0xFB87, 0x0000, 0x0000}, // dul
    {0x0691, 0xFB8C, 0xFB8D, 0x0000, 0x0000}, // rreh
    {0x0698, 0xFB8A, 0xFB8B, 0x0000, 0x0000}, // jeh
    {0x06A4, 0xFB6A, 0xFB6B, 0xFB6C, 0xFB6D}, // veh
    {0x06A6, 0xFB6E, 0xFB6F, 0xFB70, 0xFB71}, // peheh
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91}, // keheh
    {0x06AD, 0xFBD3, 0xFBD4, 0xFBD5, 0xFBD6}, // ng
    {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95}, // gaf
    {0x06B1, 0xFB9A, 0xFB9B, 0xFB9C, 0xFB9D}, // ngoeh
    {0x06B3, 0xFB96, 0xFB97, 0xFB98, 0xFB99}, // gueh
    {0x06BA, 0xFB9E, 0xFB9F, 0x0000, 0x0000}, // noon ghunna
    {0x06BB, 0xFBA0, 0xFBA1, 0xFBA2, 0xFBA3}, // rnoon
    {0x06BE, 0xFBAA, 0xFBAB, 0xFBAC, 0xFBAD}, // heh doachashmee
    {0x06C6, 0xFBD9, 0xFBDA, 0x0000, 0x0000}, // oe
    {0x06C7, 0xFBD7, 0xFBD8, 0x0000, 0x0000}, // u
    {0x06C8, 0xFBDB, 0xFBDC, 0x0000, 0x0000}, // yu
    {0x06CB, 0xFBDE, 0xFBDF, 0x0000, 0x0000}, // ve
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF}, // farsi yeh
    {0x06D0, 0xFBE4, 0xFBE5, 0xFBE6, 0xFBE7}, // e
}};

constexpr std::uint32_t kLam = 0x0644;
constexpr std::uint32_t kTatweel = 0x0640; // the joining stroke: joins both sides

const Forms *forms_of(std::uint32_t letter)
{
    const auto it = std::lower_bound(kForms.begin(), kForms.end(), letter,
                                     [](const Forms &f, std::uint32_t c) { return f.letter < c; });
    return it != kForms.end() && it->letter == letter ? &*it : nullptr;
}

// Joins the letter after it (on its left, in a right-to-left word).
bool joins_next(std::uint32_t c)
{
    const Forms *f = forms_of(c);
    return c == kTatweel || (f != nullptr && f->initial != 0);
}

// Joins the letter before it.
bool joins_previous(std::uint32_t c)
{
    const Forms *f = forms_of(c);
    return c == kTatweel || (f != nullptr && f->final_form != 0);
}

// lam + this alef: the ligature's isolated form (its final form is one more),
// or 0 when the code point is no alef.
std::uint32_t lam_alef(std::uint32_t alef)
{
    switch (alef)
    {
    case 0x0622:
        return 0xFEF5;
    case 0x0623:
        return 0xFEF7;
    case 0x0625:
        return 0xFEF9;
    case 0x0627:
        return 0xFEFB;
    default:
        return 0;
    }
}

// Combining marks: vowel signs and accents that sit on the letter before
// them. Shaping looks through them (an Arabic vowel does not break a word's
// joining) and the bidi rules give them their letter's direction (W1).
bool is_mark(std::uint32_t c)
{
    return (c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489) ||
           (c >= 0x0591 && c <= 0x05BD) || c == 0x05BF || c == 0x05C1 || c == 0x05C2 ||
           c == 0x05C4 || c == 0x05C5 || c == 0x05C7 || (c >= 0x0610 && c <= 0x061A) ||
           (c >= 0x064B && c <= 0x065F) || c == 0x0670 || (c >= 0x06D6 && c <= 0x06DC) ||
           (c >= 0x06DF && c <= 0x06E4) || c == 0x06E7 || c == 0x06E8 ||
           (c >= 0x06EA && c <= 0x06ED) || c == 0x0E31 || (c >= 0x0E34 && c <= 0x0E3A) ||
           (c >= 0x0E47 && c <= 0x0E4E) || (c >= 0x20D0 && c <= 0x20FF) ||
           (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xFE20 && c <= 0xFE2F);
}

// Bidi character types, as many as one line without embeddings needs.
// Whitespace and other neutrals are all ON.
enum class Type : std::uint8_t
{
    L,   // left to right letter
    R,   // right to left letter (Hebrew R and Arabic AL alike)
    EN,  // European digit
    AN,  // Arabic-Indic digit
    ES,  // plus or minus between digits
    ET,  // currency, percent, degree: a number's terminator
    CS,  // comma, full stop, colon, slash between digits
    NSM, // combining mark
    ON,  // neutral
};

Type classify(std::uint32_t c)
{
    if (c >= '0' && c <= '9')
        return Type::EN;
    if (c >= 0x06F0 && c <= 0x06F9) // Persian digits are European ones to UAX #9
        return Type::EN;
    if ((c >= 0x0660 && c <= 0x0669) || c == 0x066B || c == 0x066C)
        return Type::AN;
    if (c == '+' || c == '-')
        return Type::ES;
    if (c == '#' || c == '$' || c == '%' || (c >= 0xA2 && c <= 0xA5) || c == 0xB0 || c == 0xB1 ||
        (c >= 0x20A0 && c <= 0x20CF) || c == 0x066A)
        return Type::ET;
    if (c == ',' || c == '.' || c == ':' || c == '/' || c == 0xA0 || c == 0x060C)
        return Type::CS;
    if (is_mark(c))
        return Type::NSM;
    if (c == 0x200E) // left-to-right mark
        return Type::L;
    if (is_rtl(c))
        return Type::R;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        return Type::L;
    // Punctuation and symbols are neutral; every other letter (Latin
    // accents, Cyrillic, Greek, CJK, Thai) is left to right.
    if (c < 0xC0 || c == 0xD7 || c == 0xF7 || (c >= 0x2000 && c <= 0x2BFF) ||
        (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE10 && c <= 0xFE6F) ||
        (c >= 0xFF01 && c <= 0xFF20) || (c >= 0x1F000 && c <= 0x1FAFF))
        return Type::ON;
    return Type::L;
}

// What a neutral sees of its neighbour (N1): numbers count as right to left.
Type strong_of(Type t)
{
    return t == Type::L ? Type::L : Type::R;
}

std::uint32_t mirrored(std::uint32_t c)
{
    switch (c)
    {
    case '(':
        return ')';
    case ')':
        return '(';
    case '[':
        return ']';
    case ']':
        return '[';
    case '{':
        return '}';
    case '}':
        return '{';
    case '<':
        return '>';
    case '>':
        return '<';
    case 0xAB:
        return 0xBB; // « »
    case 0xBB:
        return 0xAB;
    case 0x2039:
        return 0x203A; // ‹ ›
    case 0x203A:
        return 0x2039;
    default:
        return c;
    }
}

void append_utf8(std::string *out, std::uint32_t c)
{
    if (c < 0x80)
    {
        out->push_back(static_cast<char>(c));
    }
    else if (c < 0x800)
    {
        out->push_back(static_cast<char>(0xC0 | (c >> 6)));
        out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else if (c < 0x10000)
    {
        out->push_back(static_cast<char>(0xE0 | (c >> 12)));
        out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
    else
    {
        out->push_back(static_cast<char>(0xF0 | (c >> 18)));
        out->push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

// Could the UTF-8 hold a right-to-left letter? Looks at lead bytes only, so a
// Latin, Cyrillic or CJK title is turned down without decoding it.
bool may_be_rtl(std::string_view text)
{
    const auto at = [&](std::size_t i)
    { return i < text.size() ? static_cast<unsigned char>(text[i]) : 0u; };
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char b = at(i);
        if (b < 0xD6)
            continue;
        if (b <= 0xDF) // U+0580-U+07FF: Hebrew, Arabic, Syriac, Thaana, N'Ko
            return true;
        const unsigned char next = at(i + 1);
        if (b == 0xE0 && next >= 0xA0 && next <= 0xA3) // U+0800-U+08FF
            return true;
        if (b == 0xEF && next >= 0xAC && next <= 0xBB) // U+FB00-U+FEFF: presentation forms
            return true;
        if (b == 0xE2 && next == 0x80 && at(i + 2) == 0x8F) // right-to-left mark
            return true;
        if (b == 0xF0 && (next == 0x90 || next == 0x9E) && at(i + 2) >= 0xA0) // U+10800, U+1E800
            return true;
    }
    return false;
}

// A code point, or a whole emoji sequence, of the line.
struct Unit
{
    std::size_t begin = 0;
    std::size_t end = 0;
    std::uint32_t codepoint = 0; // 0 for an emoji
    Type type = Type::ON;
    std::uint8_t level = 0;
};

// Swaps each Arabic letter for its form, looking through marks to its
// neighbours, and merges lam + alef into one unit holding the ligature.
void shape(std::vector<Unit> *units)
{
    std::vector<Unit> &u = *units;
    std::vector<std::uint32_t> letters(u.size());
    for (std::size_t i = 0; i < u.size(); ++i)
        letters[i] = u[i].codepoint;
    const auto neighbour = [&](std::size_t i, int step) -> std::uint32_t
    {
        for (std::size_t j = i + static_cast<std::size_t>(step); j < u.size();
             j += static_cast<std::size_t>(step))
        {
            if (!is_mark(letters[j]) || letters[j] == 0)
                return letters[j];
        }
        return 0; // the size_t wraps past 0 when stepping back: also the end
    };
    std::vector<Unit> shaped;
    shaped.reserve(u.size());
    for (std::size_t i = 0; i < u.size(); ++i)
    {
        Unit unit = u[i];
        const std::uint32_t c = letters[i];
        const Forms *f = forms_of(c);
        if (f == nullptr)
        {
            shaped.push_back(unit);
            continue;
        }
        const bool after_joiner = joins_next(neighbour(i, -1));
        if (c == kLam && i + 1 < u.size())
        {
            if (const std::uint32_t ligature = lam_alef(letters[i + 1]); ligature != 0)
            {
                unit.codepoint = after_joiner ? ligature + 1 : ligature;
                unit.end = u[i + 1].end;
                shaped.push_back(unit);
                ++i;
                continue;
            }
        }
        const bool before_joiner = f->initial != 0 && joins_previous(neighbour(i, +1));
        JoinForm form = JoinForm::isolated;
        if (after_joiner && before_joiner)
            form = JoinForm::medial;
        else if (after_joiner)
            form = JoinForm::final_form;
        else if (before_joiner)
            form = JoinForm::initial;
        unit.codepoint = arabic_form(c, form);
        shaped.push_back(unit);
    }
    u = std::move(shaped);
}

} // namespace

bool is_rtl(std::uint32_t c)
{
    return (c >= 0x0590 && c <= 0x08FF) || (c >= 0xFB1D && c <= 0xFDFF) ||
           (c >= 0xFE70 && c <= 0xFEFF) || (c >= 0x10800 && c <= 0x10FFF) ||
           (c >= 0x1E800 && c <= 0x1EFFF) || c == 0x200F;
}

std::uint32_t arabic_form(std::uint32_t letter, JoinForm form)
{
    const Forms *f = forms_of(letter);
    if (f == nullptr)
        return letter;
    std::uint32_t shape = f->isolated;
    if (form == JoinForm::final_form)
        shape = f->final_form;
    else if (form == JoinForm::initial)
        shape = f->initial;
    else if (form == JoinForm::medial)
        shape = f->medial;
    // A right-joining letter asked for its initial or medial form has
    // only the isolated or final one.
    if (shape == 0 && form == JoinForm::medial)
        shape = f->final_form;
    return shape != 0 ? shape : f->isolated;
}

std::string_view visual_line(std::string_view text, std::string *storage)
{
    if (!may_be_rtl(text))
        return text;

    std::vector<Unit> units;
    units.reserve(text.size());
    bool any_rtl = false;
    for (std::size_t index = 0; index < text.size();)
    {
        Unit unit;
        unit.begin = index;
        std::size_t consumed = 0;
        if (emoji::match(text, index, &consumed) != 0)
        {
            index += consumed; // a picture: neutral, and never split
        }
        else
        {
            unit.codepoint = next_codepoint(text, &index);
            any_rtl = any_rtl || is_rtl(unit.codepoint);
        }
        unit.end = index;
        units.push_back(unit);
    }
    if (!any_rtl)
        return text;

    shape(&units);
    const std::size_t n = units.size();
    for (Unit &unit : units)
        unit.type = unit.codepoint == 0 ? Type::ON : classify(unit.codepoint);

    // Paragraph direction (P2, P3): the first strong letter's.
    int base = 0;
    for (const Unit &unit : units)
    {
        if (unit.type == Type::L || unit.type == Type::R)
        {
            base = unit.type == Type::R ? 1 : 0;
            break;
        }
    }
    const Type edge = base == 1 ? Type::R : Type::L; // sos and eos

    // W1: a mark takes the type of what it sits on.
    Type previous = edge;
    for (Unit &unit : units)
    {
        if (unit.type == Type::NSM)
            unit.type = previous;
        previous = unit.type;
    }
    // W4: one separator between two numbers of a kind joins them ("1,000",
    // "3.5"; a plus or minus only between European digits).
    for (std::size_t i = 1; i + 1 < n; ++i)
    {
        const Type before = units[i - 1].type;
        const Type after = units[i + 1].type;
        if (units[i].type == Type::ES && before == Type::EN && after == Type::EN)
            units[i].type = Type::EN;
        else if (units[i].type == Type::CS && before == after &&
                 (before == Type::EN || before == Type::AN))
            units[i].type = before;
    }
    // W5: currency and percent signs next to European digits belong to them.
    for (std::size_t i = 0; i < n;)
    {
        if (units[i].type != Type::ET)
        {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < n && units[end].type == Type::ET)
            ++end;
        const bool by_number =
            (i > 0 && units[i - 1].type == Type::EN) || (end < n && units[end].type == Type::EN);
        if (by_number)
            for (std::size_t k = i; k < end; ++k)
                units[k].type = Type::EN;
        i = end;
    }
    // W6, W7: leftover separators are neutral; European digits after a
    // left-to-right letter read as left to right.
    Type last_strong = edge;
    for (Unit &unit : units)
    {
        if (unit.type == Type::ES || unit.type == Type::ET || unit.type == Type::CS)
            unit.type = Type::ON;
        if (unit.type == Type::L || unit.type == Type::R)
            last_strong = unit.type;
        else if (unit.type == Type::EN && last_strong == Type::L)
            unit.type = Type::L;
    }
    // N1, N2: neutrals between two of a direction take it, others the
    // paragraph's.
    for (std::size_t i = 0; i < n;)
    {
        if (units[i].type != Type::ON)
        {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < n && units[end].type == Type::ON)
            ++end;
        const Type before = i == 0 ? edge : strong_of(units[i - 1].type);
        const Type after = end == n ? edge : strong_of(units[end].type);
        const Type resolved = before == after ? before : edge;
        for (std::size_t k = i; k < end; ++k)
            units[k].type = resolved;
        i = end;
    }
    // I1, I2: embedding levels.
    std::uint8_t highest = 0;
    for (Unit &unit : units)
    {
        if (base == 0)
            unit.level = unit.type == Type::L ? 0 : unit.type == Type::R ? 1 : 2;
        else
            unit.level = unit.type == Type::R ? 1 : 2;
        highest = std::max(highest, unit.level);
    }

    // L2: from the highest level down to 1, reverse every run at or above it.
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i)
        order[i] = i;
    for (int level = highest; level >= 1; --level)
    {
        for (std::size_t i = 0; i < n;)
        {
            if (units[order[i]].level < level)
            {
                ++i;
                continue;
            }
            std::size_t end = i;
            while (end < n && units[order[end]].level >= level)
                ++end;
            std::reverse(order.begin() + static_cast<std::ptrdiff_t>(i),
                         order.begin() + static_cast<std::ptrdiff_t>(end));
            i = end;
        }
    }

    // A reversed mark now comes before its letter, which is where a
    // zero-width mark drawn from the pen lands over it in the Arabic face.
    storage->clear();
    storage->reserve(text.size() + text.size() / 2);
    for (const std::size_t i : order)
    {
        const Unit &unit = units[i];
        if (unit.codepoint == 0)
        {
            storage->append(text.substr(unit.begin, unit.end - unit.begin));
            continue;
        }
        // L4: brackets in right-to-left text face the other way.
        append_utf8(storage, unit.level % 2 == 1 ? mirrored(unit.codepoint) : unit.codepoint);
    }
    return *storage;
}

} // namespace hui::gfx
