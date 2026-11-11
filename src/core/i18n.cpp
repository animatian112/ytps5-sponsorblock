// youtube-tv - The languages the app's own words are shown in.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/i18n.hpp"

#include <atomic>
#include <cstdio>
#include <string_view>
#include <unordered_map>

namespace hui::i18n
{
namespace
{

constexpr LanguageInfo kInfo[kLanguageCount] = {
    {"en", "English"}, {"it", "Italiano"},      {"es", "Español"},
    {"ru", "Русский"}, {"zh-Hans", "简体中文"}, {"ja", "日本語"},
};

std::atomic<int> g_language{0};

// English phrase -> catalog line, built on the first lookup. A function-local
// static, so the first tr() from any thread builds it exactly once.
const std::unordered_map<std::string_view, const Entry *> &index()
{
    static const std::unordered_map<std::string_view, const Entry *> map = []
    {
        std::unordered_map<std::string_view, const Entry *> built;
        for (const Entry &entry : catalog())
            built.emplace(entry.english, &entry);
        return built;
    }();
    return map;
}

bool cjk()
{
    const Language language = current();
    return language == Language::chinese || language == Language::japanese;
}

// Russian picks a noun's form by the number: 1, 21 ... one; 2-4, 22-24 ...
// few; everything else many (5-20, 25-30 ...).
const char *russian_plural(long long n, const char *one, const char *few, const char *many)
{
    const long long tens = n % 100;
    const long long units = n % 10;
    if (tens >= 11 && tens <= 14)
        return many;
    if (units == 1)
        return one;
    if (units >= 2 && units <= 4)
        return few;
    return many;
}

} // namespace

const LanguageInfo &info(Language language)
{
    const int index = static_cast<int>(language);
    return kInfo[index >= 0 && index < kLanguageCount ? index : 0];
}

Language current()
{
    return static_cast<Language>(g_language.load(std::memory_order_relaxed));
}

void set_language(Language language)
{
    const int index = static_cast<int>(language);
    g_language.store(index >= 0 && index < kLanguageCount ? index : 0, std::memory_order_relaxed);
}

Language from_console(int console)
{
    // SceSystemParamLang: 0 Japanese, 1 English (US), 3 Spanish, 5 Italian,
    // 8 Russian, 11 Simplified Chinese, 18 English (UK), 20 Spanish (Latin
    // America). Traditional Chinese (10) stays English: the catalog's Chinese
    // is Simplified, and Settings offers it to whoever wants it.
    switch (console)
    {
    case 0:
        return Language::japanese;
    case 3:
    case 20:
        return Language::spanish;
    case 5:
        return Language::italian;
    case 8:
        return Language::russian;
    case 11:
        return Language::chinese;
    default:
        return Language::english;
    }
}

Language resolve(int setting, int console)
{
    if (setting >= 1 && setting <= kLanguageCount)
        return static_cast<Language>(setting - 1);
    return from_console(console);
}

const char *tr(const char *english)
{
    const Language language = current();
    if (language == Language::english || english == nullptr)
        return english;
    const auto &map = index();
    const auto found = map.find(english);
    if (found == map.end())
        return english;
    const char *translated = found->second->translated[static_cast<int>(language) - 1];
    return translated != nullptr && translated[0] != '\0' ? translated : english;
}

std::string format(std::string_view pattern, std::initializer_list<std::string_view> values)
{
    std::string out;
    out.reserve(pattern.size() + 16);
    const std::string_view *next = values.begin();
    for (std::size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '}' &&
            next != values.end())
        {
            out += *next++;
            ++i;
            continue;
        }
        out += pattern[i];
    }
    return out;
}

std::string clock_time(int hour, int minute, bool twelve_hour)
{
    char text[32];
    if (!twelve_hour)
    {
        std::snprintf(text, sizeof(text), "%02d:%02d", hour, minute);
        return text;
    }
    const int shown = hour % 12 == 0 ? 12 : hour % 12;
    if (cjk())
    {
        // 午前 / 午後 (上午 / 下午) come before the time.
        const bool chinese = current() == Language::chinese;
        const char *half = hour < 12 ? (chinese ? "上午" : "午前") : (chinese ? "下午" : "午後");
        std::snprintf(text, sizeof(text), "%s %d:%02d", half, shown, minute);
        return text;
    }
    std::snprintf(text, sizeof(text), "%d:%02d %s", shown, minute, hour < 12 ? "AM" : "PM");
    return text;
}

