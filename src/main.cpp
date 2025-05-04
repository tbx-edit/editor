// TODO: list
// for every operation that edits something we need to register a did change event
// add logic for going up and down in the search menu
// add in multiple textbuffers for a viewport
// eventually get to highlighting, later on though check out
// https://tree-sitter.github.io/tree-sitter/3-syntax-highlighting.html
// check out temp/tree_sitter_...

#include <algorithm>
#include <fmt/core.h>
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <nlohmann/detail/input/input_adapters.hpp>
#include <rapidfuzz/fuzz.hpp>

#include "modal_editor/modal_editor.hpp"

#include "graphics/ui/ui.hpp"
#include "graphics/window/window.hpp"
#include "graphics/shader_cache/shader_cache.hpp"
#include "graphics/batcher/generated/batcher.hpp"
#include "graphics/vertex_geometry/vertex_geometry.hpp"
#include "graphics/texture_atlas/texture_atlas.hpp"
#include "graphics/viewport/viewport.hpp"
#include "graphics/colors/colors.hpp"

#include "utility/fs_utils/fs_utils.hpp"
#include "utility/input_state/input_state.hpp"
#include "utility/glfw_lambda_callback_manager/glfw_lambda_callback_manager.hpp"
#include "utility/limited_vector/limited_vector.hpp"
#include "utility/temporal_binary_signal/temporal_binary_signal.hpp"
#include "utility/text_buffer/text_buffer.hpp"
#include "utility/input_state/input_state.hpp"
#include "utility/periodic_signal/periodic_signal.hpp"
#include "utility/config_file_parser/config_file_parser.hpp"
#include "utility/lsp_client/lsp_client.hpp"
#include "utility/resource_path/resource_path.hpp"
#include "utility/text_diff/text_diff.hpp"

#include <cstdio>
#include <cstdlib>

#include <iostream>
#include <fstream>
#include <regex>
#include <vector>
#include <string>
#include <stdexcept>

#include <iostream>
#include <filesystem>
#include <cstdio>

Colors colors;
/**
 * Gets the directory of the executable.
 *
 * @return A string representing the directory where the executable is located.
 */
std::string get_executable_path(char **argv) {
    auto dir = std::filesystem::weakly_canonical(std::filesystem::path(argv[0])).parent_path();
    return dir.string(); // Returns the directory as a string
}

std::string extract_filename(const std::string &full_path) {
    std::filesystem::path path(full_path);
    return path.filename().string();
}

std::string get_current_time_string() {
    // Get current time as a time_t object
    auto now = std::chrono::system_clock::now();
    std::time_t now_time_t = std::chrono::system_clock::to_time_t(now);

    // Convert to local time
    std::tm local_tm = *std::localtime(&now_time_t);

    // Format time to string
    std::ostringstream time_stream;
    time_stream << std::put_time(&local_tm, "%Y-%m-%d %H:%M:%S"); // Custom format
    return time_stream.str();
}

std::string remove_file_prefix(const std::string &file_uri) {
#ifdef _WIN32
    // On Windows, remove "file:///" prefix if present
    return (file_uri.rfind("file:///", 0) == 0) ? file_uri.substr(8) : file_uri;
#else
    // On other platforms, remove "file://" prefix if present
    return (file_uri.rfind("file://", 0) == 0) ? file_uri.substr(7) : file_uri;
#endif
}

bool starts_with_any_prefix(const std::string &str, const std::vector<std::string> &prefixes) {
    for (const auto &prefix : prefixes) {
        if (str.rfind(prefix, 0) == 0) { // Check if 'prefix' is at position 0
            return true;
        }
    }
    return false;
}

// todo fill in the json file for the font we created, then create a shader for absolute position and textures
// then use the texture atlas in conjuction with temp/editor code to render the right characters in the right place
// by re-writing the render function, where instead it will go to the batcher and do a queue draw where we replicate
// the text coords a bunch, no need for the texture packer, but intead just load in the correct image and also
// i thinkt hat we might have correct the stock behavior of the texture atlas which flips the image, use renderdoc for
// that.

void adjust_uv_coordinates_in_place(std::vector<glm::vec2> &uv_coords, float horizontal_push, float top_push,
                                    float bottom_push) {
    // Ensure the vector has exactly 4 elements
    if (uv_coords.size() != 4) {
        throw std::invalid_argument("UV coordinates vector must contain exactly 4 elements.");
    }

    // Push in the UV coordinates with separate adjustments for top and bottom
    uv_coords[0].x -= horizontal_push; // Top-right
    uv_coords[0].y += top_push;

    uv_coords[1].x -= horizontal_push; // Bottom-right
    uv_coords[1].y -= bottom_push;

    uv_coords[2].x += horizontal_push; // Bottom-left
    uv_coords[2].y -= bottom_push;

    uv_coords[3].x += horizontal_push; // Top-left
    uv_coords[3].y += top_push;
}

std::string get_mode_string(EditorMode current_mode) {
    switch (current_mode) {
    case MOVE_AND_EDIT:
        return "MOVE_AND_EDIT";
        break;
    case INSERT:
        return "INSERT";
        break;
    case VISUAL_SELECT:
        return "VISUAL_SELECT";
        break;
    case COMMAND:
        return "COMMAND";
        break;
    }
    return "";
}

