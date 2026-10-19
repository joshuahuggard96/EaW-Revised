#pragma once

// The fork's Remastered Graphics page of the options screen (options_screen.cpp): the dialog,
// built from retail's advanced video dialog so it wears the same skin, its rows and what each
// slider stores. The screen keeps the draft, Accept and Cancel; this file only knows the page.
// Values reach the engine through remastered_settings.hpp.

#include "eawr/data/ui/dialog_script.hpp"
#include "eawr/presentation/ui/settings.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend::options_remastered {

// The page's slider rows, top to bottom.
struct Row final {
    std::string_view id;
    std::string_view caption;
    int minimum;
    int maximum;
    int step;
};
[[nodiscard]] std::span<const Row> rows() noexcept;

inline constexpr std::string_view fps_check = "IDC_REM_FPS";
inline constexpr std::string_view defaults_button = "IDC_REM_DEFAULTS";
inline constexpr std::string_view video_button = "IDC_REMASTERED";

// The id of the label that shows a row's value.
[[nodiscard]] std::string value_label(std::string_view row);

// Rewrites retail's IDD_ADVANCED_VIDEO_OPTIONS_DIALOG into the page: its title, slider row,
// check box and buttons are the templates. False when one of them is missing.
[[nodiscard]] bool build_page(data::ui::Dialog& dialog);
// Adds the Remastered button under the video page's Advanced button.
void add_video_button(data::ui::Dialog& dialog);

// A row's slider value in the draft. The profile row shows `running` while the setting is
// automatic (0 retail, 1 enhanced, 2 remastered).
[[nodiscard]] int value_of(const presentation::ui::PlayerSettings& draft, std::string_view row, int running_profile);
// Stores a slider value; the profile is stored only once the player moved it.
void store(presentation::ui::PlayerSettings& draft, std::string_view row, int value, bool profile_touched);
// What the value label shows.
[[nodiscard]] std::string value_text(std::string_view row, int value);
// Why the row is disabled: the command line set its option. Empty when it is free.
[[nodiscard]] std::optional<std::string> pinned_reason(std::string_view row);

} // namespace eawr::presentation::godot_backend::options_remastered
