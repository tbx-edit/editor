#include "modal_editor.hpp"

std::string input_key_to_string(InputKey key, bool shift_pressed) {
    switch (key) {
    case InputKey::a:
        return shift_pressed ? "A" : "a";
    case InputKey::b:
        return shift_pressed ? "B" : "b";
    case InputKey::c:
        return shift_pressed ? "C" : "c";
    case InputKey::d:
        return shift_pressed ? "D" : "d";
    case InputKey::e:
        return shift_pressed ? "E" : "e";
    case InputKey::f:
        return shift_pressed ? "F" : "f";
    case InputKey::g:
        return shift_pressed ? "G" : "g";
    case InputKey::h:
        return shift_pressed ? "H" : "h";
    case InputKey::i:
        return shift_pressed ? "I" : "i";
    case InputKey::j:
        return shift_pressed ? "J" : "j";
    case InputKey::k:
        return shift_pressed ? "K" : "k";
    case InputKey::l:
        return shift_pressed ? "L" : "l";
    case InputKey::m:
        return shift_pressed ? "M" : "m";
    case InputKey::n:
        return shift_pressed ? "N" : "n";
    case InputKey::o:
        return shift_pressed ? "O" : "o";
    case InputKey::p:
        return shift_pressed ? "P" : "p";
    case InputKey::q:
        return shift_pressed ? "Q" : "q";
    case InputKey::r:
        return shift_pressed ? "R" : "r";
    case InputKey::s:
        return shift_pressed ? "S" : "s";
    case InputKey::t:
        return shift_pressed ? "T" : "t";
    case InputKey::u:
        return shift_pressed ? "U" : "u";
    case InputKey::v:
        return shift_pressed ? "V" : "v";
    case InputKey::w:
        return shift_pressed ? "W" : "w";
    case InputKey::x:
        return shift_pressed ? "X" : "x";
    case InputKey::y:
        return shift_pressed ? "Y" : "y";
    case InputKey::z:
        return shift_pressed ? "Z" : "z";

    case InputKey::ZERO:
        return shift_pressed ? ")" : "0";
    case InputKey::ONE:
        return shift_pressed ? "!" : "1";
    case InputKey::TWO:
        return shift_pressed ? "@" : "2";
    case InputKey::THREE:
        return shift_pressed ? "#" : "3";
    case InputKey::FOUR:
        return shift_pressed ? "$" : "4";
    case InputKey::FIVE:
        return shift_pressed ? "%" : "5";
    case InputKey::SIX:
        return shift_pressed ? "^" : "6";
    case InputKey::SEVEN:
        return shift_pressed ? "&" : "7";
    case InputKey::EIGHT:
        return shift_pressed ? "*" : "8";
    case InputKey::NINE:
        return shift_pressed ? "(" : "9";

    case InputKey::SPACE:
        return " ";
    case InputKey::ENTER:
        return "\n";
    case InputKey::TAB:
        return "\t";
    case InputKey::COMMA:
        return shift_pressed ? "<" : ",";
    case InputKey::PERIOD:
        return shift_pressed ? ">" : ".";
    case InputKey::SLASH:
        return shift_pressed ? "?" : "/";
    case InputKey::SEMICOLON:
        return shift_pressed ? ":" : ";";
    case InputKey::SINGLE_QUOTE:
        return shift_pressed ? "\"" : "'";
    case InputKey::LEFT_SQUARE_BRACKET:
        return shift_pressed ? "{" : "[";
    case InputKey::RIGHT_SQUARE_BRACKET:
        return shift_pressed ? "}" : "]";
    case InputKey::MINUS:
        return shift_pressed ? "_" : "-";
    case InputKey::EQUAL:
        return shift_pressed ? "+" : "=";
    case InputKey::BACKSLASH:
        return shift_pressed ? "|" : "\\";

    default:
        return ""; // ignore unknown or non-character keys
    }
}