void render(Viewport &viewport, vertex_geometry::Grid &screen_grid, vertex_geometry::Grid &status_bar_grid,
            vertex_geometry::Grid &command_bar_grid, std::string &command_bar_input,
            TextureAtlas &monospaced_font_atlas, Batcher &batcher, TemporalBinarySignal &command_bar_input_signal,
            int center_idx_x, int center_idx_y, int num_cols, int num_lines, int col_where_selection_mode_started,
            int line_where_selection_mode_started, ShaderCache &shader_cache,
            std::unordered_map<EditorMode, glm::vec4> &mode_to_cursor_color, double delta_time,
            PeriodicSignal &one_second_signal_for_status_bar_time_update, ModalEditor &modal_editor) {

    bool should_replace = viewport.moved_signal.has_just_changed() or viewport.buffer->edit_signal.has_just_changed();
    auto changed_cells = viewport.get_changed_cells_since_last_tick();

    // FILE BUFFER RENDER
    int unique_idx = 0;
    for (int line = 0; line < screen_grid.rows; line++) {
        for (int col = 0; col < screen_grid.cols; col++) {
            auto cell_rect = screen_grid.get_at(col, line);
            vertex_geometry::IndexedVertices cell_ivs = cell_rect.get_ivs();
            std::string cell_char(1, viewport.get_symbol_at(line, col));
            auto cell_char_tcs = monospaced_font_atlas.get_texture_coordinates_of_sub_texture(cell_char);
            // because the texture has the font inside the cell.
            adjust_uv_coordinates_in_place(cell_char_tcs, 0.017, 0.045, 0.01);

            /*if (viewport.has_cell_changed(line, col)) {*/
            /*    batcher.absolute_position_with_solid_color_shader_batcher.queue_draw(unique_idx,
             * cell_ivs.indices,*/
            /*                                                                         cell_ivs.vertices, true);*/
            /*}*/

            batcher.absolute_position_textured_shader_batcher.queue_draw(
                unique_idx, cell_ivs.indices, cell_ivs.vertices, cell_char_tcs, viewport.has_cell_changed(line, col));
            unique_idx++;
        }
    }

    // STATUS BAR
    std::string mode_string =
        get_mode_string(modal_editor.current_mode) + " | " + extract_filename(viewport.buffer->current_file_path) +
        (viewport.buffer->modified_without_save ? "[+]" : "") + " | " + get_current_time_string() + " |";

    bool should_update_status_bar = one_second_signal_for_status_bar_time_update.process_and_get_signal();

    for (int line = 0; line < status_bar_grid.rows; line++) {
        for (int col = 0; col < status_bar_grid.cols; col++) {

            auto cell_rect = status_bar_grid.get_at(col, line);
            vertex_geometry::IndexedVertices cell_ivs = cell_rect.get_ivs();

            std::string cell_char;
            if (col < mode_string.size()) {
                cell_char = std::string(1, mode_string[col]);
            } else {
                cell_char = "-";
            }

            auto cell_char_tcs = monospaced_font_atlas.get_texture_coordinates_of_sub_texture(cell_char);

            adjust_uv_coordinates_in_place(cell_char_tcs, 0.017, 0.045, 0.01);

            batcher.absolute_position_textured_shader_batcher.queue_draw(
                unique_idx, cell_ivs.indices, cell_ivs.vertices, cell_char_tcs, should_update_status_bar);
            unique_idx++;
        }
    }

    // command bar
    for (int line = 0; line < command_bar_grid.rows; line++) {
        for (int col = 0; col < command_bar_grid.cols; col++) {

            auto cell_rect = command_bar_grid.get_at(col, line);
            vertex_geometry::IndexedVertices cell_ivs = cell_rect.get_ivs();

            std::string cell_char;
            if (col < command_bar_input.size()) {
                cell_char = std::string(1, command_bar_input[col]);
            } else {
                cell_char = " ";
            }

            auto cell_char_tcs = monospaced_font_atlas.get_texture_coordinates_of_sub_texture(cell_char);

            adjust_uv_coordinates_in_place(cell_char_tcs, 0.017, 0.045, 0.01);

            batcher.absolute_position_textured_shader_batcher.queue_draw(unique_idx, cell_ivs.indices,
                                                                         cell_ivs.vertices, cell_char_tcs,
                                                                         command_bar_input_signal.has_just_changed());
            unique_idx++;
        }
    }

    if (modal_editor.mode_change_signal.has_just_changed()) {
        auto selected_color = mode_to_cursor_color[modal_editor.current_mode];
        shader_cache.set_uniform(ShaderType::ABSOLUTE_POSITION_WITH_SOLID_COLOR, ShaderUniformVariable::RGBA_COLOR,
                                 selected_color);
    }

    if (modal_editor.current_mode == VISUAL_SELECT) {
        int visual_col_delta = -(viewport.active_buffer_col_under_cursor - col_where_selection_mode_started);
        int visual_line_delta = -(viewport.active_buffer_line_under_cursor - line_where_selection_mode_started);

        // Clamp the delta values to ensure they stay within the bounds of the grid
        int clamped_visual_line_delta = std::clamp(center_idx_y + visual_line_delta, 0, num_lines - 1);
        int clamped_visual_col_delta = std::clamp(center_idx_x + visual_col_delta, 0, num_cols - 1);

        // Clamp the starting coordinates to ensure they stay within the bounds of the grid
        int clamped_center_idx_y = std::clamp(center_idx_y, 0, num_lines - 1);
        int clamped_center_idx_x = std::clamp(center_idx_x, 0, num_cols - 1);

        // Call the function with the clamped values
        std::vector<vertex_geometry::Rectangle> visually_selected_rectangles =
            screen_grid.get_rectangles_in_bounding_box(clamped_visual_line_delta, clamped_visual_col_delta,
                                                       clamped_center_idx_y, clamped_center_idx_x);

        int obj_id = 1;
        for (auto &rect : visually_selected_rectangles) {
            auto rect_ivs = rect.get_ivs();
            batcher.absolute_position_with_solid_color_shader_batcher.queue_draw(obj_id, rect_ivs.indices,
                                                                                 rect_ivs.vertices, should_replace);
            obj_id++;
        }
    } else { // regular render the cursor in the middle
        auto center_rect = screen_grid.get_at(center_idx_x, center_idx_y);
        auto center_ivs = center_rect.get_ivs();
        batcher.absolute_position_with_solid_color_shader_batcher.queue_draw(0, center_ivs.indices,
                                                                             center_ivs.vertices);
    }

    monospaced_font_atlas.bind_texture();
    batcher.absolute_position_textured_shader_batcher.draw_everything();
    batcher.absolute_position_with_solid_color_shader_batcher.draw_everything();
}

void setup_sdf_shader_uniforms(ShaderCache &shader_cache) {
    auto text_color = glm::vec3(0.5, 0.5, 1);
    float char_width = 0.5;
    float edge_transition = 0.1;

    shader_cache.use_shader_program(ShaderType::TRANSFORM_V_WITH_SIGNED_DISTANCE_FIELD_TEXT);

    shader_cache.set_uniform(ShaderType::TRANSFORM_V_WITH_SIGNED_DISTANCE_FIELD_TEXT, ShaderUniformVariable::TRANSFORM,
                             glm::mat4(1.0f));

    shader_cache.set_uniform(ShaderType::TRANSFORM_V_WITH_SIGNED_DISTANCE_FIELD_TEXT, ShaderUniformVariable::RGB_COLOR,
                             text_color);

    shader_cache.set_uniform(ShaderType::TRANSFORM_V_WITH_SIGNED_DISTANCE_FIELD_TEXT,
                             ShaderUniformVariable::CHARACTER_WIDTH, char_width);

    shader_cache.set_uniform(ShaderType::TRANSFORM_V_WITH_SIGNED_DISTANCE_FIELD_TEXT,
                             ShaderUniformVariable::EDGE_TRANSITION_WIDTH, edge_transition);
    shader_cache.stop_using_shader_program();
}

template <typename Iterable>
std::vector<std::pair<std::string, double>> find_matching_files(const std::string &query, const Iterable &files,
                                                                size_t result_limit, double filename_weight = 0.7) {
    std::vector<std::pair<std::string, double>> results;

    rapidfuzz::fuzz::CachedRatio<char> scorer(query);
    spdlog::debug("Starting file matching with query: '{}' and result limit: {}", query, result_limit);

    for (const auto &file : files) {
        std::string file_path = file.string();
        std::string filename = std::filesystem::path(file_path).filename().string();

        // Calculate similarity scores for both the full path and the filename
        double path_score = scorer.similarity(file_path);
        double filename_score = scorer.similarity(filename);

        // Weighted combination of both scores
        double combined_score = (1.0 - filename_weight) * path_score + filename_weight * filename_score;

        spdlog::debug("File: '{}', Path Score: {:.2f}, Filename Score: {:.2f}, Combined Score: {:.2f}", file_path,
                      path_score, filename_score, combined_score);

        results.emplace_back(file_path, combined_score);
    }

    std::sort(results.begin(), results.end(), [](const auto &a, const auto &b) { return a.second > b.second; });

    spdlog::debug("Sorting completed. Total files evaluated: {}", results.size());

    if (results.size() > result_limit) {
        results.resize(result_limit);
        spdlog::debug("Trimmed results to the top {} files.", result_limit);
    }

    return results;
}

void update_graphical_search_results(std::string &fs_browser_search_query,
                                     std::vector<std::filesystem::path> &searchable_files, FileBrowser &fb,
                                     std::vector<int> &doids_for_textboxes_for_active_directory_for_later_removal,
                                     UI &fs_browser, TemporalBinarySignal &search_results_changed_signal,
                                     int &selected_file_doid, std::vector<std::string> &currently_matched_files) {
    // Find matching files
    int file_limit = 10;
    std::vector<std::pair<std::string, double>> matching_files =
        find_matching_files(fs_browser_search_query, searchable_files, file_limit);

    // here we update the list in the UI, but the thing is the ui is screwed up rn so how to do?
    // Display results
    if (matching_files.empty()) {
        std::cout << "No matching files found for query: \"" << fs_browser_search_query << "\"." << std::endl;
    } else {
        vertex_geometry::Grid file_rows(matching_files.size(), 1, fb.main_file_view_rect);
        auto file_rects = file_rows.get_column(0);

        // clear out old data
        for (auto doid : doids_for_textboxes_for_active_directory_for_later_removal) {
            fs_browser.remove_textbox(doid);
        }
        doids_for_textboxes_for_active_directory_for_later_removal.clear();

        // TODO this is a bad way to do things, use the replace function later on
        fs_browser.remove_textbox(selected_file_doid);
        selected_file_doid = fs_browser.add_textbox(fs_browser_search_query, fb.file_selection_bar, colors.gray40);

        // clear out old data

        // note during this process we delete the old ones and load in the new, so no need to replace.
        // TODO we need to ad da function to remove data from the batcher to make this "complete"
        int i = 0;
        std::cout << "Matching Files (Sorted by Similarity):" << std::endl;

        // clear out old results

        currently_matched_files.clear();
        for (const auto &[file, score] : matching_files) {
            std::cout << file << " (Score: " << score << ")" << std::endl;
            int oid = fs_browser.add_textbox(file, file_rects.at(i), colors.grey);
            doids_for_textboxes_for_active_directory_for_later_removal.push_back(oid);
            currently_matched_files.push_back(file);
            i++;
        }
        search_results_changed_signal.toggle_state();
    }
}

