// youtube-tv - Tests: the app's languages, their catalog and their dates.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/keyboard_languages.hpp"
#include "core/i18n.hpp"
#include "core/save_file.hpp"
#include "core/settings.hpp"
#include "gfx/font.hpp"
#include "yt/sponsorblock.hpp"

#include <gtest/gtest.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#ifndef HUI_SOURCE_DIR
#define HUI_SOURCE_DIR "."
#endif

namespace
{

using hui::i18n::Language;

// Every test leaves the app in English, as it starts.
class I18n : public ::testing::Test
{
  protected:
    ~I18n() override
    {
        hui::i18n::set_language(Language::english);
    }
};

int count(std::string_view text, std::string_view what)
{
    int found = 0;
    for (std::size_t at = text.find(what); at != std::string_view::npos;
         at = text.find(what, at + what.size()))
        ++found;
    return found;
}

std::size_t leading_spaces(std::string_view text)
{
    return text.find_first_not_of(' ') == std::string_view::npos ? text.size()
                                                                 : text.find_first_not_of(' ');
}

// The phrases the screens pass to tr() and trf(): each run of string
// literals right after the call, with its escapes undone, as the compiler
// joins them.
std::vector<std::string> phrases_in(const std::string &source)
{
    std::vector<std::string> found;
    for (std::size_t at = 0; (at = source.find("tr", at)) != std::string::npos; ++at)
    {
        if (at > 0 && (std::isalnum(static_cast<unsigned char>(source[at - 1])) != 0 ||
                       source[at - 1] == '_' || source[at - 1] == ':'))
        {
            // Part of a longer name, or a qualified call that is not ours.
            if (!(source[at - 1] == ':' && at >= 6 && source.compare(at - 6, 6, "i18n::") == 0))
                continue;
        }
        std::size_t i = at + 2;
        if (i < source.size() && source[i] == 'f')
            ++i;
        if (i >= source.size() || source[i] != '(')
            continue;
        ++i;
        std::string phrase;
        bool any = false;
        while (true)
        {
            while (i < source.size() && std::isspace(static_cast<unsigned char>(source[i])) != 0)
                ++i;
            if (i >= source.size() || source[i] != '"')
                break;
            any = true;
            for (++i; i < source.size() && source[i] != '"'; ++i)
            {
                if (source[i] != '\\')
                {
                    phrase += source[i];
                    continue;
                }
                const char escaped = source[++i];
                if (escaped == 'n')
                    phrase += '\n';
                else if (escaped == 'x')
                {
                    phrase += static_cast<char>(std::stoi(source.substr(i + 1, 2), nullptr, 16));
                    i += 2;
                }
                else
                    phrase += escaped;
            }
            ++i; // the closing quote
        }
        if (any)
            found.push_back(phrase);
    }
    return found;
}

const hui::i18n::Entry *find(std::string_view english)
{
    for (const hui::i18n::Entry &entry : hui::i18n::catalog())
        if (english == entry.english)
            return &entry;
    return nullptr;
}

TEST_F(I18n, EveryLineHasEveryLanguageAndTheEnglishsPlaceholders)
{
    std::set<std::string_view> seen;
    ASSERT_GT(hui::i18n::catalog().size(), 400u);
    for (const hui::i18n::Entry &entry : hui::i18n::catalog())
    {
        const std::string_view english = entry.english;
        EXPECT_TRUE(seen.insert(english).second) << "twice in the catalog: " << english;
        for (const char *translated : entry.translated)
        {
            ASSERT_NE(translated, nullptr) << english;
            const std::string_view text = translated;
            EXPECT_FALSE(text.empty()) << english;
            EXPECT_EQ(count(text, "{}"), count(english, "{}")) << english << " -> " << text;
            EXPECT_EQ(leading_spaces(text), leading_spaces(english)) << english << " -> " << text;
        }
    }
}

TEST_F(I18n, EveryPhraseTheScreensTranslateIsInTheCatalog)
{
    int phrases = 0;
    const std::filesystem::path app = std::filesystem::path(HUI_SOURCE_DIR) / "src" / "app";
    for (const auto &file : std::filesystem::directory_iterator(app))
    {
        if (file.path().extension() != ".cpp")
            continue;
        std::ifstream input(file.path());
        const std::string source((std::istreambuf_iterator<char>(input)),
                                 std::istreambuf_iterator<char>());
        for (const std::string &phrase : phrases_in(source))
        {
            ++phrases;
            EXPECT_NE(find(phrase), nullptr)
                << file.path().filename().string() << ": no catalog line for \"" << phrase << "\"";
        }
    }
    EXPECT_GT(phrases, 400) << "the scan found the screens' phrases";
}

TEST_F(I18n, TheNamesSettingsListsAreInTheCatalog)
{
    using hui::Settings;
    std::vector<std::string_view> names;
    for (const char *name : Settings::kSectionNames)
        names.emplace_back(name);
    for (const char *name : Settings::kBootSections)
        names.emplace_back(name);
    for (const char *name : Settings::kColorSchemes)
        names.emplace_back(name);
    for (const char *name : Settings::kPlayerButtonNames)
        names.emplace_back(name);
    for (const auto &region : Settings::kRegions)
        names.emplace_back(region[2]);
    for (const auto &language : Settings::kSubtitleLanguages)
        names.emplace_back(language[1]);
    for (const hui::app::KeyboardLanguage &language : hui::app::keyboard_languages())
        names.emplace_back(language.name);
    for (int i = 0; i < yt::sponsorblock::kCategoryCount; ++i)
        names.emplace_back(yt::sponsorblock::category_label(i));
    // The rows Home makes of its own (rows_view.cpp shows them translated).
    names.emplace_back("Recommended");
    names.emplace_back("Continue watching");
    for (const std::string_view name : names)
        EXPECT_NE(find(name), nullptr) << "no catalog line for \"" << name << "\"";
}

TEST_F(I18n, TrAnswersInTheLanguageSetAndPassesUnknownPhrasesThrough)
{
    EXPECT_STREQ(hui::tr("Settings"), "Settings") << "English until set";
    hui::i18n::set_language(Language::italian);
    EXPECT_STREQ(hui::tr("Settings"), "Impostazioni");
    hui::i18n::set_language(Language::spanish);
    EXPECT_STREQ(hui::tr("Settings"), "Ajustes");
    hui::i18n::set_language(Language::russian);
    EXPECT_STREQ(hui::tr("Settings"), "Настройки");
    hui::i18n::set_language(Language::chinese);
    EXPECT_STREQ(hui::tr("Settings"), "设置");
    hui::i18n::set_language(Language::japanese);
    EXPECT_STREQ(hui::tr("Settings"), "設定");
    // A video title, or a phrase nobody translated yet, shows as it is.
    const char *title = "Never Gonna Give You Up";
    EXPECT_EQ(hui::tr(title), title);
    EXPECT_EQ(hui::tr(nullptr), nullptr);
}

TEST_F(I18n, TrfPutsTheValuesWhereTheLanguagePutsThem)
{
    EXPECT_EQ(hui::trf("Added to {}", std::string("Mix")), "Added to Mix");
    hui::i18n::set_language(Language::japanese);
    EXPECT_EQ(hui::trf("Added to {}", std::string("Mix")), "Mix に追加しました");
    hui::i18n::set_language(Language::russian);
    EXPECT_EQ(hui::trf("{} views", std::string("1,234")), "Просмотров: 1,234");
    EXPECT_EQ(hui::trf("Version {} (running {})", std::string("1.0.200"), std::string("1.0.199")),
              "Версия 1.0.200 (сейчас 1.0.199)");
}

TEST_F(I18n, FormatNeverReadsPastTheValuesItHas)
{
    using hui::i18n::format;
    EXPECT_EQ(format("{} and {}", {"a", "b"}), "a and b");
    EXPECT_EQ(format("{} and {}", {"a"}), "a and {}");
    EXPECT_EQ(format("{}", {"a", "b"}), "a");
    EXPECT_EQ(format("no values", {}), "no values");
    EXPECT_EQ(format("{ } {", {"a"}), "{ } {");
}

TEST_F(I18n, TheConsolesLanguageDecidesUnlessSettingsPicksOne)
{
    using hui::i18n::from_console;
    using hui::i18n::resolve;
    EXPECT_EQ(from_console(0), Language::japanese);
    EXPECT_EQ(from_console(1), Language::english);
    EXPECT_EQ(from_console(18), Language::english);
    EXPECT_EQ(from_console(3), Language::spanish);
    EXPECT_EQ(from_console(20), Language::spanish) << "Latin American Spanish";
    EXPECT_EQ(from_console(5), Language::italian);
    EXPECT_EQ(from_console(8), Language::russian);
    EXPECT_EQ(from_console(11), Language::chinese);
    EXPECT_EQ(from_console(2), Language::english) << "French: no catalog column yet";
    EXPECT_EQ(from_console(-1), Language::english) << "unreadable";

    EXPECT_EQ(resolve(0, 5), Language::italian) << "0 follows the console";
    EXPECT_EQ(resolve(1, 5), Language::english);
    EXPECT_EQ(resolve(6, 1), Language::japanese);
    EXPECT_EQ(resolve(99, 8), Language::russian) << "out of range follows the console";
    EXPECT_EQ(hui::Settings::kUiLanguageMax, hui::i18n::kLanguageCount);
    EXPECT_STREQ(hui::i18n::info(Language::chinese).native, "简体中文");
    EXPECT_STREQ(hui::i18n::info(Language::spanish).code, "es");
}

TEST(I18nSettings, TheLanguagePickedSurvivesTheSaveFile)
{
    hui::Settings settings;
    EXPECT_EQ(settings.ui_language, 0) << "the console's language by default";
    settings.ui_language = 5; // Simplified Chinese
    hui::Settings loaded;
    ASSERT_TRUE(hui::decode_settings(hui::encode_settings(settings), &loaded));
    EXPECT_EQ(loaded.ui_language, 5);
    EXPECT_EQ(hui::i18n::resolve(loaded.ui_language, 1), Language::chinese);
}

TEST_F(I18n, DatesAndTimesReadAsEachLanguageWritesThem)
{
    using namespace hui::i18n;
    EXPECT_EQ(long_date(2024, 5, 1), "May 1, 2024");
    EXPECT_EQ(short_date(5, 10, 16), "Fri Oct 16");
    EXPECT_EQ(clock_time(22, 34, true), "10:34 PM");
    EXPECT_EQ(clock_time(0, 5, true), "12:05 AM");
    EXPECT_EQ(clock_time(22, 34, false), "22:34");
    EXPECT_EQ(days_ago(0), "today");
    EXPECT_EQ(days_ago(1), "1 day ago");
    EXPECT_EQ(days_ago(14), "2 weeks ago");
    EXPECT_EQ(days_ago(800), "2 years ago");

    set_language(Language::italian);
    EXPECT_EQ(long_date(2024, 5, 1), "1 mag 2024");
    EXPECT_EQ(short_date(5, 10, 16), "ven 16 ott");
    EXPECT_EQ(days_ago(1), "1 giorno fa");
    EXPECT_EQ(days_ago(3), "3 giorni fa");

    set_language(Language::spanish);
    EXPECT_EQ(days_ago(60), "hace 2 meses");
    EXPECT_EQ(days_ago(365), "hace 1 año");

    set_language(Language::russian);
    EXPECT_EQ(long_date(2024, 5, 1), "1 мая 2024 г.");
    EXPECT_EQ(days_ago(1), "1 день назад");
    EXPECT_EQ(days_ago(3), "3 дня назад");
    EXPECT_EQ(days_ago(5), "5 дней назад");
    EXPECT_EQ(days_ago(21), "3 недели назад");
    EXPECT_EQ(days_ago(11 * 365), "11 лет назад") << "11 to 14 take the many form";
    EXPECT_EQ(days_ago(21 * 365), "21 год назад");

    set_language(Language::chinese);
    EXPECT_EQ(long_date(2024, 5, 1), "2024年5月1日");
    EXPECT_EQ(short_date(5, 10, 16), "10月16日 周五");
    EXPECT_EQ(clock_time(22, 34, true), "下午 10:34");
    EXPECT_EQ(days_ago(3), "3天前");

    set_language(Language::japanese);
    EXPECT_EQ(short_date(5, 10, 16), "10月16日(金)");
    EXPECT_EQ(clock_time(9, 5, true), "午前 9:05");
    EXPECT_EQ(days_ago(90), "3か月前");
    EXPECT_EQ(short_date(1, 13, 1), "") << "a month out of range says nothing";
}

TEST_F(I18n, TheBakedFontsDrawEveryTranslatedCharacter)
{
    // The Latin face, with the Chinese, Japanese and Korean face it falls
    // back on, as src/main.cpp loads them.
    std::string latin_data;
    std::string cjk_data;
    ASSERT_TRUE(hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/google-sans-regular.huifont",
                                     &latin_data));
    ASSERT_TRUE(hui::save::read_file(HUI_SOURCE_DIR "/assets/fonts/noto-sans-cjk.huifont",
                                     &cjk_data, 32u << 20));
    hui::gfx::Font latin;
    hui::gfx::Font cjk;
    ASSERT_TRUE(latin.load(latin_data)) << latin.error();
    ASSERT_TRUE(cjk.load(cjk_data)) << cjk.error();
    EXPECT_FALSE(latin.has_glyph(0x8BBE)) << "设 comes from the fallback";
    latin.add_fallback(&cjk, 1);
    EXPECT_TRUE(latin.has_glyph(0x8BBE));