ModalEditor::ModalEditor(Viewport &viewport) : viewport(viewport) {
    rcr.add_regex("^x", [&](const std::smatch &m) { delete_at_current_cursor_position_logic(); });

    rcr.add_regex("^m", [&](const std::smatch &m) {
        if (current_mode == MOVE_AND_EDIT || current_mode == VISUAL_SELECT) {
            viewport.move_cursor_to_middle_of_line();
        }
    });

    rcr.add_regex("^\\$", [&](const std::smatch &m) {
        if (current_mode == MOVE_AND_EDIT || current_mode == VISUAL_SELECT) {
            viewport.move_cursor_to_end_of_line();
        }
    });

    rcr.add_regex("^[pP]", [&](const std::smatch &m) { paste_at_cursor_position_logic(m); });

    rcr.add_regex("^0", [&](const std::smatch &m) {
        if (current_mode == MOVE_AND_EDIT || current_mode == VISUAL_SELECT) {
            viewport.move_cursor_to_start_of_line();
        }
    });

    rcr.add_regex("^v", [&](const std::smatch &m) { start_visual_selection(); });

    rcr.add_regex("^i", [&](const std::smatch &m) { enter_insert_mode(); });

    rcr.add_regex("^a", [&](const std::smatch &m) { enter_insert_mode_after_cursor_position(); });

    rcr.add_regex("^A", [&](const std::smatch &m) { enter_insert_mode_at_end_of_line(); });

    rcr.add_regex("^I", [&](const std::smatch &m) {
        enter_insert_mode_in_front_of_first_non_whitespace_character_on_active_line();
    });

    rcr.add_regex("^\\^",
                  [&](const std::smatch &m) { move_cursor_to_first_non_whitespace_character_on_active_line(); });

    rcr.add_regex(R"(^(\d*)([jklh]))", [&](const std::smatch &m) { handle_hjkl_with_number_modifier(m); });

    rcr.add_regex(R"(^(\d*)G)", [&](const std::smatch &m) { go_to_specific_line_number(m); });

    rcr.add_regex(R"(^([cd]?)([fFtT])(.))",
                  [&](const std::smatch &m) { change_or_delete_till_or_find_to_character(m); });

    // Regex for [cd][webB] (change/delete with word motions)
    rcr.add_regex(R"(^([cd]?)([webB]))", [&](const std::smatch &m) { change_or_delete_using_word_motion(m); });

    // modification within brackets
    rcr.add_regex(R"(^([cd])([ai])([bB]))",
                  [&](const std::smatch &m) { change_or_delete_inside_or_around_brackets(m); });

    // go to top of file
    rcr.add_regex("^gg", [&](const std::smatch &m) {
        if (current_mode == MOVE_AND_EDIT) {
            viewport.set_active_buffer_line_under_cursor(0);
        }
    });

    rcr.add_regex("^[oO]", [&](const std::smatch &m) { open_new_line_below_or_above_current_line(m); });

    rcr.add_regex("^u", [&](const std::smatch &m) { undo(); });
    rcr.add_regex("^r", [&](const std::smatch &m) { redo(); });
    rcr.add_regex("^ sf", [&](const std::smatch &m) { launch_search_files(); });
    rcr.add_regex("^  ", [&](const std::smatch &m) { search_active_buffers(); });
    // rcr.add_regex("^ gd", [&](const std::smatch &m) { go_to_definition_dummy(); });

    rcr.add_regex("^dd", [&](const std::smatch &m) {
        if (current_mode == MOVE_AND_EDIT) {
            viewport.delete_line_at_cursor();
        }
    });

    rcr.add_regex("^yy", [&](const std::smatch &m) {
        if (current_mode == MOVE_AND_EDIT) {
            std::string current_line = viewport.buffer->get_line(viewport.active_buffer_line_under_cursor);
            // NOTE: commented out because trying not to depend on the clipboard thing, instead replace with lambda
            // functiont o call or something like that glfwSetClipboardString(window.glfw_window,
            // current_line.c_str());
        }
    });

    bool editing_a_cpp_project = true;
    if (editing_a_cpp_project) {
        rcr.add_regex("^ cc", [&](const std::smatch &m) { switch_to_cpp_source_file(); });

        rcr.add_regex("^ hh", [&](const std::smatch &m) { switch_to_hpp_source_file(); });
    }
    configured_rcr = true;
};