void go_to_definition(JSON lsp_response, Viewport &viewport, LSPClient &lsp_client) {
    try {
        if (!lsp_response.contains("result") || lsp_response["result"].empty()) {
            std::cerr << "LSP go_to_definition: No result found in response." << std::endl;
            return;
        }

        JSON location = lsp_response["result"].is_array() ? lsp_response["result"][0] : lsp_response["result"];

        if (!location.contains("uri") || !location.contains("range")) {
            std::cerr << "LSP go_to_definition: Missing uri or range in response." << std::endl;
            return;
        }

        std::string file_uri = location["uri"].get<std::string>();

        std::string file_to_open = remove_file_prefix(file_uri);

        int line = location["range"]["start"]["line"].get<int>();
        int col = location["range"]["start"]["character"].get<int>();

        // switch_files(viewport, file_to_open, true);
        viewport.set_active_buffer_line_col_under_cursor(line, col);
    } catch (const std::exception &e) {
        std::cerr << "LSP go_to_definition: Exception parsing response: " << e.what() << std::endl;
    }
}

// this is disabled because I don't want to do lsp just yet
void go_to_definition_dummy() {
    // if (modal_editor.current_mode == MOVE_AND_EDIT) {
    //     auto on_definition_found = [&](JSON lsp_response) {
    //         // NOTE: that lsp_response needs to be captured by value because it will go out of scope after this
    //         // function is called
    //         auto callback = [lsp_response, &viewport, &lsp_client]() {
    //             go_to_definition(lsp_response, viewport, lsp_client);
    //         };
    //         lsp_callbacks_to_run_synchronously.push_back(callback);
    //     };
    //
    //     lsp_client.make_go_to_definition_request(viewport.buffer->current_file_path,
    //                                              viewport.active_buffer_line_under_cursor,
    //                                              viewport.active_buffer_col_under_cursor, on_definition_found);
    //
    //     auto tr =
    //         TextRange(viewport.active_buffer_line_under_cursor, 0, viewport.active_buffer_line_under_cursor,
    //                   viewport.buffer->get_line(viewport.active_buffer_line_under_cursor).length());
    //
    //     /*lsp_client.make_get_text_request(viewport.buffer->current_file_path, tr);*/
    //     }
}

// code to make sure the cursor stays within the lines
void snap_to_end_of_line_while_navigating(Viewport &viewport, int &saved, int &saved_last_col, int &saved_last_line) {

    const std::string &line = viewport.buffer->get_line(viewport.active_buffer_line_under_cursor);

    if (line.size() < saved_last_col) {
        if (saved == 0) {
            saved_last_col = viewport.active_buffer_col_under_cursor;
            saved = 1;
            viewport.set_active_buffer_line_col_under_cursor(viewport.active_buffer_line_under_cursor, line.size());
        }
    }
    if (line.size() >= saved_last_col) {
        if (saved == 1) {
            viewport.set_active_buffer_line_col_under_cursor(viewport.active_buffer_line_under_cursor, saved_last_col);
        }
        saved_last_col = viewport.active_buffer_col_under_cursor;
        saved = 0;
    }

    if (saved_last_line != viewport.active_buffer_line_under_cursor && saved == 1) {
        viewport.set_active_buffer_line_col_under_cursor(viewport.active_buffer_line_under_cursor, line.size());
        saved_last_line = viewport.active_buffer_line_under_cursor;
    }

    if (line.size() != viewport.active_buffer_col_under_cursor) {
        saved = 0;
        saved_last_col = viewport.active_buffer_col_under_cursor;
    }
}

bool is_integer(const std::string &str) {
    // create a stringstream from the input string
    std::stringstream ss(str);
    // declare a temporary integer variable to hold the value
    int temp;
    // try to extract an integer from the stringstream and ensure the whole string was processed
    return (ss >> temp && ss.eof());
}

