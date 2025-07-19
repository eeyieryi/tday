#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "config.h"

#define STRIKETHROUGH "\e[9m"
#define RESET_FMT "\e[0m"

void set_raw_mode(struct termios *old_termios) {
    struct termios new_termios;

    // Save current terminal settings
    tcgetattr(STDIN_FILENO, old_termios);

    // Create new terminal settings
    new_termios = *old_termios;

    // Disable canonical mode and echo
    new_termios.c_lflag &= ~(ICANON | ECHO);

    // Set minimum characters to 1 and timeout to 0
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;

    // Apply new settings
    tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);
}

void restore_terminal_mode(struct termios *tio) { tcsetattr(STDIN_FILENO, TCSANOW, tio); }

#define zero_array(arr, n)            \
    do {                              \
        for (int i = 0; i < n; i++) { \
            arr[i] = 0;               \
        }                             \
    } while (0)

#define zero_buf(buf, size, cursor)         \
    do {                                    \
        zero_array(buf, MAX_STRING_LENGTH); \
        (*size) = 0;                        \
        (*cursor) = 0;                      \
    } while (0)

#define return_defer(value) \
    do {                    \
        (result) = value;   \
        goto defer;         \
    } while (0)

#define check_db_err(db_err, ctx)                                         \
    do {                                                                  \
        if ((db_err) != SQLITE_OK) {                                      \
            fprintf(stderr, "[error] " ctx ": %s\n", sqlite3_errmsg(DB)); \
            return_defer(1);                                              \
        }                                                                 \
    } while (0)

#define MAX_SELECTION (entries_in_view - 1)
#define MAX_ENTRIES_IN_VIEW 10
#define MAX_STRING_LENGTH 64

typedef enum CurrentView { LIST_VIEW, NEW_ENTRY_VIEW, EDIT_ENTRY_VIEW } current_view_t;

typedef struct Entry {
    int id;
    int completed;
    int ignored;
    char description[MAX_STRING_LENGTH];
} entry_t;

#define create_table_entries_sql          \
    "CREATE TABLE IF NOT EXISTS entries(" \
    "id INTEGER PRIMARY KEY, "            \
    "description TEXT NOT NULL, "         \
    "completed INTEGER DEFAULT(0), "      \
    "ignored INTEGER DEFAULT(0)"          \
    ");"

#define migrate_table_entries_v1_sql "ALTER TABLE entries ADD COLUMN updated_at INTEGER;"

#define get_entries_sql \
    "SELECT * FROM entries WHERE ignored = 0 ORDER BY completed ASC, updated_at DESC, id DESC LIMIT 10;"

#define insert_entry_sql                 \
    "INSERT INTO entries (description) " \
    "VALUES (?);"

#define update_entry_sql                                                            \
    "UPDATE entries "                                                               \
    "SET description = ?, completed = ?, ignored = ?, updated_at = strftime('%s') " \
    "WHERE id = ?;"

#define delete_entry_sql "DELETE FROM entries WHERE id = ?;"

#define clear_completed_entries_sql "UPDATE entries SET ignored = 1 WHERE completed = 1;"

sqlite3 *DB;
sqlite3_stmt *get_entries_stmt = NULL;
sqlite3_stmt *insert_entry_stmt = NULL;
sqlite3_stmt *update_entry_stmt = NULL;
sqlite3_stmt *delete_entry_stmt = NULL;
sqlite3_stmt *clear_completed_entries_stmt = NULL;

int get_db_version(int *db_version) {
    int result = 0;
    sqlite3_stmt *stmt = NULL;
    result = sqlite3_prepare_v2(DB, "PRAGMA user_version;", -1, &stmt, 0);
    if (result != SQLITE_OK) {
        return_defer(result);
    }
    result = sqlite3_step(stmt);
    if (result != SQLITE_ROW) {
        return_defer(result);
    }
    (*db_version) = sqlite3_column_int(stmt, 0);
    result = sqlite3_step(stmt);
    if (result != SQLITE_DONE) {
        return_defer(result);
    }
    return_defer(0);
defer:
    sqlite3_finalize(stmt);
    return result;
}