// this is here because I'm removing the dependency on lsp client
std::string root_project_directory = "";
std::string get_full_path(const std::string &file_path) {
#if defined(_WIN32) || defined(_WIN64)
    if (file_path.empty() || file_path[0] != 'C') {
        return root_project_directory + file_path;
    }
#else
    if (file_path.empty() || file_path[0] != '/') {
        return root_project_directory + file_path;
    }
#endif
    return file_path;
}

void ModalEditor::switch_files(const std::string &file_to_open, bool store_movements_to_history) {

    bool found_active_file_buffer = false;
    for (auto active_file_buffer : viewport.active_file_buffers) {
        std::cout << active_file_buffer->current_file_path << std::endl;
        if (active_file_buffer->current_file_path == file_to_open) {
            std::cout << "found matching buffer for " << file_to_open << " using it." << std::endl;
            viewport.switch_buffers_and_adjust_viewport_position(active_file_buffer, store_movements_to_history);
            found_active_file_buffer = true;
        }
    }

    if (not found_active_file_buffer) {
        std::cout << "didn't find matching buffer creating new buffer for: " << file_to_open << std::endl;
        auto ltb = std::make_shared<LineTextBuffer>();
        // ltb->load_file(lsp_client.get_full_path(file_to_open));
        ltb->load_file(get_full_path(file_to_open));
        // lsp_client.make_did_open_request(file_to_open);
        viewport.switch_buffers_and_adjust_viewport_position(ltb, store_movements_to_history);
    }
}

void ModalEditor::insert_character_in_insert_mode(unsigned int character_code) {
    if (current_mode == INSERT and not insert_mode_signal.has_just_changed()) {

        // update the thing or else it gets stuck
        if (insert_mode_signal.next_has_just_changed()) {
            return;
        }

        // Convert the character code to a character
        char character = static_cast<char>(character_code);

        auto td = viewport.insert_character_at_cursor(character);

        // lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);

        /*// Insert the character at the current cursor position*/
        /*if (!viewport.insert_character_at_cursor(character)) {*/
        /*    viewport.active_buffer_col_under_cursor viewport.active_buffer_line_under_cursor*/
        /*            // Handle the case where the insertion failed*/
        /*            std::cerr*/
        /*        << "Failed to insert character at cursor position.\n";*/
        /*}*/
    }
}

void ModalEditor::delete_at_current_cursor_position_logic() {

    if (current_mode == MOVE_AND_EDIT) {
        auto td = viewport.delete_character_at_active_position();
        if (td != EMPTY_TEXT_DIFF) {
            // lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
        }
    }

    if (current_mode == VISUAL_SELECT) {
        // TODO: make change request
        auto tms = viewport.buffer->delete_bounding_box(
            line_where_selection_mode_started, col_where_selection_mode_started,
            viewport.active_buffer_line_under_cursor, viewport.active_buffer_col_under_cursor);

        for (const auto &tm : tms) {
            if (tm != EMPTY_TEXT_DIFF) {
                // lsp_client.make_did_change_request(viewport.buffer->current_file_path, tm);
            }
        }

        viewport.set_active_buffer_line_col_under_cursor(line_where_selection_mode_started,
                                                         col_where_selection_mode_started);

        current_mode = MOVE_AND_EDIT;
        mode_change_signal.toggle_state();
    }
}

void ModalEditor::paste_at_cursor_position_logic(const std::smatch &m) {
    if (current_mode == MOVE_AND_EDIT) {
        if (m.str(0) == "P") {
            // Uppercase 'P' - insert content from the clipboard
            // NOTE: that this needs to be replaced somehow
            // const char *clipboard_content = glfwGetClipboardString(window.glfw_window);
            // if (clipboard_content) {
            //     // TODO: make change request
            //     viewport.insert_string_at_cursor(clipboard_content);
            // }
        } else if (m.str(0) == "p") {
            // Lowercase 'p' - insert the last deleted content
            // TODO: make change request
            viewport.insert_string_at_cursor(viewport.buffer->get_last_deleted_content());
        }
    }
}

void ModalEditor::start_visual_selection() {
    if (current_mode == MOVE_AND_EDIT) {
        current_mode = VISUAL_SELECT;
        mode_change_signal.toggle_state();
        line_where_selection_mode_started = viewport.active_buffer_line_under_cursor;
        col_where_selection_mode_started = viewport.active_buffer_col_under_cursor;
    }
}

