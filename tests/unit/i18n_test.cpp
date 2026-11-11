// youtube-tv - The language tables: lookups, pieces put together, capitals.
// Copyright (C) 2026 SponsorBlock for PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/i18n.hpp"
#include "core/settings.hpp"
#include "gfx/font.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{

namespace i18n = hui::i18n;

class I18n : public ::testing::Test
{
  protected:
    void TearDown() override
    {
        i18n::set_language("en");
    }
};

TEST_F(I18n, EnglishShowsTextAsGiven)
{
    EXPECT_EQ(i18n::set_language("en"), "en");
    EXPECT_EQ(i18n::tr("Settings"), "Settings");
    EXPECT_EQ(std::string(hui::gfx::shown_text("Settings")), "Settings");
}

TEST_F(I18n, UnknownLanguagesFallBackToEnglishOrTheirFamily)
{
    EXPECT_EQ(i18n::set_language("ja"), "en");
    EXPECT_EQ(i18n::set_language("pt"), "pt-BR");
    EXPECT_EQ(i18n::set_language("es-419"), "es");
    EXPECT_EQ(i18n::youtube_code("pt-BR"), "pt");
}

TEST_F(I18n, EveryBuiltInLanguageLoadsAndTranslates)
{
    for (int i = 2; i < hui::Settings::kLanguageCount; ++i)
    {
        const std::string code = hui::Settings::kLanguages[i][0];
        EXPECT_EQ(i18n::set_language(code), code);
        EXPECT_NE(i18n::tr("Settings"), "Settings") << code;
        EXPECT_NE(i18n::tr("Send yt.log to your phone"), "Send yt.log to your phone") << code;
    }
}

TEST_F(I18n, FontsShowTheTranslation)
{
    i18n::set_language("es");
    EXPECT_EQ(std::string(hui::gfx::shown_text("Settings")), "Ajustes");
    EXPECT_EQ(std::string(hui::gfx::shown_text("Nothing in the table")), "Nothing in the table");
}

TEST_F(I18n, PiecesOfARunTimeStringAreTranslated)
{
    i18n::set_language("es");
    EXPECT_EQ(std::string(i18n::shown("Skipped Sponsor")), "Saltado: Sponsor");
    EXPECT_EQ(std::string(i18n::shown("12 videos")), "12 vídeos");
    EXPECT_EQ(std::string(i18n::shown("Stops in 15 minutes")), "Se detiene en 15 minutos");
}

TEST_F(I18n, CapitalLabelsFindTheirTranslation)
{
    i18n::set_language("de");
    EXPECT_EQ(std::string(i18n::shown("SETTINGS")), "EINSTELLUNGEN");
    EXPECT_EQ(i18n::upper("größe"), "GRÖßE");
    i18n::set_language("ru");
    EXPECT_EQ(i18n::upper("ёж"), "ЁЖ");
    i18n::set_language("tr");
    EXPECT_EQ(i18n::upper("ıi"), "Iİ");
}

TEST_F(I18n, VerbatimTextIsNeverTranslated)
{
    i18n::set_language("fr");
    EXPECT_EQ(std::string(i18n::shown(i18n::kVerbatim + "Settings")), i18n::kVerbatim + "Settings");
    const hui::gfx::VerbatimText verbatim;
    EXPECT_EQ(std::string(hui::gfx::shown_text("Settings")), "Settings");
}

TEST_F(I18n, LanguageNamesStayInTheirOwnLanguage)
{
    i18n::set_language("es");
    const std::string english = hui::Settings::kLanguages[1][1];
    EXPECT_EQ(std::string(hui::gfx::shown_text(english)), english);
}

} // namespace