std::string short_date(int weekday, int month, int day)
{
    static const char *const kDays[kLanguageCount][7] = {
        {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"},
        {"dom", "lun", "mar", "mer", "gio", "ven", "sab"},
        {"dom", "lun", "mar", "mié", "jue", "vie", "sáb"},
        {"вс", "пн", "вт", "ср", "чт", "пт", "сб"},
        {"周日", "周一", "周二", "周三", "周四", "周五", "周六"},
        {"日", "月", "火", "水", "木", "金", "土"},
    };
    static const char *const kMonths[4][12] = {
        {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
        {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"},
        {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sept", "oct", "nov", "dic"},
        {"янв", "фев", "мар", "апр", "мая", "июн", "июл", "авг", "сен", "окт", "ноя", "дек"},
    };
    if (month < 1 || month > 12)
        return {};
    const int language = static_cast<int>(current());
    const char *name = kDays[language][((weekday % 7) + 7) % 7];
    char text[48];
    switch (current())
    {
    case Language::english:
        std::snprintf(text, sizeof(text), "%s %s %d", name, kMonths[0][month - 1], day);
        break;
    case Language::chinese:
        std::snprintf(text, sizeof(text), "%d月%d日 %s", month, day, name);
        break;
    case Language::japanese:
        std::snprintf(text, sizeof(text), "%d月%d日(%s)", month, day, name);
        break;
    default:
        std::snprintf(text, sizeof(text), "%s %d %s", name, day, kMonths[language][month - 1]);
        break;
    }
    return text;
}

std::string long_date(int year, int month, int day)
{
    // Russian's month names are in the genitive, as a date says them.
    static const char *const kMonths[4][12] = {
        {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"},
        {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"},
        {"ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sept", "oct", "nov", "dic"},
        {"янв.", "февр.", "мар.", "апр.", "мая", "июн.", "июл.", "авг.", "сент.", "окт.", "нояб.",
         "дек."},
    };
    if (month < 1 || month > 12)
        return {};
    char text[48];
    switch (current())
    {
    case Language::english:
        std::snprintf(text, sizeof(text), "%s %d, %d", kMonths[0][month - 1], day, year);
        break;
    case Language::chinese:
    case Language::japanese:
        std::snprintf(text, sizeof(text), "%d年%d月%d日", year, month, day);
        break;
    case Language::russian:
        std::snprintf(text, sizeof(text), "%d %s %d г.", day, kMonths[3][month - 1], year);
        break;
    default:
        std::snprintf(text, sizeof(text), "%d %s %d", day,
                      kMonths[static_cast<int>(current())][month - 1], year);
        break;
    }
    return text;
}

std::string days_ago(long long days)
{
    enum Unit
    {
        kDay,
        kWeek,
        kMonth,
        kYear
    };
    const Language language = current();
    if (days <= 0)
    {
        static const char *const kToday[kLanguageCount] = {"today",   "oggi", "hoy",
                                                           "сегодня", "今天", "今日"};
        return kToday[static_cast<int>(language)];
    }
    Unit unit = kDay;
    long long n = days;
    if (days >= 365)
    {
        unit = kYear;
        n = days / 365;
    }
    else if (days >= 31)
    {
        unit = kMonth;
        n = days / 30 > 0 ? days / 30 : 1;
    }
    else if (days >= 7)
    {
        unit = kWeek;
        n = days / 7;
    }
    const std::string count = std::to_string(n);
    const bool one = n == 1;
    switch (language)
    {
    case Language::italian:
    {
        static const char *const kOne[] = {"giorno", "settimana", "mese", "anno"};
        static const char *const kMany[] = {"giorni", "settimane", "mesi", "anni"};
        return count + " " + (one ? kOne[unit] : kMany[unit]) + " fa";
    }
    case Language::spanish:
    {
        static const char *const kOne[] = {"día", "semana", "mes", "año"};
        static const char *const kMany[] = {"días", "semanas", "meses", "años"};
        return "hace " + count + " " + (one ? kOne[unit] : kMany[unit]);
    }
    case Language::russian:
    {
        static const char *const kForms[][3] = {{"день", "дня", "дней"},
                                                {"неделю", "недели", "недель"},
                                                {"месяц", "месяца", "месяцев"},
                                                {"год", "года", "лет"}};
        return count + " " + russian_plural(n, kForms[unit][0], kForms[unit][1], kForms[unit][2]) +
               " назад";
    }
    case Language::chinese:
    {
        static const char *const kUnits[] = {"天前", "周前", "个月前", "年前"};
        return count + kUnits[unit];
    }
    case Language::japanese:
    {
        static const char *const kUnits[] = {"日前", "週間前", "か月前", "年前"};
        return count + kUnits[unit];
    }
    case Language::english:
    default:
    {
        static const char *const kUnits[] = {"day", "week", "month", "year"};
        return count + " " + kUnits[unit] + (one ? "" : "s") + " ago";
    }
    }
}

} // namespace hui::i18n