void ModalEditor::enter_insert_mode() {
    // left control pressed then don't do this because of ctrl-i command
    if (not iks.is_pressed(InputKey::LEFT_CONTROL) and current_mode == MOVE_AND_EDIT) {
        current_mode = INSERT;
        insert_mode_signal.toggle_state();
        mode_change_signal.toggle_state();
    }
}

void ModalEditor::enter_insert_mode_after_cursor_position() {
    if (current_mode == MOVE_AND_EDIT) {
        viewport.scroll_right();
        current_mode = INSERT;
        insert_mode_signal.toggle_state();
        mode_change_signal.toggle_state();
    }
}

void ModalEditor::enter_insert_mode_at_end_of_line() {
    if (current_mode == MOVE_AND_EDIT) {
        viewport.move_cursor_to_end_of_line();
        current_mode = INSERT;
        insert_mode_signal.toggle_state();
        mode_change_signal.toggle_state();
    }
}

void ModalEditor::enter_insert_mode_in_front_of_first_non_whitespace_character_on_active_line() {
    if (current_mode == MOVE_AND_EDIT) {
        int ciofnwc = viewport.buffer->find_col_idx_of_first_non_whitespace_character_in_line(
            viewport.active_buffer_line_under_cursor);
        viewport.set_active_buffer_col_under_cursor(ciofnwc);
        current_mode = INSERT;
        insert_mode_signal.toggle_state();
        mode_change_signal.toggle_state();
    }
}

void ModalEditor::move_cursor_to_first_non_whitespace_character_on_active_line() {
    if (current_mode == MOVE_AND_EDIT) {
        int ciofnwc = viewport.buffer->find_col_idx_of_first_non_whitespace_character_in_line(
            viewport.active_buffer_line_under_cursor);
        viewport.set_active_buffer_col_under_cursor(ciofnwc);
    }
}

void ModalEditor::handle_hjkl_with_number_modifier(const std::smatch &m) {
    if (current_mode == MOVE_AND_EDIT or current_mode == VISUAL_SELECT) {
        int count = m[1].str().empty() ? 1 : std::stoi(m[1].str());
        char direction = m[2].str()[0];

        int line_delta = 0, col_delta = 0;
        switch (direction) {
        case 'j':
            if (viewport.buffer->line_count() > viewport.active_buffer_line_under_cursor + 1) {
                line_delta = count;
            }
            break; // Scroll down
        case 'k':
            if (viewport.active_buffer_line_under_cursor > 0) {
                line_delta = -count;
            }
            break; // Scroll up
        case 'h':
            if (viewport.active_buffer_col_under_cursor > 0) {
                col_delta = -count;
            }
            break; // Scroll left
        case 'l':
            const std::string &line = viewport.buffer->get_line(viewport.active_buffer_line_under_cursor);

            // TODO: this is bad because it stops me from being able to scroll rightware
            // if I want to on a blank line which is a real use case because sometimes
            // I want to be able to scroll right without caring about where I am in the file
            // note that this behavior is different than vim and thats ok
            /*if (line.size() > viewport.active_buffer_col_under_cursor) {*/
            /*    col_delta = count;*/
            /*}*/

            col_delta = count;

            break; // Scroll right
        }

        std::cout << "line delta: " << line_delta << ", col delta: " << col_delta << std::endl;
        viewport.scroll(line_delta, col_delta);
    }
}

void ModalEditor::go_to_specific_line_number(const std::smatch &m) {
    std::string digits = m[1].str();
    if (digits.empty()) {
        int last_line_index = viewport.buffer->line_count() - 1;
        viewport.set_active_buffer_line_under_cursor(last_line_index);
    } else {
        int number = std::stoi(digits);
        viewport.set_active_buffer_line_under_cursor(number - 1);
    }
}

