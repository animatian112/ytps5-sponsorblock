// youtube-tv - Settings: the codec switches, the resolution cap, haptics and
// sign-out, as one scrolling Form.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "app/context.hpp"
#include "app/screen.hpp"

#include "ui/components.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace hui::app
{

class SettingsScreen final : public Screen
{
  public:
    explicit SettingsScreen(Context &context);

    const char *id() const override
    {
        return "settings";
    }
    const char *title() const override
    {
        return "Settings";
    }
    const char *hint() const override
    {
        return open_ >= 0 ? "Circle back to the tiles   Options quit"
                          : "Cross open   Left: menu   Options quit";
    }
    // Left past the first tile of a row hands the focus to the sidebar.
    bool wants_rail_focus() const override
    {
        return wants_rail_;
    }
    bool uses_back() const override
    {
        return open_ >= 0;
    }
    void enter() override;
    void update(const InputFrame &input, float dt, ui::Feedback &feedback) override;
    void draw(const Context &context, Frame &frame) const override;
    std::span<const TourStep> tour() const override;

    // SmartTube's settings: a grid of tiles, each opening its group of
    // options (or doing one thing, like the network test).
    enum class Icon
    {
        accounts,
        main_ui,
        player,
        subtitles,
        general,
        search,
        sponsorblock,
        remote,
        network,
        updates,
        about,
    };

  private:
    struct Tile
    {
        std::string label;
        Icon icon = Icon::general;
        std::unique_ptr<ui::Form> form; // null: the tile is an action
        int action = 0;
    };

    ui::Form &form()
    {
        return *tiles_[static_cast<std::size_t>(open_)].form;
    }
    ui::Form &add_tile(const char *label, Icon icon);
    // One option bound to a Settings field: a toggle (flag, or a sidebar
    // section's visibility bit) or a stepper over `values`.
    struct Binding
    {
        int id = 0;
        bool Settings::*flag = nullptr;
        int Settings::*number = nullptr;
        std::vector<int> values;
        int section = -1;   // Settings::hidden_sections bit; the toggle reads "shown"
        int *raw = nullptr; // an array element (Settings::sb_act[i])
    };
    void add_raw_steps(ui::Form &form, const char *label, int *value, std::vector<int> values,
                       std::vector<std::string> labels, const char *description = nullptr);
    void add_flag(ui::Form &form, const char *label, bool Settings::*flag,
                  const char *description = nullptr);
    void add_steps(ui::Form &form, const char *label, int Settings::*number,
                   std::vector<int> values, std::vector<std::string> labels,
                   const char *description = nullptr);
    void add_section(ui::Form &form, int section);
    // True when the id was one of the bound options (and is now applied).
    bool apply_binding(int id);
    std::vector<Binding> bindings_;
    int next_binding_id_ = 500;
    // The Accounts page lists the remembered accounts; it is rebuilt each
    // time it opens and when the list changes.
    void rebuild_accounts();
    void refresh_remote();
    std::string accounts_key_;
    void apply(int id, ui::Feedback &feedback);
    void activate(int id);
    void update_grid(const InputFrame &input, ui::Feedback &feedback);
    void draw_icon(gfx::DrawList &list, Icon icon, const gfx::Rect &box) const;

    Context &context_;
    std::vector<Tile> tiles_;
    int focus_ = 0;
    int open_ = -1; // the tile whose options are on screen, -1 for the grid
    bool wants_rail_ = false;
    float clock_ = 0.0f;
    // The "Check for updates" action waits for the background check to settle
    // before it reports the result.
    bool update_watching_ = false;
    std::uint64_t update_watch_revision_ = 0;
};

} // namespace hui::app