InputKeyState create_input_key_state(InputState &input_state) {
    InputKeyState iks;

    iks.input_key_to_is_pressed[InputKey::a] = input_state.is_pressed(EKey::a);
    iks.input_key_to_is_pressed[InputKey::b] = input_state.is_pressed(EKey::b);
    iks.input_key_to_is_pressed[InputKey::c] = input_state.is_pressed(EKey::c);
    iks.input_key_to_is_pressed[InputKey::d] = input_state.is_pressed(EKey::d);
    iks.input_key_to_is_pressed[InputKey::e] = input_state.is_pressed(EKey::e);
    iks.input_key_to_is_pressed[InputKey::f] = input_state.is_pressed(EKey::f);
    iks.input_key_to_is_pressed[InputKey::g] = input_state.is_pressed(EKey::g);
    iks.input_key_to_is_pressed[InputKey::h] = input_state.is_pressed(EKey::h);
    iks.input_key_to_is_pressed[InputKey::i] = input_state.is_pressed(EKey::i);
    iks.input_key_to_is_pressed[InputKey::j] = input_state.is_pressed(EKey::j);
    iks.input_key_to_is_pressed[InputKey::k] = input_state.is_pressed(EKey::k);
    iks.input_key_to_is_pressed[InputKey::l] = input_state.is_pressed(EKey::l);
    iks.input_key_to_is_pressed[InputKey::m] = input_state.is_pressed(EKey::m);
    iks.input_key_to_is_pressed[InputKey::n] = input_state.is_pressed(EKey::n);
    iks.input_key_to_is_pressed[InputKey::o] = input_state.is_pressed(EKey::o);
    iks.input_key_to_is_pressed[InputKey::p] = input_state.is_pressed(EKey::p);
    iks.input_key_to_is_pressed[InputKey::q] = input_state.is_pressed(EKey::q);
    iks.input_key_to_is_pressed[InputKey::r] = input_state.is_pressed(EKey::r);
    iks.input_key_to_is_pressed[InputKey::s] = input_state.is_pressed(EKey::s);
    iks.input_key_to_is_pressed[InputKey::t] = input_state.is_pressed(EKey::t);
    iks.input_key_to_is_pressed[InputKey::u] = input_state.is_pressed(EKey::u);
    iks.input_key_to_is_pressed[InputKey::v] = input_state.is_pressed(EKey::v);
    iks.input_key_to_is_pressed[InputKey::w] = input_state.is_pressed(EKey::w);
    iks.input_key_to_is_pressed[InputKey::x] = input_state.is_pressed(EKey::x);
    iks.input_key_to_is_pressed[InputKey::y] = input_state.is_pressed(EKey::y);
    iks.input_key_to_is_pressed[InputKey::z] = input_state.is_pressed(EKey::z);

    iks.input_key_to_is_pressed[InputKey::SPACE] = input_state.is_pressed(EKey::SPACE);
    iks.input_key_to_is_pressed[InputKey::GRAVE_ACCENT] = input_state.is_pressed(EKey::GRAVE_ACCENT);
    iks.input_key_to_is_pressed[InputKey::TILDE] = input_state.is_pressed(EKey::TILDE);

    iks.input_key_to_is_pressed[InputKey::ONE] = input_state.is_pressed(EKey::ONE);
    iks.input_key_to_is_pressed[InputKey::TWO] = input_state.is_pressed(EKey::TWO);
    iks.input_key_to_is_pressed[InputKey::THREE] = input_state.is_pressed(EKey::THREE);
    iks.input_key_to_is_pressed[InputKey::FOUR] = input_state.is_pressed(EKey::FOUR);
    iks.input_key_to_is_pressed[InputKey::FIVE] = input_state.is_pressed(EKey::FIVE);
    iks.input_key_to_is_pressed[InputKey::SIX] = input_state.is_pressed(EKey::SIX);
    iks.input_key_to_is_pressed[InputKey::SEVEN] = input_state.is_pressed(EKey::SEVEN);
    iks.input_key_to_is_pressed[InputKey::EIGHT] = input_state.is_pressed(EKey::EIGHT);
    iks.input_key_to_is_pressed[InputKey::NINE] = input_state.is_pressed(EKey::NINE);
    iks.input_key_to_is_pressed[InputKey::ZERO] = input_state.is_pressed(EKey::ZERO);
    iks.input_key_to_is_pressed[InputKey::MINUS] = input_state.is_pressed(EKey::MINUS);
    iks.input_key_to_is_pressed[InputKey::EQUAL] = input_state.is_pressed(EKey::EQUAL);

    iks.input_key_to_is_pressed[InputKey::EXCLAMATION_POINT] = input_state.is_pressed(EKey::EXCLAMATION_POINT);
    iks.input_key_to_is_pressed[InputKey::AT_SIGN] = input_state.is_pressed(EKey::AT_SIGN);
    iks.input_key_to_is_pressed[InputKey::NUMBER_SIGN] = input_state.is_pressed(EKey::NUMBER_SIGN);
    iks.input_key_to_is_pressed[InputKey::DOLLAR_SIGN] = input_state.is_pressed(EKey::DOLLAR_SIGN);
    iks.input_key_to_is_pressed[InputKey::PERCENT_SIGN] = input_state.is_pressed(EKey::PERCENT_SIGN);
    iks.input_key_to_is_pressed[InputKey::CARET] = input_state.is_pressed(EKey::CARET);
    iks.input_key_to_is_pressed[InputKey::AMPERSAND] = input_state.is_pressed(EKey::AMPERSAND);
    iks.input_key_to_is_pressed[InputKey::ASTERISK] = input_state.is_pressed(EKey::ASTERISK);
    iks.input_key_to_is_pressed[InputKey::LEFT_PARENTHESIS] = input_state.is_pressed(EKey::LEFT_PARENTHESIS);
    iks.input_key_to_is_pressed[InputKey::RIGHT_PARENTHESIS] = input_state.is_pressed(EKey::RIGHT_PARENTHESIS);
    iks.input_key_to_is_pressed[InputKey::UNDERSCORE] = input_state.is_pressed(EKey::UNDERSCORE);
    iks.input_key_to_is_pressed[InputKey::PLUS] = input_state.is_pressed(EKey::PLUS);

    iks.input_key_to_is_pressed[InputKey::LEFT_SQUARE_BRACKET] = input_state.is_pressed(EKey::LEFT_SQUARE_BRACKET);
    iks.input_key_to_is_pressed[InputKey::RIGHT_SQUARE_BRACKET] = input_state.is_pressed(EKey::RIGHT_SQUARE_BRACKET);
    iks.input_key_to_is_pressed[InputKey::LEFT_CURLY_BRACKET] = input_state.is_pressed(EKey::LEFT_CURLY_BRACKET);
    iks.input_key_to_is_pressed[InputKey::RIGHT_CURLY_BRACKET] = input_state.is_pressed(EKey::RIGHT_CURLY_BRACKET);

    iks.input_key_to_is_pressed[InputKey::COMMA] = input_state.is_pressed(EKey::COMMA);
    iks.input_key_to_is_pressed[InputKey::PERIOD] = input_state.is_pressed(EKey::PERIOD);
    iks.input_key_to_is_pressed[InputKey::LESS_THAN] = input_state.is_pressed(EKey::LESS_THAN);
    iks.input_key_to_is_pressed[InputKey::GREATER_THAN] = input_state.is_pressed(EKey::GREATER_THAN);

    iks.input_key_to_is_pressed[InputKey::CAPS_LOCK] = input_state.is_pressed(EKey::CAPS_LOCK);
    iks.input_key_to_is_pressed[InputKey::ESCAPE] = input_state.is_pressed(EKey::ESCAPE);
    iks.input_key_to_is_pressed[InputKey::ENTER] = input_state.is_pressed(EKey::ENTER);
    iks.input_key_to_is_pressed[InputKey::TAB] = input_state.is_pressed(EKey::TAB);
    iks.input_key_to_is_pressed[InputKey::BACKSPACE] = input_state.is_pressed(EKey::BACKSPACE);
    iks.input_key_to_is_pressed[InputKey::INSERT] = input_state.is_pressed(EKey::INSERT);
    iks.input_key_to_is_pressed[InputKey::DELETE] = input_state.is_pressed(EKey::DELETE);

    iks.input_key_to_is_pressed[InputKey::RIGHT] = input_state.is_pressed(EKey::RIGHT);
    iks.input_key_to_is_pressed[InputKey::LEFT] = input_state.is_pressed(EKey::LEFT);
    iks.input_key_to_is_pressed[InputKey::UP] = input_state.is_pressed(EKey::UP);
    iks.input_key_to_is_pressed[InputKey::DOWN] = input_state.is_pressed(EKey::DOWN);

    iks.input_key_to_is_pressed[InputKey::SLASH] = input_state.is_pressed(EKey::SLASH);
    iks.input_key_to_is_pressed[InputKey::QUESTION_MARK] = input_state.is_pressed(EKey::QUESTION_MARK);
    iks.input_key_to_is_pressed[InputKey::BACKSLASH] = input_state.is_pressed(EKey::BACKSLASH);
    iks.input_key_to_is_pressed[InputKey::PIPE] = input_state.is_pressed(EKey::PIPE);
    iks.input_key_to_is_pressed[InputKey::COLON] = input_state.is_pressed(EKey::COLON);
    iks.input_key_to_is_pressed[InputKey::SEMICOLON] = input_state.is_pressed(EKey::SEMICOLON);
    iks.input_key_to_is_pressed[InputKey::SINGLE_QUOTE] = input_state.is_pressed(EKey::SINGLE_QUOTE);
    iks.input_key_to_is_pressed[InputKey::DOUBLE_QUOTE] = input_state.is_pressed(EKey::DOUBLE_QUOTE);

    iks.input_key_to_is_pressed[InputKey::LEFT_SHIFT] = input_state.is_pressed(EKey::LEFT_SHIFT);
    iks.input_key_to_is_pressed[InputKey::RIGHT_SHIFT] = input_state.is_pressed(EKey::RIGHT_SHIFT);
    iks.input_key_to_is_pressed[InputKey::LEFT_CONTROL] = input_state.is_pressed(EKey::LEFT_CONTROL);
    iks.input_key_to_is_pressed[InputKey::RIGHT_CONTROL] = input_state.is_pressed(EKey::RIGHT_CONTROL);
    iks.input_key_to_is_pressed[InputKey::LEFT_ALT] = input_state.is_pressed(EKey::LEFT_ALT);
    iks.input_key_to_is_pressed[InputKey::RIGHT_ALT] = input_state.is_pressed(EKey::RIGHT_ALT);
    iks.input_key_to_is_pressed[InputKey::LEFT_SUPER] = input_state.is_pressed(EKey::LEFT_SUPER);
    iks.input_key_to_is_pressed[InputKey::RIGHT_SUPER] = input_state.is_pressed(EKey::RIGHT_SUPER);

    iks.input_key_to_is_pressed[InputKey::FUNCTION_KEY] = input_state.is_pressed(EKey::FUNCTION_KEY);
    iks.input_key_to_is_pressed[InputKey::MENU_KEY] = input_state.is_pressed(EKey::MENU_KEY);

    iks.input_key_to_is_pressed[InputKey::LEFT_MOUSE_BUTTON] = input_state.is_pressed(EKey::LEFT_MOUSE_BUTTON);
    iks.input_key_to_is_pressed[InputKey::RIGHT_MOUSE_BUTTON] = input_state.is_pressed(EKey::RIGHT_MOUSE_BUTTON);
    iks.input_key_to_is_pressed[InputKey::MIDDLE_MOUSE_BUTTON] = input_state.is_pressed(EKey::MIDDLE_MOUSE_BUTTON);
    iks.input_key_to_is_pressed[InputKey::SCROLL_UP] = input_state.is_pressed(EKey::SCROLL_UP);
    // NOTE: the following one is causing out of range, instead of bug fixing this, I'm just not going to because we're
    // probably not going to use scroll for a while and fix the bug later
    // iks.input_key_to_is_pressed[InputKey::SCROLL_DOWN] = input_state.is_pressed(EKey::SCROLL_DOWN);

    // not required
    // iks.input_key_to_is_pressed[InputKey::DUMMY] = input_state.is_pressed(EKey::DUMMY);

    iks.input_key_to_just_pressed[InputKey::a] = input_state.is_just_pressed(EKey::a);
    iks.input_key_to_just_pressed[InputKey::b] = input_state.is_just_pressed(EKey::b);
    iks.input_key_to_just_pressed[InputKey::c] = input_state.is_just_pressed(EKey::c);
    iks.input_key_to_just_pressed[InputKey::d] = input_state.is_just_pressed(EKey::d);
    iks.input_key_to_just_pressed[InputKey::e] = input_state.is_just_pressed(EKey::e);
    iks.input_key_to_just_pressed[InputKey::f] = input_state.is_just_pressed(EKey::f);
    iks.input_key_to_just_pressed[InputKey::g] = input_state.is_just_pressed(EKey::g);
    iks.input_key_to_just_pressed[InputKey::h] = input_state.is_just_pressed(EKey::h);
    iks.input_key_to_just_pressed[InputKey::i] = input_state.is_just_pressed(EKey::i);
    iks.input_key_to_just_pressed[InputKey::j] = input_state.is_just_pressed(EKey::j);
    iks.input_key_to_just_pressed[InputKey::k] = input_state.is_just_pressed(EKey::k);
    iks.input_key_to_just_pressed[InputKey::l] = input_state.is_just_pressed(EKey::l);
    iks.input_key_to_just_pressed[InputKey::m] = input_state.is_just_pressed(EKey::m);
    iks.input_key_to_just_pressed[InputKey::n] = input_state.is_just_pressed(EKey::n);
    iks.input_key_to_just_pressed[InputKey::o] = input_state.is_just_pressed(EKey::o);
    iks.input_key_to_just_pressed[InputKey::p] = input_state.is_just_pressed(EKey::p);
    iks.input_key_to_just_pressed[InputKey::q] = input_state.is_just_pressed(EKey::q);
    iks.input_key_to_just_pressed[InputKey::r] = input_state.is_just_pressed(EKey::r);
    iks.input_key_to_just_pressed[InputKey::s] = input_state.is_just_pressed(EKey::s);
    iks.input_key_to_just_pressed[InputKey::t] = input_state.is_just_pressed(EKey::t);
    iks.input_key_to_just_pressed[InputKey::u] = input_state.is_just_pressed(EKey::u);
    iks.input_key_to_just_pressed[InputKey::v] = input_state.is_just_pressed(EKey::v);
    iks.input_key_to_just_pressed[InputKey::w] = input_state.is_just_pressed(EKey::w);
    iks.input_key_to_just_pressed[InputKey::x] = input_state.is_just_pressed(EKey::x);
    iks.input_key_to_just_pressed[InputKey::y] = input_state.is_just_pressed(EKey::y);
    iks.input_key_to_just_pressed[InputKey::z] = input_state.is_just_pressed(EKey::z);

    iks.input_key_to_just_pressed[InputKey::SPACE] = input_state.is_just_pressed(EKey::SPACE);
    iks.input_key_to_just_pressed[InputKey::GRAVE_ACCENT] = input_state.is_just_pressed(EKey::GRAVE_ACCENT);
    iks.input_key_to_just_pressed[InputKey::TILDE] = input_state.is_just_pressed(EKey::TILDE);

    iks.input_key_to_just_pressed[InputKey::ONE] = input_state.is_just_pressed(EKey::ONE);
    iks.input_key_to_just_pressed[InputKey::TWO] = input_state.is_just_pressed(EKey::TWO);
    iks.input_key_to_just_pressed[InputKey::THREE] = input_state.is_just_pressed(EKey::THREE);
    iks.input_key_to_just_pressed[InputKey::FOUR] = input_state.is_just_pressed(EKey::FOUR);
    iks.input_key_to_just_pressed[InputKey::FIVE] = input_state.is_just_pressed(EKey::FIVE);
    iks.input_key_to_just_pressed[InputKey::SIX] = input_state.is_just_pressed(EKey::SIX);
    iks.input_key_to_just_pressed[InputKey::SEVEN] = input_state.is_just_pressed(EKey::SEVEN);
    iks.input_key_to_just_pressed[InputKey::EIGHT] = input_state.is_just_pressed(EKey::EIGHT);
    iks.input_key_to_just_pressed[InputKey::NINE] = input_state.is_just_pressed(EKey::NINE);
    iks.input_key_to_just_pressed[InputKey::ZERO] = input_state.is_just_pressed(EKey::ZERO);
    iks.input_key_to_just_pressed[InputKey::MINUS] = input_state.is_just_pressed(EKey::MINUS);
    iks.input_key_to_just_pressed[InputKey::EQUAL] = input_state.is_just_pressed(EKey::EQUAL);

    iks.input_key_to_just_pressed[InputKey::EXCLAMATION_POINT] = input_state.is_just_pressed(EKey::EXCLAMATION_POINT);
    iks.input_key_to_just_pressed[InputKey::AT_SIGN] = input_state.is_just_pressed(EKey::AT_SIGN);
    iks.input_key_to_just_pressed[InputKey::NUMBER_SIGN] = input_state.is_just_pressed(EKey::NUMBER_SIGN);
    iks.input_key_to_just_pressed[InputKey::DOLLAR_SIGN] = input_state.is_just_pressed(EKey::DOLLAR_SIGN);
    iks.input_key_to_just_pressed[InputKey::PERCENT_SIGN] = input_state.is_just_pressed(EKey::PERCENT_SIGN);
    iks.input_key_to_just_pressed[InputKey::CARET] = input_state.is_just_pressed(EKey::CARET);
    iks.input_key_to_just_pressed[InputKey::AMPERSAND] = input_state.is_just_pressed(EKey::AMPERSAND);
    iks.input_key_to_just_pressed[InputKey::ASTERISK] = input_state.is_just_pressed(EKey::ASTERISK);
    iks.input_key_to_just_pressed[InputKey::LEFT_PARENTHESIS] = input_state.is_just_pressed(EKey::LEFT_PARENTHESIS);
    iks.input_key_to_just_pressed[InputKey::RIGHT_PARENTHESIS] = input_state.is_just_pressed(EKey::RIGHT_PARENTHESIS);
    iks.input_key_to_just_pressed[InputKey::UNDERSCORE] = input_state.is_just_pressed(EKey::UNDERSCORE);
    iks.input_key_to_just_pressed[InputKey::PLUS] = input_state.is_just_pressed(EKey::PLUS);

    iks.input_key_to_just_pressed[InputKey::LEFT_SQUARE_BRACKET] =
        input_state.is_just_pressed(EKey::LEFT_SQUARE_BRACKET);
    iks.input_key_to_just_pressed[InputKey::RIGHT_SQUARE_BRACKET] =
        input_state.is_just_pressed(EKey::RIGHT_SQUARE_BRACKET);
    iks.input_key_to_just_pressed[InputKey::LEFT_CURLY_BRACKET] = input_state.is_just_pressed(EKey::LEFT_CURLY_BRACKET);
    iks.input_key_to_just_pressed[InputKey::RIGHT_CURLY_BRACKET] =
        input_state.is_just_pressed(EKey::RIGHT_CURLY_BRACKET);

    iks.input_key_to_just_pressed[InputKey::COMMA] = input_state.is_just_pressed(EKey::COMMA);
    iks.input_key_to_just_pressed[InputKey::PERIOD] = input_state.is_just_pressed(EKey::PERIOD);
    iks.input_key_to_just_pressed[InputKey::LESS_THAN] = input_state.is_just_pressed(EKey::LESS_THAN);
    iks.input_key_to_just_pressed[InputKey::GREATER_THAN] = input_state.is_just_pressed(EKey::GREATER_THAN);

    iks.input_key_to_just_pressed[InputKey::CAPS_LOCK] = input_state.is_just_pressed(EKey::CAPS_LOCK);
    iks.input_key_to_just_pressed[InputKey::ESCAPE] = input_state.is_just_pressed(EKey::ESCAPE);
    iks.input_key_to_just_pressed[InputKey::ENTER] = input_state.is_just_pressed(EKey::ENTER);
    iks.input_key_to_just_pressed[InputKey::TAB] = input_state.is_just_pressed(EKey::TAB);
    iks.input_key_to_just_pressed[InputKey::BACKSPACE] = input_state.is_just_pressed(EKey::BACKSPACE);
    iks.input_key_to_just_pressed[InputKey::INSERT] = input_state.is_just_pressed(EKey::INSERT);
    iks.input_key_to_just_pressed[InputKey::DELETE] = input_state.is_just_pressed(EKey::DELETE);

    iks.input_key_to_just_pressed[InputKey::RIGHT] = input_state.is_just_pressed(EKey::RIGHT);
    iks.input_key_to_just_pressed[InputKey::LEFT] = input_state.is_just_pressed(EKey::LEFT);
    iks.input_key_to_just_pressed[InputKey::UP] = input_state.is_just_pressed(EKey::UP);
    iks.input_key_to_just_pressed[InputKey::DOWN] = input_state.is_just_pressed(EKey::DOWN);

    iks.input_key_to_just_pressed[InputKey::SLASH] = input_state.is_just_pressed(EKey::SLASH);
    iks.input_key_to_just_pressed[InputKey::QUESTION_MARK] = input_state.is_just_pressed(EKey::QUESTION_MARK);
    iks.input_key_to_just_pressed[InputKey::BACKSLASH] = input_state.is_just_pressed(EKey::BACKSLASH);
    iks.input_key_to_just_pressed[InputKey::PIPE] = input_state.is_just_pressed(EKey::PIPE);
    iks.input_key_to_just_pressed[InputKey::COLON] = input_state.is_just_pressed(EKey::COLON);
    iks.input_key_to_just_pressed[InputKey::SEMICOLON] = input_state.is_just_pressed(EKey::SEMICOLON);
    iks.input_key_to_just_pressed[InputKey::SINGLE_QUOTE] = input_state.is_just_pressed(EKey::SINGLE_QUOTE);
    iks.input_key_to_just_pressed[InputKey::DOUBLE_QUOTE] = input_state.is_just_pressed(EKey::DOUBLE_QUOTE);

    iks.input_key_to_just_pressed[InputKey::LEFT_SHIFT] = input_state.is_just_pressed(EKey::LEFT_SHIFT);
    iks.input_key_to_just_pressed[InputKey::RIGHT_SHIFT] = input_state.is_just_pressed(EKey::RIGHT_SHIFT);
    iks.input_key_to_just_pressed[InputKey::LEFT_CONTROL] = input_state.is_just_pressed(EKey::LEFT_CONTROL);
    iks.input_key_to_just_pressed[InputKey::RIGHT_CONTROL] = input_state.is_just_pressed(EKey::RIGHT_CONTROL);
    iks.input_key_to_just_pressed[InputKey::LEFT_ALT] = input_state.is_just_pressed(EKey::LEFT_ALT);
    iks.input_key_to_just_pressed[InputKey::RIGHT_ALT] = input_state.is_just_pressed(EKey::RIGHT_ALT);
    iks.input_key_to_just_pressed[InputKey::LEFT_SUPER] = input_state.is_just_pressed(EKey::LEFT_SUPER);
    iks.input_key_to_just_pressed[InputKey::RIGHT_SUPER] = input_state.is_just_pressed(EKey::RIGHT_SUPER);

    iks.input_key_to_just_pressed[InputKey::FUNCTION_KEY] = input_state.is_just_pressed(EKey::FUNCTION_KEY);
    iks.input_key_to_just_pressed[InputKey::MENU_KEY] = input_state.is_just_pressed(EKey::MENU_KEY);

    iks.input_key_to_just_pressed[InputKey::LEFT_MOUSE_BUTTON] = input_state.is_just_pressed(EKey::LEFT_MOUSE_BUTTON);
    iks.input_key_to_just_pressed[InputKey::RIGHT_MOUSE_BUTTON] = input_state.is_just_pressed(EKey::RIGHT_MOUSE_BUTTON);
    iks.input_key_to_just_pressed[InputKey::MIDDLE_MOUSE_BUTTON] =
        input_state.is_just_pressed(EKey::MIDDLE_MOUSE_BUTTON);
    iks.input_key_to_just_pressed[InputKey::SCROLL_UP] = input_state.is_just_pressed(EKey::SCROLL_UP);
    // NOTE: the following one is causing out of range, instead of bug fixing this, I'm just not going to because we're
    // probably not going to use scroll for a while and fix the bug later
    // iks.input_key_to_is_pressed[InputKey::SCROLL_DOWN] = input_state.is_pressed(EKey::SCROLL_DOWN);

    // not required
    // iks.input_key_to_is_pressed[InputKey::DUMMY] = input_state.is_pressed(EKey::DUMMY);

    return iks;
}