void ModalEditor::change_or_delete_till_or_find_to_character(const std::smatch &m) {

    std::string action = m[1].str(); // 'c' (change) or 'd' (delete), optional
    std::string motion = m[2].str(); // 'f', 'F', 't', or 'T'
    char character = m[3].str()[0];  // The character to search for

    int col_idx = -1;

    // Handle motion commands
    if (motion == "f") {
        col_idx = viewport.buffer->find_rightward_index(viewport.active_buffer_line_under_cursor,
                                                        viewport.active_buffer_col_under_cursor, character);
    } else if (motion == "F") {
        col_idx = viewport.buffer->find_leftward_index(viewport.active_buffer_line_under_cursor,
                                                       viewport.active_buffer_col_under_cursor, character);
    } else if (motion == "t") {
        col_idx = viewport.buffer->find_rightward_index_before(viewport.active_buffer_line_under_cursor,
                                                               viewport.active_buffer_col_under_cursor, character);
    } else if (motion == "T") {
        col_idx = viewport.buffer->find_leftward_index_before(viewport.active_buffer_line_under_cursor,
                                                              viewport.active_buffer_col_under_cursor, character);
    }

    // Apply action based on motion result
    if (col_idx != -1) {
        if (!action.empty()) {
            // If action is 'c' or 'd', handle deletion and mode change
            viewport.buffer->delete_bounding_box(viewport.active_buffer_line_under_cursor,
                                                 viewport.active_buffer_col_under_cursor,
                                                 viewport.active_buffer_line_under_cursor, col_idx);
            if (action == "c") {
                current_mode = INSERT;
                mode_change_signal.toggle_state();
            }
        } else {
            viewport.set_active_buffer_col_under_cursor(col_idx);
        }
    } else {
        std::cout << "Character '" << character << "' not found for motion '" << motion << "'.\n";
    }
}

void ModalEditor::change_or_delete_using_word_motion(const std::smatch &m) {

    std::string command = m[1].str();
    std::string motion = m[2].str();

    // If no command is provided (meaning [cd] is missing), run these default motions
    if (command.empty()) {
        if (motion == "w") {
            viewport.move_cursor_forward_by_word();
        } else if (motion == "e") {
            viewport.move_cursor_forward_until_end_of_word();
        } else if (motion == "b") {
            viewport.move_cursor_backward_by_word();
        } else if (motion == "B") {
            viewport.move_cursor_backward_until_start_of_word();
        }
        return;
    }

    // otherwise run the deletion stuff
    int col_idx = -1;
    if (motion == "w") {
        // minus one is used here to mimic default vim behavior and not to delet the first character of the next
        // word
        col_idx = viewport.buffer->find_forward_by_word_index(viewport.active_buffer_line_under_cursor,
                                                              viewport.active_buffer_col_under_cursor) -
                  1;
    } else if (motion == "e") {
        col_idx = viewport.buffer->find_forward_to_end_of_word(viewport.active_buffer_line_under_cursor,
                                                               viewport.active_buffer_col_under_cursor);
    } else if (motion == "b") {
        col_idx = viewport.buffer->find_backward_to_start_of_word(viewport.active_buffer_line_under_cursor,
                                                                  viewport.active_buffer_col_under_cursor);
    } else if (motion == "B") {
        col_idx = viewport.buffer->find_backward_by_word_index(viewport.active_buffer_line_under_cursor,
                                                               viewport.active_buffer_col_under_cursor);
    }

    // Apply action based on motion result
    if (col_idx != -1) {
        viewport.buffer->delete_bounding_box(viewport.active_buffer_line_under_cursor,
                                             viewport.active_buffer_col_under_cursor,
                                             viewport.active_buffer_line_under_cursor, col_idx);
        if (command == "c") {
            current_mode = INSERT;
            insert_mode_signal.toggle_state();
            mode_change_signal.toggle_state();
        }
    }
}

void ModalEditor::change_or_delete_inside_or_around_brackets(const std::smatch &m) {

    std::string command = m[1].str();
    std::string inside_or_around = m[2].str();
    std::string bracket_type = m[3].str();

    int left_match_col = -1;
    int right_match_col = -1;

    if (bracket_type == "b") {
        left_match_col = viewport.buffer->find_column_index_of_previous_left_bracket(
            viewport.active_buffer_line_under_cursor, viewport.active_buffer_col_under_cursor);
        right_match_col = viewport.buffer->find_column_index_of_next_right_bracket(
            viewport.active_buffer_line_under_cursor, viewport.active_buffer_col_under_cursor);
    }

    if (bracket_type == "B") {
        left_match_col = viewport.buffer->find_column_index_of_character_leftward(
            viewport.active_buffer_line_under_cursor, viewport.active_buffer_col_under_cursor, '{');
        right_match_col = viewport.buffer->find_column_index_of_next_character(
            viewport.active_buffer_line_under_cursor, viewport.active_buffer_col_under_cursor, '}');
    }

    if (left_match_col == -1 or right_match_col == -1) {
        return;
    }

    if (inside_or_around == "i") {
        left_match_col++;
        right_match_col--;
    }

    viewport.buffer->delete_bounding_box(viewport.active_buffer_line_under_cursor, right_match_col,
                                         viewport.active_buffer_line_under_cursor, left_match_col);
    viewport.set_active_buffer_line_col_under_cursor(viewport.active_buffer_line_under_cursor, left_match_col);
}