int load_entries(entry_t *entries, int *entries_sz) {
    int result;
    result = sqlite3_step(get_entries_stmt);
    while (result == SQLITE_ROW) {
        entry_t entry = {0};
        entry.id = sqlite3_column_int(get_entries_stmt, 0);
        const unsigned char *description = sqlite3_column_text(get_entries_stmt, 1);
        int i = 0;
        for (; i < sqlite3_column_bytes(get_entries_stmt, 1); i++) {
            entry.description[i] = description[i];
        }
        entry.description[i] = '\0';

        entry.completed = sqlite3_column_int(get_entries_stmt, 2);
        entries[(*entries_sz)++] = entry;
        result = sqlite3_step(get_entries_stmt);
    }
    if (result != SQLITE_DONE) {
        fprintf(stderr, "[error] step_get_entries_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    return_defer(0);
defer:
    sqlite3_reset(get_entries_stmt);
    return result;
}

int new_entry(char *description) {
    int result;
    result = sqlite3_bind_text(insert_entry_stmt, 1, description, -1, SQLITE_STATIC);
    if (result != SQLITE_OK) {
        fprintf(stderr, "[error] bind_insert_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    result = sqlite3_step(insert_entry_stmt);
    if (result != SQLITE_DONE) {
        fprintf(stderr, "[error] step_insert_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(1);
    }
defer:
    sqlite3_clear_bindings(insert_entry_stmt);
    sqlite3_reset(insert_entry_stmt);
    return result;
}

int update_entry(entry_t *entry) {
    int result;
    result = sqlite3_bind_text(update_entry_stmt, 1, entry->description, -1, SQLITE_STATIC);
    if (result != SQLITE_OK) {
        fprintf(stderr, "[error] bind_description_update_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    result = sqlite3_bind_int(update_entry_stmt, 2, entry->completed);
    if (result != SQLITE_OK) {
        fprintf(stderr, "[error] bind_completed_update_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    result = sqlite3_bind_int(update_entry_stmt, 3, entry->ignored);
    if (result != SQLITE_OK) {
        fprintf(stderr, "[error] bind_completed_update_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    result = sqlite3_bind_int(update_entry_stmt, 4, entry->id);
    if (result != SQLITE_OK) {
        fprintf(stderr, "[error] bind_where_id_update_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    result = sqlite3_step(update_entry_stmt);
    if (result != SQLITE_DONE) {
        fprintf(stderr, "[error] step_update_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    return_defer(0);
defer:
    sqlite3_reset(update_entry_stmt);
    sqlite3_clear_bindings(update_entry_stmt);
    return result;
}

int delete_entry(entry_t *entry) {
    int result;
    result = sqlite3_bind_int(delete_entry_stmt, 1, entry->id);
    if (result != SQLITE_OK) {
        fprintf(stderr, "[error] bind_where_id_delete_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    result = sqlite3_step(delete_entry_stmt);
    if (result != SQLITE_DONE) {
        fprintf(stderr, "[error] step_delete_entry_stmt: %s\n", sqlite3_errmsg(DB));
        return_defer(result);
    }
    return_defer(0);
defer:
    sqlite3_reset(delete_entry_stmt);
    sqlite3_clear_bindings(delete_entry_stmt);
    return result;
}

int main(void) {
    int result = 0;
    int entries_sz = 0;
    int current_selection = 0;
    int entries_in_view = 0;
    entry_t entries[MAX_ENTRIES_IN_VIEW] = {0};

    struct termios old_tio;
    set_raw_mode(&old_tio);

    check_db_err(sqlite3_open(db_file_path, &DB), "sqlite3_open");
    check_db_err(sqlite3_exec(DB, create_table_entries_sql, NULL, 0, NULL), "create_table_entries");

    int db_version;
    check_db_err(get_db_version(&db_version), "get_db_version");
    if (db_version == 0) {
        sqlite3_exec(DB, "BEGIN TRANSACTION;", NULL, NULL, NULL);
        check_db_err(sqlite3_exec(DB, migrate_table_entries_v1_sql, NULL, 0, NULL), "migrate_table_entries_v1");
        check_db_err(sqlite3_exec(DB, "PRAGMA user_version = 1;", NULL, 0, NULL), "db_version_to_1");
        sqlite3_exec(DB, "END TRANSACTION;", NULL, NULL, NULL);
    }

    check_db_err(sqlite3_prepare_v2(DB, get_entries_sql, -1, &get_entries_stmt, 0), "prepare_get_entries_stmt");
    check_db_err(sqlite3_prepare_v2(DB, insert_entry_sql, -1, &insert_entry_stmt, 0), "prepare_insert_entry_stmt");
    check_db_err(sqlite3_prepare_v2(DB, update_entry_sql, -1, &update_entry_stmt, 0), "prepare_update_entry_stmt");
    check_db_err(sqlite3_prepare_v2(DB, delete_entry_sql, -1, &delete_entry_stmt, 0), "prepare_delete_entry_stmt");
    check_db_err(sqlite3_prepare_v2(DB, clear_completed_entries_sql, -1, &clear_completed_entries_stmt, 0),
                 "prepare_clear_completed_entries_stmt");

    current_view_t current_view = LIST_VIEW;

    char new_entry_buf[MAX_STRING_LENGTH];
    zero_array(new_entry_buf, MAX_STRING_LENGTH);
    int new_entry_buf_size = 0;
    int new_entry_buf_cursor = new_entry_buf_size;

    char edit_entry_buf[MAX_STRING_LENGTH];
    zero_array(edit_entry_buf, MAX_STRING_LENGTH);
    int edit_entry_buf_size = 0;
    int edit_entry_buf_cursor = edit_entry_buf_size;

    char input_handle_buf[3];
    zero_array(input_handle_buf, 3);

    char *buf = NULL;
    int *buf_size = NULL;
    int *buf_cursor = NULL;
    int buf_noop = 1;

    int should_reload_entries = 1;
    while (1) {
        if (should_reload_entries) {
            for (int i = 0; i < entries_sz; i++) {
                entries[i].id = 0;
                entries[i].completed = 0;
                entries[i].ignored = 0;
                int len = strlen(entries[i].description);
                for (int i = 0; i < len; i++) {
                    entries[i].description[i] = 0;
                }
                entries[i].description[0] = '\0';
            }
            entries_sz = 0;
            load_entries(entries, &entries_sz);
            entries_in_view = entries_sz > MAX_ENTRIES_IN_VIEW ? MAX_ENTRIES_IN_VIEW : entries_sz;
            should_reload_entries = 0;
        }

        switch (current_view) {
        case LIST_VIEW:
            // > tday
            //
            // [ ] work on tday
            // [ ] study opengl
            // [ ] read japanese
            //
            // up (k) / down (j) to move selection
            // space/enter to toggle completed status
            // (n)ew entry (e)dit (d)elete, (x) to clear completed
            // escape to (q)uit
            printf("\e[2J"); // Clear entire screen
            printf("\e[H");  // Move cursor to top-left
            printf("tday\n\n");
            for (int i = 0; i < entries_in_view; i++) {
                if (current_selection == i) {
                    printf("\e[33m> " RESET_FMT);
                } else {
                    printf("  ");
                }
                if (entries[i].completed) {
                    printf("[x] " STRIKETHROUGH);
                } else {
                    printf("[ ] ");
                }
                printf("%s" RESET_FMT "\n", entries[i].description);
            }
            if (entries_in_view == 0) {
                printf("no entries yet\n");
            }
            printf("\n");
            printf("up (k) / down (j) to move selection\n"
                   "space/enter to toggle completed status\n"
                   "(n)ew entry, (e)dit, (d)elete, (x) to clear completed\n"
                   "escape to (q)uit\n");
            break;
        case NEW_ENTRY_VIEW:
            // > tday
            //
            // new task description:
            // >
            //
            // enter to save, escape to go back
            printf("\e[2J"); // Clear entire screen
            printf("\e[H");  // Move cursor to top-left
            printf("tday\n\n");
            printf("new task description:\n");
            printf("\e[6;0H");
            printf("enter to save, escape to go back\n");
            printf("\e[4;0H");
            printf("> %.*s", new_entry_buf_size, new_entry_buf);
            printf("\e[4;%dH", new_entry_buf_cursor + 3);
            break;
        case EDIT_ENTRY_VIEW:
            // > tday
            //
            // description: %
            // > %
            //
            // enter to save, escape to discard changes
            printf("\e[2J"); // Clear entire screen
            printf("\e[H");  // Move cursor to top-left
            printf("tday\n\n");
            printf("description: %s\n", entries[current_selection].description);
            printf("\e[6;0H");
            printf("enter to save, escape to discard changes\n");
            printf("\e[4;0H");
            printf("> %.*s", edit_entry_buf_size, edit_entry_buf);
            printf("\e[4;%dH", edit_entry_buf_cursor + 3);
            break;
        }
        fflush(stdout);

        switch (current_view) {
        case LIST_VIEW:
            buf = NULL;
            buf_size = NULL;
            buf_cursor = NULL;
            buf_noop = 1;
        case NEW_ENTRY_VIEW:
            buf = new_entry_buf;
            buf_size = &new_entry_buf_size;
            buf_cursor = &new_entry_buf_cursor;
            buf_noop = 0;
            break;
        case EDIT_ENTRY_VIEW:
            buf = edit_entry_buf;
            buf_size = &edit_entry_buf_size;
            buf_cursor = &edit_entry_buf_cursor;
            buf_noop = 0;
            break;
        }

        if (read(STDIN_FILENO, &input_handle_buf, 3) > 0) {
            char ch = input_handle_buf[0];
            switch (ch) {
            case '\n': // ENTER
                switch (current_view) {
                case LIST_VIEW:
                    entries[current_selection].completed = entries[current_selection].completed ? 0 : 1;
                    update_entry(&entries[current_selection]);
                    should_reload_entries = 1;
                    break;
                case NEW_ENTRY_VIEW:
                    new_entry(buf);
                    zero_buf(buf, buf_size, buf_cursor);
                    current_view = LIST_VIEW;
                    should_reload_entries = 1;
                    break;
                case EDIT_ENTRY_VIEW: {
                    if ((*buf_size) == 0)
                        break;

                    int i = 0;
                    for (; i < (*buf_size); i++) {
                        entries[current_selection].description[i] = buf[i];
                    }
                    entries[current_selection].description[i] = '\0';

                    update_entry(&entries[current_selection]);
                    zero_buf(buf, buf_size, buf_cursor);
                    should_reload_entries = 1;
                    current_view = LIST_VIEW;
                } break;
                }
                break;
            case 'e':
                switch (current_view) {
                case LIST_VIEW:
                    if (entries_in_view > 0) {
                        int len = strlen(entries[current_selection].description);
                        for (int i = 0; i < len; i++) {
                            edit_entry_buf[i] = entries[current_selection].description[i];
                        }

                        edit_entry_buf_size = strlen(edit_entry_buf);
                        edit_entry_buf_cursor = edit_entry_buf_size;
                        current_view = EDIT_ENTRY_VIEW;
                    }
                    break;
                case NEW_ENTRY_VIEW:
                case EDIT_ENTRY_VIEW:
                    goto treat_as_char;
                }
                break;
            case 'd':
                switch (current_view) {
                case LIST_VIEW:
                    delete_entry(&entries[current_selection]);
                    should_reload_entries = 1;
                    current_selection -= 1;
                    if (current_selection < 0) {
                        current_selection = 0;
                    }
                    break;
                case NEW_ENTRY_VIEW:
                case EDIT_ENTRY_VIEW:
                    goto treat_as_char;
                }
                break;
            case 'x':
                switch (current_view) {
                case LIST_VIEW:
                    sqlite3_step(clear_completed_entries_stmt);
                    sqlite3_reset(clear_completed_entries_stmt);
                    should_reload_entries = 1;
                    break;
                case NEW_ENTRY_VIEW:
                case EDIT_ENTRY_VIEW:
                    goto treat_as_char;
                }
                break;
            case 'n':
                switch (current_view) {
                case LIST_VIEW:
                    current_view = NEW_ENTRY_VIEW;
                    break;
                case NEW_ENTRY_VIEW:
                case EDIT_ENTRY_VIEW:
                    goto treat_as_char;
                }
                break;
            case 'q':
                switch (current_view) {
                case LIST_VIEW:
                    return_defer(0);
                    break;
                case NEW_ENTRY_VIEW:
                case EDIT_ENTRY_VIEW:
                    goto treat_as_char;
                }
                break;
            case 127: // DELETE
                if (!buf_noop) {
                    if ((*buf_size) > 0 && (*buf_cursor) > 0) {
                        (*buf_cursor)--; // We want to delete the character to the left of the cursor
                        for (int i = *buf_cursor; i < (*buf_size); i++) {
                            buf[i] = buf[i + 1];
                        }
                        (*buf_size)--;
                    }
                }
                break;
            case 'k':
                if (current_view == LIST_VIEW) {
                    current_selection -= 1;
                    if (current_selection < 0) {
                        current_selection = MAX_SELECTION;
                    }
                } else {
                    goto treat_as_char;
                }
                break;
            case 'j':
                if (current_view == LIST_VIEW) {
                    current_selection += 1;
                    if (current_selection > MAX_SELECTION) {
                        current_selection = 0;
                    }
                } else {
                    goto treat_as_char;
                }
                break;
            case 27:
                ch = input_handle_buf[1];
                if (ch == '[') {
                    ch = input_handle_buf[2];
                    switch (ch) {
                    case 'A': // UP ARROW
                        if (current_view == LIST_VIEW) {
                            current_selection -= 1;
                            if (current_selection < 0) {
                                current_selection = MAX_SELECTION;
                            }
                        }
                        break;
                    case 'B': // DOWN ARROW
                        if (current_view == LIST_VIEW) {
                            current_selection += 1;
                            if (current_selection > MAX_SELECTION) {
                                current_selection = 0;
                            }
                        }
                        break;
                    case 'C': // RIGHT ARROW
                        if (!buf_noop) {
                            (*buf_cursor)++;
                            if ((*buf_cursor) > (*buf_size)) {
                                (*buf_cursor) = (*buf_size);
                            }
                        }
                        break;
                    case 'D': // LEFT ARROW
                        if (!buf_noop) {
                            (*buf_cursor)--;
                            if ((*buf_cursor) < 0) {
                                (*buf_cursor) = 0;
                            }
                        }
                        break;
                    default:
                        break;
                    }
                } else {
                    if (input_handle_buf[1] == 0) {
                        // ESCAPE
                        switch (current_view) {
                        case LIST_VIEW:
                            return_defer(0);
                            break;
                        case NEW_ENTRY_VIEW:
                            // Don't discard changes
                            current_view = LIST_VIEW;
                            break;
                        case EDIT_ENTRY_VIEW:
                            // Discard changes
                            current_view = LIST_VIEW;
                            zero_buf(buf, buf_size, buf_cursor);
                            break;
                        }
                    }
                }
                break;
            default:
            treat_as_char:
                switch (current_view) {
                case LIST_VIEW:
                    break;
                case NEW_ENTRY_VIEW:
                case EDIT_ENTRY_VIEW:
                    if ((*buf_size) + 1 > MAX_STRING_LENGTH) {
                        break;
                    }
                    (*buf_size)++;
                    for (int i = (*buf_size); i > (*buf_cursor); i--) {
                        buf[i] = buf[i - 1];
                    }
                    buf[(*buf_cursor)++] = ch;
                    break;
                }
                break;
            }

            // cleanup input buffer
            zero_array(input_handle_buf, 3);
        }
    }

defer:
    restore_terminal_mode(&old_tio);
    printf("Quitting program...\n");
    sqlite3_finalize(get_entries_stmt);
    sqlite3_finalize(insert_entry_stmt);
    sqlite3_finalize(update_entry_stmt);
    sqlite3_finalize(delete_entry_stmt);
    sqlite3_finalize(clear_completed_entries_stmt);
    sqlite3_close(DB);
    return result;
}