int main(int argc, char *argv[]) {

    // NOTE: because the lsp server runs in another thread asychronously
    // if we allow the callbacks to be run the moment the lsp server responds
    // then the order in which logic runs can be different on each iteration
    // thus we queue up the callback logic into a vector so that we can
    // handle it in the main thread
    std::vector<std::function<void()>> lsp_callbacks_to_run_synchronously;

    ResourcePath rp(false);

#if defined(_WIN32) || defined(_WIN64)
    LSPClient lsp_client(
        rp.gfp("C:\\Users\\ccn\\projects\\cpp-toolbox-organization\\editor\\").string(), "cpp",
        rp.gfp("C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Tools\\Llvm\\bin\\clangd.exe")
            .string());
#else
    // LSPClient lsp_client("/home/ccn/projects/cpp-toolbox-organization/editor/");
    LSPClient lsp_client("");
#endif

    std::thread thread([&] {
        while (true) {
            lsp_client.process_requests_and_responses();
        }
    });

    int saved_for_automatic_column_adjustment = 0;
    int saved_last_col_for_automatic_column_adjustment = 0;
    int saved_last_line_for_automatic_column_adjustment = 0;

    unsigned int windowed_screen_width_px = 700;
    unsigned int windowed_screen_height_px = 700;

    bool automatic_column_adjustment = false;
    std::string username = "tbx_user";

    // numbers must be odd to have a center
    int num_lines = 41;
    int num_cols = 101;

    bool start_in_fullscreen = false;

    Configuration::SectionKeyPairToConfigLogic section_key_to_config_logic = {
        {{"graphics", "start_in_fullscreen"},
         [&](const std::string &value) {
             if (value == "true") {
                 start_in_fullscreen = true;
             }
         }},
        {{"graphics", "windowed_screen_width_px"},
         [&](const std::string &value) {
             if (is_integer(value)) {
                 windowed_screen_width_px = std::stoi(value);
                 std::cout << "set width to " << value << std::endl;
             } else {
                 std::cout << "Error: 'windowed_screen_width_px ' is not a valid integer: " << value << std::endl;
             }
         }},
        {{"graphics", "windowed_screen_height_px"},
         [&](const std::string &value) {
             if (is_integer(value)) {
                 windowed_screen_height_px = std::stoi(value);
             } else {
                 std::cout << "Error: 'windowed_screen_height_px ' is not a valid integer: " << value << std::endl;
             }
         }},
        {{"viewport", "automatic_column_adjustment"},
         [&](const std::string &value) {
             if (value == "true") {
                 automatic_column_adjustment = true;
             }
         }},
        {{"viewport", "num_lines"},
         [&](const std::string &value) {
             if (is_integer(value)) {
                 num_lines = std::stoi(value);
             } else {
                 std::cout << "Error: 'num_lines' is not a valid integer: " << value << std::endl;
             }
         }},
        {{"viewport", "num_cols"},
         [&](const std::string &value) {
             if (is_integer(value)) {
                 num_cols = std::stoi(value);
             } else {
                 std::cout << "Error: 'num_cols' is not a valid integer: " << value << std::endl;
             }
         }},
        {{"user", "name"}, [&](const std::string &value) { username = value; }},
    };

    std::filesystem::path config_path = "~/.tbx_cfg.ini";
    // Configuration config(config_path, section_key_to_config_logic);

    std::cout << username << std::endl;

    PeriodicSignal one_second_signal_for_status_bar_time_update(2);

    std::cout << get_executable_path(argv) << std::endl;

    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::debug);
    auto file_sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("mwe_shader_cache_logs.txt", true);
    file_sink->set_level(spdlog::level::info);
    std::vector<spdlog::sink_ptr> sinks = {console_sink, file_sink};

    Window window;
    window.initialize_glfw_glad_and_return_window(windowed_screen_width_px, windowed_screen_height_px, "glfw window",
                                                  start_in_fullscreen, false, false);

    std::vector<ShaderType> requested_shaders = {
        ShaderType::ABSOLUTE_POSITION_TEXTURED,
        ShaderType::ABSOLUTE_POSITION_WITH_SOLID_COLOR,
        ShaderType::TRANSFORM_V_WITH_SIGNED_DISTANCE_FIELD_TEXT,
        ShaderType::ABSOLUTE_POSITION_WITH_COLORED_VERTEX,
    };

    ShaderCache shader_cache(requested_shaders, sinks);
    Batcher batcher(shader_cache);
    setup_sdf_shader_uniforms(shader_cache);

    std::filesystem::path font_info_path =
        std::filesystem::path("assets") / "fonts" / "times_64_sdf_atlas_font_info.json";
    std::filesystem::path font_json_path = std::filesystem::path("assets") / "fonts" / "times_64_sdf_atlas.json";
    std::filesystem::path font_image_path = std::filesystem::path("assets") / "fonts" / "times_64_sdf_atlas.png";
    FontAtlas font_atlas(font_info_path.string(), font_json_path.string(), font_image_path.string(),
                         windowed_screen_width_px, false, true);

    std::string search_dir = ".";
    std::vector<std::string> ignore_dirs = {"build", ".git", "__pycache__"};
    std::vector<std::filesystem::path> searchable_files = rec_get_all_files(search_dir, ignore_dirs);
    /*for (const auto &file : searchable_files) {*/
    /*    std::cout << file.string() << '\n';*/
    /*}*/

    // FS BROWSER UI START
    std::vector<int> doids_for_textboxes_for_active_directory_for_later_removal;

    UI fs_browser(font_atlas);
    FileBrowser fb(1.5, 1.5);

    std::string temp = "File Search";
    std::string select = "select a file";
    fs_browser.add_colored_rectangle(fb.background_rect, colors.gray10);
    int curr_dir_doid = fs_browser.add_textbox(temp, fb.current_directory_rect, colors.gold);
    fs_browser.add_colored_rectangle(fb.main_file_view_rect, colors.gray40);
    int selected_file_doid = fs_browser.add_textbox(select, fb.file_selection_bar, colors.gray40);
    // FS BROWSER UI END

    // afbFS BROWSER UI START

    std::vector<int> afb_doids_for_textboxes_for_active_directory_for_later_removal;
    UI active_file_buffers_ui(font_atlas);
    FileBrowser afb(1.5, 1.5);

    std::string afb_label = "Active File Buffers";
    std::string afb_select = "select a file";
    std::vector<std::string> currently_matched_active_file_buffers;
    active_file_buffers_ui.add_colored_rectangle(afb.background_rect, colors.gray10);
    int afb_curr_dir_doid = active_file_buffers_ui.add_textbox(temp, afb.current_directory_rect, colors.gold);
    active_file_buffers_ui.add_colored_rectangle(afb.main_file_view_rect, colors.gray40);
    int afb_selected_file_doid = active_file_buffers_ui.add_textbox(select, afb.file_selection_bar, colors.gray40);
    // afbFS BROWSER UI END

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    InputState input_state;
    std::cout << "after constructor" << std::endl;

    std::unordered_map<EditorMode, glm::vec4> mode_to_cursor_color = {
        {MOVE_AND_EDIT, {.5, .5, .5, .5}},
        {INSERT, {.8, .8, .5, .5}},
        {VISUAL_SELECT, {.8, .5, .8, .5}},
        {COMMAND, {.8, .5, .5, .5}},
    };

    shader_cache.set_uniform(ShaderType::ABSOLUTE_POSITION_WITH_SOLID_COLOR, ShaderUniformVariable::RGBA_COLOR,
                             mode_to_cursor_color[MOVE_AND_EDIT]);

    int width, height;

    TextureAtlas monospaced_font_atlas("assets/font/font.json", "assets/font/font.png");

    int line_where_selection_mode_started = -1;
    int col_where_selection_mode_started = -1;
    float status_bar_top_pos = -0.90;
    float command_bar_top_pos = -0.95;
    float top_line_pos = 1;

    vertex_geometry::Rectangle file_buffer_rect = vertex_geometry::create_rectangle_from_corners(
        glm::vec3(-1, top_line_pos, 0), glm::vec3(1, top_line_pos, 0), glm::vec3(-1, status_bar_top_pos, 0),
        glm::vec3(1, status_bar_top_pos, 0));

    vertex_geometry::Rectangle status_bar_rect = vertex_geometry::create_rectangle_from_corners(
        glm::vec3(-1, status_bar_top_pos, 0), glm::vec3(1, status_bar_top_pos, 0),
        glm::vec3(-1, command_bar_top_pos, 0), glm::vec3(1, command_bar_top_pos, 0));

    vertex_geometry::Rectangle command_bar_rect = vertex_geometry::create_rectangle_from_corners(
        glm::vec3(-1, command_bar_top_pos, 0), glm::vec3(1, command_bar_top_pos, 0), glm::vec3(-1, -1, 0),
        glm::vec3(1, -1, 0));

    /*vertex_geometry::Grid file_buffer_grid(num_lines, num_cols, */
    vertex_geometry::Grid screen_grid(num_lines, num_cols, file_buffer_rect);
    vertex_geometry::Grid status_bar_grid(1, num_cols, status_bar_rect);
    vertex_geometry::Grid command_bar_grid(1, num_cols, command_bar_rect);

    int center_idx_x = num_cols / 2;
    int center_idx_y = num_lines / 2;

    glm::vec2 active_visible_screen_position;

    // Check if the user provided a file argument
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file>" << std::endl;
        return 1; // Return an error code if no argument is provided
    }

    std::string filename = argv[1];

    // initialize the system.
    auto file_buffer = std::make_shared<LineTextBuffer>();

    Viewport viewport(file_buffer, num_lines, num_cols, center_idx_y, center_idx_x);

    ModalEditor modal_editor(viewport);
    modal_editor.switch_files(filename, true);

    std::function<void(unsigned int)> char_callback = [&](unsigned int character_code) {
        if (modal_editor.fs_browser_is_active) {
        } else {
            modal_editor.insert_character_in_insert_mode(character_code);

            if (modal_editor.current_mode == COMMAND) {
                // Convert the character code to a character
                char character = static_cast<char>(character_code);
                modal_editor.command_bar_input += character;
                modal_editor.command_bar_input_signal.toggle_state();
            }
        }
    };

    // Define the key callback
    // TODO: these key callbacks need to be emptied out
    std::function<void(int, int, int, int)> key_callback = [&](int key, int scancode, int action, int mods) {
        // these events happen once when the key is pressed down, aka its non-repeating; a one time event

        if (action == GLFW_PRESS || action == GLFW_RELEASE) {
            Key &active_key = *input_state.glfw_code_to_key.at(key);
            bool is_pressed = (action == GLFW_PRESS);
            active_key.pressed_signal.set_signal(is_pressed);

            Key &enum_grabbed_key = *input_state.key_enum_to_object.at(active_key.key_enum);

            if (modal_editor.current_mode == MOVE_AND_EDIT && modal_editor.command_bar_input == ":") {
                modal_editor.command_bar_input = "";
                modal_editor.command_bar_input_signal.toggle_state();
            }

            if (modal_editor.current_mode == MOVE_AND_EDIT) {
                if (action == GLFW_PRESS) {
                    if (active_key.key_type == KeyType::ALPHA or active_key.key_type == KeyType::NUMERIC or
                        active_key.string_repr == "escape") {

                        std::string key_str = active_key.string_repr;

                        // print out the key that was just pressed
                        std::cout << "key_str:" << key_str << std::endl;

                        if (key_str == "u" && viewport.buffer->get_last_deleted_content() == "") {
                            modal_editor.command_bar_input = "Ain't no more history!";
                            modal_editor.command_bar_input_signal.toggle_state();
                        }
                    }
                    if (active_key.key_enum == EKey::ESCAPE or active_key.key_enum == EKey::CAPS_LOCK) {
                        modal_editor.potential_automatic_command = "";
                    }
                }
            }
        }

        if (action == GLFW_PRESS || action == GLFW_REPEAT) {
            switch (key) {
            case GLFW_KEY_CAPS_LOCK:
                modal_editor.current_mode = MOVE_AND_EDIT;
                modal_editor.mode_change_signal.toggle_state();
                break;
            case GLFW_KEY_BACKSPACE:
                if (modal_editor.fs_browser_is_active) {
                    std::cout << "search backspace" << std::endl;
                    modal_editor.fs_browser_search_query =
                        modal_editor.fs_browser_search_query.empty()
                            ? ""
                            : modal_editor.fs_browser_search_query.substr(
                                  0, modal_editor.fs_browser_search_query.size() - 1);
                    update_graphical_search_results(modal_editor.fs_browser_search_query, searchable_files, fb,
                                                    doids_for_textboxes_for_active_directory_for_later_removal,
                                                    fs_browser, modal_editor.search_results_changed_signal,
                                                    selected_file_doid, modal_editor.currently_matched_files);
                } else {
                    std::cout << "non seach backspace" << std::endl;
                    if (modal_editor.current_mode == INSERT) {
                        auto td = viewport.backspace_at_active_position();
                        if (td != EMPTY_TEXT_DIFF) {
                            lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
                        }
                    }
                }
                break;
            case GLFW_KEY_TAB:
                if (modal_editor.current_mode == INSERT) {
                    auto td = viewport.insert_tab_at_cursor();
                    if (td != EMPTY_TEXT_DIFF) {
                        lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
                    }
                }
                break;
            case GLFW_KEY_Y:
                if (modal_editor.current_mode == VISUAL_SELECT) {
                    std::string curr_sel = viewport.buffer->get_bounding_box_string(
                        line_where_selection_mode_started, col_where_selection_mode_started,
                        viewport.active_buffer_line_under_cursor, viewport.active_buffer_col_under_cursor);
                    glfwSetClipboardString(window.glfw_window, curr_sel.c_str());
                }
                break;
            default:
                break;
            }
        }
    };
    std::function<void(double, double)> mouse_pos_callback = [](double _, double _1) {};
    std::function<void(int, int, int)> mouse_button_callback = [](int _, int _1, int _2) {};
    GLFWLambdaCallbackManager glcm(window.glfw_window, char_callback, key_callback, mouse_pos_callback,
                                   mouse_button_callback);

    double last_time = 0.0;
    double delta_time = 0.0;
    while (!glfwWindowShouldClose(window.glfw_window)) {
        double current_time = glfwGetTime();
        delta_time = current_time - last_time;
        last_time = current_time;

        /*lsp_client.process_requests_and_responses();*/

        glfwGetFramebufferSize(window.glfw_window, &width, &height);

        glViewport(0, 0, width, height);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        render(viewport, screen_grid, status_bar_grid, command_bar_grid, modal_editor.command_bar_input,
               monospaced_font_atlas, batcher, modal_editor.command_bar_input_signal, center_idx_x, center_idx_y,
               num_cols, num_lines, col_where_selection_mode_started, line_where_selection_mode_started, shader_cache,
               mode_to_cursor_color, delta_time, one_second_signal_for_status_bar_time_update, modal_editor);

        // render UI stuff

        if (modal_editor.fs_browser_is_active) {

            for (auto &cb : fs_browser.get_colored_boxes()) {
                batcher.absolute_position_with_colored_vertex_shader_batcher.queue_draw(
                    cb.id, cb.ivpsc.indices, cb.ivpsc.xyz_positions, cb.ivpsc.rgb_colors);
            }

            for (auto &tb : fs_browser.get_text_boxes()) {
                bool should_change = modal_editor.search_results_changed_signal.has_just_changed();
                batcher.absolute_position_with_colored_vertex_shader_batcher.queue_draw(
                    tb.id, tb.background_ivpsc.indices, tb.background_ivpsc.xyz_positions,
                    tb.background_ivpsc.rgb_colors);

                batcher.transform_v_with_signed_distance_field_text_shader_batcher.queue_draw(
                    tb.id, tb.text_drawing_data.indices, tb.text_drawing_data.xyz_positions,
                    tb.text_drawing_data.texture_coordinates, tb.modified_signal.has_just_changed());
            }

            for (auto &tb : fs_browser.get_clickable_text_boxes()) {
                batcher.absolute_position_with_colored_vertex_shader_batcher.queue_draw(
                    tb.id, tb.ivpsc.indices, tb.ivpsc.xyz_positions, tb.ivpsc.rgb_colors,
                    tb.modified_signal.has_just_changed());

                batcher.transform_v_with_signed_distance_field_text_shader_batcher.queue_draw(
                    tb.id, tb.text_drawing_data.indices, tb.text_drawing_data.xyz_positions,
                    tb.text_drawing_data.texture_coordinates);
            }

            /*glDisable(GL_DEPTH_TEST);*/
            font_atlas.texture_atlas.bind_texture();
            batcher.absolute_position_with_colored_vertex_shader_batcher.draw_everything();
            batcher.transform_v_with_signed_distance_field_text_shader_batcher.draw_everything();
            /*glEnable(GL_DEPTH_TEST);*/
        }

        // render UI stuff

        // we must save the previous viewport screen before the screen is modified that way
        // the changes made will be compared against the previous screen state
        viewport.save_previous_viewport_screen();

        // run_key_logic(input_state, viewport, line_where_selection_mode_started, col_where_selection_mode_started,
        //               command_bar_input, command_bar_input_signal, window, searchable_files, fb,
        //               doids_for_textboxes_for_active_directory_for_later_removal, fs_browser, selected_file_doid,
        //               insert_mode_signal, modal_editor, lsp_callbacks_to_run_synchronously);

        modal_editor.iks = create_input_key_state(input_state);
        modal_editor.run_key_logic(searchable_files);

        // for (auto &callback : lsp_callbacks_to_run_synchronously) {
        //     std::cout << "running callback" << std::endl;
        //     callback();
        // }
        // lsp_callbacks_to_run_synchronously.clear();

        if (automatic_column_adjustment) {
            snap_to_end_of_line_while_navigating(viewport, saved_for_automatic_column_adjustment,
                                                 saved_last_col_for_automatic_column_adjustment,
                                                 saved_last_line_for_automatic_column_adjustment);
        }
        TemporalBinarySignal::process_all();
        glfwSwapBuffers(window.glfw_window);
        glfwPollEvents();
    }

    thread.detach();

    glfwDestroyWindow(window.glfw_window);

    glfwTerminate();
    exit(EXIT_SUCCESS);
}