void ModalEditor::open_new_line_below_or_above_current_line(const std::smatch &m) {

    // left control pressed then don't do this because of ctrl-o command
    if (not iks.is_pressed(InputKey::LEFT_CONTROL) and current_mode == MOVE_AND_EDIT) {
        if (m.str() == "O") {
            // Create a new line above the cursor and scroll up
            auto td = viewport.create_new_line_above_cursor_and_scroll_up();
            if (td != EMPTY_TEXT_DIFF) {
                // lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
            }
        } else {
            // Create a new line below the cursor and scroll down
            auto td = viewport.create_new_line_at_cursor_and_scroll_down();
            if (td != EMPTY_TEXT_DIFF) {
                // lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
            }
        }

        for (int i = 0; i < viewport.buffer->get_indentation_level(viewport.active_buffer_line_under_cursor,
                                                                   viewport.active_buffer_col_under_cursor);
             i++) {
            auto td = viewport.insert_tab_at_cursor();
            if (td != EMPTY_TEXT_DIFF) {
                // lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
            }
        }

        current_mode = INSERT;
        insert_mode_signal.toggle_state();
        mode_change_signal.toggle_state();
    }
}

void ModalEditor::undo() {
    if (current_mode == MOVE_AND_EDIT) {
        auto tm = viewport.buffer->undo();
        if (tm != EMPTY_TEXT_DIFF) {
            // lsp_client.make_did_change_request(viewport.buffer->current_file_path, tm);
        }
    }
}

void ModalEditor::redo() {
    if (current_mode == MOVE_AND_EDIT) {
        auto tm = viewport.buffer->redo();
        if (tm != EMPTY_TEXT_DIFF) {
            // lsp_client.make_did_change_request(viewport.buffer->current_file_path, tm);
        }
    }
}

void ModalEditor::launch_search_files() {
    if (current_mode == MOVE_AND_EDIT) {
        fs_browser_is_active = true;
    }
}

void ModalEditor::search_active_buffers() {
    if (current_mode == MOVE_AND_EDIT) {
        afb_is_active = true;
    }
}

void ModalEditor::switch_to_cpp_source_file() {
    if (current_mode == MOVE_AND_EDIT) {
        std::filesystem::path current_path = viewport.buffer->current_file_path;
        if (current_path.extension() == ".hpp") {
            std::filesystem::path cpp_path = current_path;
            cpp_path.replace_extension(".cpp");

            if (std::filesystem::exists(cpp_path)) {
                switch_files(cpp_path.string(), true);
            }
        }
    }
}

void ModalEditor::switch_to_hpp_source_file() {
    if (current_mode == MOVE_AND_EDIT) {
        std::filesystem::path current_path = viewport.buffer->current_file_path;
        if (current_path.extension() == ".cpp") {
            std::filesystem::path hpp_path = current_path;
            hpp_path.replace_extension(".hpp");

            if (std::filesystem::exists(hpp_path)) {
                switch_files(hpp_path.string(), true);
            }
        }
    }
}