    const auto check = [&](std::string_view text)
    {
        std::size_t i = 0;
        while (i < text.size())
        {
            const std::uint32_t code = hui::gfx::next_codepoint(text, &i);
            EXPECT_TRUE(latin.has_glyph(code))
                << "U+" << std::hex << code << " in \"" << text << "\"";
        }
    };
    for (const hui::i18n::Entry &entry : hui::i18n::catalog())
        for (const char *translated : entry.translated)
            check(translated);
    for (int i = 0; i < hui::i18n::kLanguageCount; ++i)
        check(hui::i18n::info(static_cast<Language>(i)).native);
    for (int language = 0; language < hui::i18n::kLanguageCount; ++language)
    {
        hui::i18n::set_language(static_cast<Language>(language));
        for (int weekday = 0; weekday < 7; ++weekday)
            check(hui::i18n::short_date(weekday, weekday + 1, 1));
        for (int month = 1; month <= 12; ++month)
            check(hui::i18n::long_date(2024, month, 1));
        check(hui::i18n::clock_time(13, 0, true));
        for (const long long days : {0LL, 1LL, 3LL, 8LL, 40LL, 400LL, 2000LL})
            check(hui::i18n::days_ago(days));
    }
    // The keyboards' keys, and what shift types on them.
    for (const hui::ui::KeyboardLayout &layout : hui::app::keyboard_layouts((1 << 5) - 1))
        for (const auto &row : layout.rows)
            for (const auto &key : row)
            {
                check(key.text);
                check(key.shifted);
                check(key.label);
            }
    // And text YouTube sends: Traditional Chinese and Korean titles.
    check("繁體中文標題");
    check("한국어 제목");
}

} // namespace