void ModalEditor::run_key_logic(std::vector<std::filesystem::path> &searchable_files) {

    // less keystrokes please:
    // std::function<bool(EKey)> jp = [&](EKey k) { return input_state.is_just_pressed(k); };
    // std::function<bool(EKey)> ip = [&](EKey k) { return input_state.is_pressed(k); };

    std::function<bool(InputKey)> jp = [&](InputKey k) { return iks.is_just_pressed(k); };
    std::function<bool(InputKey)> ip = [&](InputKey k) { return iks.is_pressed(k); };

    // auto keys_just_pressed_this_tick = input_state.get_keys_just_pressed_this_tick();
    auto keys_just_pressed_this_tick = iks.get_keys_just_pressed_this_tick();
    if (not fs_browser_is_active) {
        if (current_mode != INSERT) {

            // NOTE: if keys just pressed this tick is has length greater or equal to 2, then that implies two keys were
            // pressed ina single tick, should be rare enough to ignore, but note that it may be a cause for later bugs.
            if (not keys_just_pressed_this_tick.empty()) {
                for (const auto &key : keys_just_pressed_this_tick) {
                    potential_regex_command += key;
                }
                std::cout << "prc: " << potential_regex_command << std::endl;
            }

            bool command_was_run = rcr.potentially_run_command(potential_regex_command);
            if (command_was_run) {
                potential_regex_command = "";
            }

            if (jp(InputKey::ESCAPE) or jp(InputKey::CAPS_LOCK)) {
                potential_regex_command = "";
            }

            bool key_pressed_based_command_run = false;
            if (current_mode == MOVE_AND_EDIT) {
                if (ip(InputKey::LEFT_CONTROL)) {
                    if (jp(InputKey::u)) {
                        viewport.scroll(-5, 0);
                        key_pressed_based_command_run = true;
                    }
                    if (jp(InputKey::d)) {
                        viewport.scroll(5, 0);
                        key_pressed_based_command_run = true;
                    }
                    if (jp(InputKey::o)) {
                        viewport.history.go_back();
                        auto [file_path, line, col] = viewport.history.get_current_history_flc();
                        std::cout << "going back to: " << file_path << line << col << std::endl;
                        if (file_path != viewport.buffer->current_file_path) {
                            switch_files(file_path, false);
                        }
                        viewport.set_active_buffer_line_col_under_cursor(line, col, false);
                    }
                    if (jp(InputKey::i)) {
                        viewport.history.go_forward();
                        auto [file_path, line, col] = viewport.history.get_current_history_flc();
                        std::cout << "going forward to: " << file_path << line << col << std::endl;
                        if (file_path != viewport.buffer->current_file_path) {
                            switch_files(file_path, false);
                        }
                        viewport.set_active_buffer_line_col_under_cursor(line, col, false);
                    }
                }
                if (ip(InputKey::LEFT_SHIFT)) {

                    if (jp(InputKey::m)) {
                        int last_line_index = (viewport.buffer->line_count() - 1) / 2;
                        viewport.set_active_buffer_line_under_cursor(last_line_index);
                        key_pressed_based_command_run = true;
                    }
                }

                if (jp(InputKey::LEFT_SHIFT)) {
                    if (jp(InputKey::n)) {
                        // Check if there are any search results
                        if (!search_results.empty()) {
                            // Move to the previous search result, using forced positive modulo
                            current_search_index =
                                (current_search_index - 1 + search_results.size()) % search_results.size();
                            TextRange sti = search_results[current_search_index];
                            viewport.set_active_buffer_line_col_under_cursor(sti.start_line, sti.start_col);

                        } else {
                            std::cout << "No search results found" << std::endl;
                        }
                        key_pressed_based_command_run = true;
                    }
                } else {
                    if (jp(InputKey::n)) {
                        std::cout << "next one" << std::endl;

                        if (!search_results.empty()) {
                            current_search_index = (current_search_index + 1) % search_results.size();
                            TextRange sti = search_results[current_search_index];
                            viewport.set_active_buffer_line_col_under_cursor(sti.start_line, sti.start_col);
                        } else {
                            std::cout << "No search results found" << std::endl;
                        }

                        key_pressed_based_command_run = true;
                    }
                }
                if (ip(InputKey::LEFT_SHIFT)) {
                    if (jp(InputKey::SEMICOLON)) {
                        current_mode = COMMAND;
                        command_bar_input = ":";
                        command_bar_input_signal.toggle_state();
                        mode_change_signal.toggle_state();

                        key_pressed_based_command_run = true;
                    }
                }
                if (jp(InputKey::SLASH)) {
                    current_mode = COMMAND;
                    command_bar_input = "/";
                    command_bar_input_signal.toggle_state();
                    mode_change_signal.toggle_state();
                    key_pressed_based_command_run = true;
                }

            } else if (current_mode == COMMAND) {
                // only doing this cause space isn't handled by the char callback
                if (jp(InputKey::ENTER)) {
                    if (command_bar_input == ":w") {
                        viewport.buffer->save_file();
                        key_pressed_based_command_run = true;
                    }
                    if (command_bar_input == ":q") {
                        // glfwSetWindowShouldClose(window.glfw_window, true);
                        key_pressed_based_command_run = true;
                    }

                    if (command_bar_input == ":tfs") {
                        // window.toggle_fullscreen();
                        key_pressed_based_command_run = true;
                    }
                    if (command_bar_input.front() == '/') {
                        std::string search_request = command_bar_input.substr(1); // remove the "/"
                        search_results = viewport.buffer->find_forward_matches(viewport.active_buffer_line_under_cursor,
                                                                               viewport.active_buffer_col_under_cursor,
                                                                               search_request);
                        if (!search_results.empty()) {
                            std::cout << "search active true now" << std::endl;
                            current_search_index = 0; // start from the first result

                            // Print out matches
                            std::cout << "Search Results for '" << search_request << "':\n";
                            for (const auto &result : search_results) {
                                // Assuming SubTextIndex has `line` and `col` attributes for position
                                std::cout << "Match at Line: " << result.start_line << ", Column: " << result.start_col
                                          << "\n";
                                // If you want to print the actual text matched:
                                /*std::cout << "Matched text: " << matched_text << "\n";*/
                            }
                            // You may want to highlight the first search result here
                            // highlight_search_result(search_results[current_search_index]);
                        }
                    }

                    command_bar_input = "";
                    command_bar_input_signal.toggle_state();
                    current_mode = MOVE_AND_EDIT;
                    mode_change_signal.toggle_state();
                }
            }

            if (key_pressed_based_command_run) {
                potential_regex_command = "";
            }
        }
    } else { // otherwise we are in the case that the file browser is active
        if (not keys_just_pressed_this_tick.empty()) {
            for (const auto &key : keys_just_pressed_this_tick) {
                fs_browser_search_query += key;
            }

            if (searchable_files.empty()) {
                std::cout << "No files found in the search directory." << std::endl;
            } else {

                // update_graphical_search_results(fs_browser_search_query, searchable_files, fb,
                //                                 doids_for_textboxes_for_active_directory_for_later_removal,
                //                                 fs_browser, search_results_changed_signal,
                //                                 selected_file_doid, currently_matched_files);
            }
        }

        if (jp(InputKey::ENTER)) {
            if (currently_matched_files.size() != 0) {
                std::cout << "about to load up: " << currently_matched_files[0] << std::endl;
                std::string file_to_open = currently_matched_files[0];
                std::cout << "file_to_open: " << file_to_open << std::endl;

                switch_files(file_to_open, true);

                fs_browser_is_active = false;
                fs_browser_search_query = "";

                return;
            }
        }

        if (jp(InputKey::CAPS_LOCK) or jp(InputKey::ESCAPE)) {
            std::cout << "tried to turn off fb" << std::endl;
            fs_browser_is_active = false;
            return;
        }
    }

    if (fs_browser_is_active) {
        return;
    }

    if (jp(InputKey::ESCAPE)) {
        current_mode = MOVE_AND_EDIT;
        mode_change_signal.toggle_state();
    }

    std::function<void()> shared_m_and_e_and_visual_selection_logic = [&]() {};

    if (current_mode == INSERT) {
        if (jp(InputKey::ENTER)) {
            auto td = viewport.create_new_line_at_cursor_and_scroll_down();
            if (td != EMPTY_TEXT_DIFF) {
                // lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
            }
            // TODO: don't use a loop make it so you can pass in the indentation level instead
            for (int i = 0; i < viewport.buffer->get_indentation_level(viewport.active_buffer_line_under_cursor,
                                                                       viewport.active_buffer_col_under_cursor);
                 i++) {
                auto td = viewport.insert_tab_at_cursor();
                if (td != EMPTY_TEXT_DIFF) {
                    // lsp_client.make_did_change_request(viewport.buffer->current_file_path, td);
                }
            }
        }
    }
}
