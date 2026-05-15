/*
 * Fuhgeddaboudit — Remove games from the NextUI play activity tracker
 */

#define AP_IMPLEMENTATION
#include "apostrophe.h"

#define PAKKIT_UI_IMPLEMENTATION
#include "pakkit_ui.h"

#include <sqlite3.h>

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define DB_PATH    "/mnt/SDCARD/.userdata/shared/game_logs.sqlite"
#define ROMS_PATH  "/mnt/SDCARD/Roms"
#define MAX_GAMES  256
#define FUHGED_MAX_PATH   1280

/* -----------------------------------------------------------------------
 * Game entry loaded from the database
 * ----------------------------------------------------------------------- */

typedef struct {
    int          rom_id;
    char         name[256];
    char         file_path[FUHGED_MAX_PATH];
    int          play_count;
    int          play_time_total;
    SDL_Texture *thumbnail;
} game_entry;

static game_entry games[MAX_GAMES];
static int game_count = 0;
static SDL_Texture *default_thumb = NULL;

typedef enum { SORT_TIME, SORT_NAME, SORT_PLAYS, SORT_COUNT } sort_mode;
static sort_mode current_sort = SORT_TIME;
static int sort_reversed = 0;

static const char *sort_label(sort_mode m) {
    switch (m) {
        case SORT_NAME:  return "Name";
        case SORT_TIME:  return "Time";
        case SORT_PLAYS: return "Plays";
        default:         return "Time";
    }
}

static int cmp_by_time(const void *a, const void *b) {
    int d = ((const game_entry *)b)->play_time_total - ((const game_entry *)a)->play_time_total;
    return d ? d : strcasecmp(((const game_entry *)a)->name, ((const game_entry *)b)->name);
}

static int cmp_by_name(const void *a, const void *b) {
    return strcasecmp(((const game_entry *)a)->name, ((const game_entry *)b)->name);
}

static int cmp_by_plays(const void *a, const void *b) {
    int d = ((const game_entry *)b)->play_count - ((const game_entry *)a)->play_count;
    return d ? d : strcasecmp(((const game_entry *)a)->name, ((const game_entry *)b)->name);
}

static void reverse_games(void) {
    for (int i = 0; i < game_count / 2; i++) {
        game_entry tmp = games[i];
        games[i] = games[game_count - 1 - i];
        games[game_count - 1 - i] = tmp;
    }
}

static void sort_games(void) {
    switch (current_sort) {
        case SORT_TIME:  qsort(games, game_count, sizeof(game_entry), cmp_by_time);  break;
        case SORT_NAME:  qsort(games, game_count, sizeof(game_entry), cmp_by_name);  break;
        case SORT_PLAYS: qsort(games, game_count, sizeof(game_entry), cmp_by_plays); break;
        default: break;
    }
    if (sort_reversed) reverse_games();
}

/* -----------------------------------------------------------------------
 * Format seconds into h:mm:ss
 * ----------------------------------------------------------------------- */

static void format_time(char *buf, size_t len, int seconds) {
    if (seconds <= 0) {
        snprintf(buf, len, "0:00:00");
        return;
    }
    int h = seconds / 3600;
    int m = (seconds % 3600) / 60;
    int s = seconds % 60;
    snprintf(buf, len, "%d:%02d:%02d", h, m, s);
}

/* -----------------------------------------------------------------------
 * Build image path from rom file_path (mirrors NextUI's get_rom_image_path)
 * ----------------------------------------------------------------------- */

static void get_image_path(const char *rom_file, char *out, size_t out_len) {
    const char *ext = strrchr(rom_file, '.');
    if (ext && (strcmp(ext, ".p8") == 0 || strcmp(ext, ".png") == 0)) {
        snprintf(out, out_len, "%s/%s", ROMS_PATH, rom_file);
        return;
    }

    char folder[FUHGED_MAX_PATH] = "";
    const char *last_slash = strrchr(rom_file, '/');
    if (last_slash) {
        size_t flen = (size_t)(last_slash - rom_file);
        if (flen >= sizeof(folder)) flen = sizeof(folder) - 1;
        memcpy(folder, rom_file, flen);
        folder[flen] = '\0';
    }

    const char *basename = last_slash ? last_slash + 1 : rom_file;
    char clean_name[256];
    snprintf(clean_name, sizeof(clean_name), "%s", basename);
    char *dot = strrchr(clean_name, '.');
    if (dot) *dot = '\0';

    snprintf(out, out_len, "%s/%s/.media/%s.png", ROMS_PATH, folder, clean_name);
}

/* -----------------------------------------------------------------------
 * Load image as texture with rounded corners
 * ----------------------------------------------------------------------- */

static SDL_Texture *load_rounded_image(const char *path, int size, int radius) {
    SDL_Surface *raw = IMG_Load(path);
    if (!raw) return NULL;

    SDL_Surface *img = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(raw);
    if (!img) return NULL;

    SDL_Surface *dst = SDL_CreateRGBSurfaceWithFormat(0, size, size, 32, SDL_PIXELFORMAT_RGBA32);
    if (!dst) { SDL_FreeSurface(img); return NULL; }

    SDL_BlitScaled(img, NULL, dst, NULL);
    SDL_FreeSurface(img);

    /* Apply rounded corner alpha mask */
    SDL_LockSurface(dst);
    Uint32 *pixels = (Uint32 *)dst->pixels;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int cx = 0, cy = 0;
            if (x < radius && y < radius) { cx = radius - x; cy = radius - y; }
            else if (x >= size - radius && y < radius) { cx = x - (size - radius - 1); cy = radius - y; }
            else if (x < radius && y >= size - radius) { cx = radius - x; cy = y - (size - radius - 1); }
            else if (x >= size - radius && y >= size - radius) { cx = x - (size - radius - 1); cy = y - (size - radius - 1); }
            if (cx * cx + cy * cy > radius * radius)
                pixels[y * (dst->pitch / 4) + x] = 0;
        }
    }
    SDL_UnlockSurface(dst);

    SDL_Texture *tex = SDL_CreateTextureFromSurface(ap__g.renderer, dst);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(dst);
    return tex;
}

/* -----------------------------------------------------------------------
 * Load games from database
 * ----------------------------------------------------------------------- */

static int load_games(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    game_count = 0;

    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return -1;

    const char *sql =
        "SELECT rom.id, rom.name, rom.file_path, "
        "  COUNT(play_activity.ROWID) AS play_count, "
        "  COALESCE(SUM(play_activity.play_time), 0) AS play_time_total "
        "FROM rom "
        "LEFT JOIN play_activity ON rom.id = play_activity.rom_id "
        "GROUP BY rom.id "
        "HAVING play_time_total > 0;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW && game_count < MAX_GAMES) {
        game_entry *g = &games[game_count];
        g->rom_id = sqlite3_column_int(stmt, 0);

        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        snprintf(g->name, sizeof(g->name), "%s", name ? name : "(unknown)");

        const char *fpath = (const char *)sqlite3_column_text(stmt, 2);
        snprintf(g->file_path, sizeof(g->file_path), "%s", fpath ? fpath : "");

        g->play_count = sqlite3_column_int(stmt, 3);
        g->play_time_total = sqlite3_column_int(stmt, 4);
        g->thumbnail = NULL;
        game_count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

/* -----------------------------------------------------------------------
 * Load thumbnail textures for all games
 * ----------------------------------------------------------------------- */

static void load_thumbnails(void) {
    int thumb_size = AP_DS(42);
    int thumb_r = thumb_size / 2;
    for (int i = 0; i < game_count; i++) {
        if (games[i].file_path[0] == '\0') continue;
        char img_path[FUHGED_MAX_PATH];
        get_image_path(games[i].file_path, img_path, sizeof(img_path));
        games[i].thumbnail = load_rounded_image(img_path, thumb_size, thumb_r);
    }
}

static void free_thumbnails(void) {
    for (int i = 0; i < game_count; i++) {
        if (games[i].thumbnail) {
            SDL_DestroyTexture(games[i].thumbnail);
            games[i].thumbnail = NULL;
        }
    }
}

/* -----------------------------------------------------------------------
 * Duplicate detection and merge
 * ----------------------------------------------------------------------- */

typedef struct {
    char name[256];
    int  count;
} dup_group;

static dup_group dup_groups[MAX_GAMES];
static int dup_group_count = 0;

typedef struct {
    int          rom_id;
    char         file_path[FUHGED_MAX_PATH];
    int          play_count;
    int          play_time_total;
    SDL_Texture *thumbnail;
} dup_entry;

static dup_entry dup_entries[MAX_GAMES];
static int dup_entry_count = 0;

static int find_duplicates(void) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    dup_group_count = 0;

    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return -1;

    const char *sql =
        "SELECT rom.name, COUNT(*) AS cnt "
        "FROM rom "
        "GROUP BY rom.name "
        "HAVING cnt > 1;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW && dup_group_count < MAX_GAMES) {
        dup_group *g = &dup_groups[dup_group_count];
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        snprintf(g->name, sizeof(g->name), "%s", name ? name : "(unknown)");
        g->count = sqlite3_column_int(stmt, 1);
        dup_group_count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

static int load_dup_entries(const char *name) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    dup_entry_count = 0;

    if (sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
        return -1;

    const char *sql =
        "SELECT rom.id, rom.file_path, "
        "  COUNT(play_activity.ROWID) AS play_count, "
        "  COALESCE(SUM(play_activity.play_time), 0) AS play_time_total "
        "FROM rom "
        "LEFT JOIN play_activity ON rom.id = play_activity.rom_id "
        "WHERE rom.name = ? "
        "GROUP BY rom.id;";

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);

    while (sqlite3_step(stmt) == SQLITE_ROW && dup_entry_count < MAX_GAMES) {
        dup_entry *e = &dup_entries[dup_entry_count];
        e->rom_id = sqlite3_column_int(stmt, 0);
        const char *fpath = (const char *)sqlite3_column_text(stmt, 1);
        snprintf(e->file_path, sizeof(e->file_path), "%s", fpath ? fpath : "");
        e->play_count = sqlite3_column_int(stmt, 2);
        e->play_time_total = sqlite3_column_int(stmt, 3);
        dup_entry_count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

static void load_dup_thumbnails(void) {
    int thumb_size = AP_DS(42);
    int thumb_r = thumb_size / 2;
    for (int i = 0; i < dup_entry_count; i++) {
        dup_entries[i].thumbnail = NULL;
        if (dup_entries[i].file_path[0] == '\0') continue;
        char img_path[FUHGED_MAX_PATH];
        get_image_path(dup_entries[i].file_path, img_path, sizeof(img_path));
        dup_entries[i].thumbnail = load_rounded_image(img_path, thumb_size, thumb_r);
    }
}

static void free_dup_thumbnails(void) {
    for (int i = 0; i < dup_entry_count; i++) {
        if (dup_entries[i].thumbnail) {
            SDL_DestroyTexture(dup_entries[i].thumbnail);
            dup_entries[i].thumbnail = NULL;
        }
    }
}

static int merge_into(int keep_id, const char *name) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK)
        return -1;

    sqlite3_stmt *stmt = NULL;

    const char *reassign =
        "UPDATE play_activity SET rom_id = ? "
        "WHERE rom_id IN (SELECT id FROM rom WHERE name = ? AND id != ?);";
    if (sqlite3_prepare_v2(db, reassign, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, keep_id);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, keep_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    const char *del_dups =
        "DELETE FROM rom WHERE name = ? AND id != ?;";
    if (sqlite3_prepare_v2(db, del_dups, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, keep_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return 0;
}

/* -----------------------------------------------------------------------
 * Delete a game's play activity and rom entry
 * ----------------------------------------------------------------------- */

static int delete_game(int rom_id) {
    sqlite3 *db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK)
        return -1;

    sqlite3_stmt *stmt = NULL;
    const char *del_activity = "DELETE FROM play_activity WHERE rom_id = ?;";
    if (sqlite3_prepare_v2(db, del_activity, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, rom_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    const char *del_rom = "DELETE FROM rom WHERE id = ?;";
    if (sqlite3_prepare_v2(db, del_rom, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, rom_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return 0;
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    const char *pak_dir = getenv("FUHGEDDABOUDIT_PAK_DIR");
    char bg_path[FUHGED_MAX_PATH];
    if (pak_dir)
        snprintf(bg_path, sizeof(bg_path), "%s/res/bg.png", pak_dir);
    else
        snprintf(bg_path, sizeof(bg_path), "res/bg.png");

    ap_config cfg = {
        .window_title = "Fuhgeddaboudit",
        .bg_image_path = bg_path,
    };
    ap_init(&cfg);

    /* Splash screen */
    {
        char splash_path[FUHGED_MAX_PATH];
        if (pak_dir)
            snprintf(splash_path, sizeof(splash_path), "%s/res/splash.png", pak_dir);
        else
            snprintf(splash_path, sizeof(splash_path), "res/splash.png");
        SDL_Texture *splash = ap_load_image(splash_path);
        if (splash) {
            int sw = ap_get_screen_width();
            int sh = ap_get_screen_height();
            int img_w, img_h;
            SDL_QueryTexture(splash, NULL, NULL, &img_w, &img_h);
            float scale_w = (float)sw / (float)img_w;
            float scale_h = (float)sh / (float)img_h;
            float scale = (scale_w < scale_h) ? scale_w : scale_h;
            int draw_w = (int)(img_w * scale);
            int draw_h = (int)(img_h * scale);
            int x = (sw - draw_w) / 2;
            int y = (sh - draw_h) / 2;
            ap_clear_screen();
            /* Fill with sky blue to match the sign image background */
            {
                SDL_Renderer *rend = ap__g.renderer;
                SDL_SetRenderDrawColor(rend, 0x87, 0xCE, 0xEB, 0xFF);
                SDL_Rect full = {0, 0, sw, sh};
                SDL_RenderFillRect(rend, &full);
            }
            ap_draw_image(splash, x, y, draw_w, draw_h);
            ap_present();
            int waited = 0;
            while (waited < 1000) {
                ap_input_event ev;
                while (ap_poll_input(&ev)) {
                    if (ev.pressed && !ev.repeated) waited = 1000;
                }
                SDL_Delay(16);
                waited += 16;
            }
            SDL_DestroyTexture(splash);
        }
    }

    if (load_games() != 0 || game_count == 0) {
        pakkit_message(
            game_count == 0 ? "No tracked games found." : "Could not open game database.",
            "OK"
        );
        ap_quit();
        return 0;
    }

    /* Load default thumbnail for games without art */
    {
        char def_path[FUHGED_MAX_PATH];
        if (pak_dir)
            snprintf(def_path, sizeof(def_path), "%s/res/default_thumb.png", pak_dir);
        else
            snprintf(def_path, sizeof(def_path), "res/default_thumb.png");
        int thumb_size = AP_DS(42);
        default_thumb = load_rounded_image(def_path, thumb_size, thumb_size / 2);
    }

    load_thumbnails();

    sort_games();

    int cursor = 0;
    for (;;) {
        pakkit_list_item items[MAX_GAMES];
        char sublabels[MAX_GAMES][128];
        for (int i = 0; i < game_count; i++) {
            char time_str[32];
            format_time(time_str, sizeof(time_str), games[i].play_time_total);
            snprintf(sublabels[i], sizeof(sublabels[i]), "%s  |  %d plays",
                     time_str, games[i].play_count);
            items[i].label = games[i].name;
            items[i].sublabel = sublabels[i];
            items[i].thumbnail = games[i].thumbnail ? games[i].thumbnail : default_thumb;
        }

        char title[64];
        snprintf(title, sizeof(title), "Fuhgeddaboudit - %s", sort_label(current_sort));

        pakkit_hint hints[] = {
            {"B", "QUIT"},
            {"Y", "SORT"},
            {"X", sort_reversed ? "DESC" : "ASC"},
            {"\xe2\x88\xb4 MERGE", ""},
            {"A", "DELETE"},
        };

        pakkit_list_opts opts = {
            .title = title,
            .hints = hints,
            .hint_count = 5,
            .secondary_button = AP_BTN_Y,
            .tertiary_button = AP_BTN_X,
            .quaternary_button = AP_BTN_MENU,
            .initial_index = cursor,
        };

        pakkit_list_result result;
        pakkit_list(&opts, items, game_count, &result);

        if (result.action == PAKKIT_ACTION_BACK)
            break;

        if (result.action == PAKKIT_ACTION_SECONDARY) {
            current_sort = (current_sort + 1) % SORT_COUNT;
            sort_games();
            cursor = 0;
            continue;
        }

        if (result.action == PAKKIT_ACTION_TERTIARY) {
            sort_reversed = !sort_reversed;
            sort_games();
            cursor = 0;
            continue;
        }

        if (result.action == PAKKIT_ACTION_QUATERNARY) {
            find_duplicates();
            if (dup_group_count == 0) {
                pakkit_message("No duplicate entries found.", "OK");
                continue;
            }

            pakkit_list_item dup_items[MAX_GAMES];
            char dup_subs[MAX_GAMES][64];
            for (int i = 0; i < dup_group_count; i++) {
                snprintf(dup_subs[i], sizeof(dup_subs[i]), "%d entries", dup_groups[i].count);
                dup_items[i].label = dup_groups[i].name;
                dup_items[i].sublabel = dup_subs[i];
                dup_items[i].thumbnail = default_thumb;
            }

            pakkit_hint dup_hints[] = {
                {"B", "BACK"},
                {"A", "SELECT"},
            };
            pakkit_list_opts dup_opts = {
                .title = "Merge Duplicates",
                .hints = dup_hints,
                .hint_count = 2,
                .initial_index = 0,
            };
            pakkit_list_result dup_result;
            pakkit_list(&dup_opts, dup_items, dup_group_count, &dup_result);

            if (dup_result.action == PAKKIT_ACTION_BACK)
                continue;

            int gi = dup_result.selected_index;
            load_dup_entries(dup_groups[gi].name);
            load_dup_thumbnails();

            pakkit_list_item entry_items[MAX_GAMES];
            char entry_subs[MAX_GAMES][128];
            for (int i = 0; i < dup_entry_count; i++) {
                char ts[32];
                format_time(ts, sizeof(ts), dup_entries[i].play_time_total);
                snprintf(entry_subs[i], sizeof(entry_subs[i]), "%s  |  %d plays",
                         ts, dup_entries[i].play_count);
                entry_items[i].label = dup_entries[i].file_path;
                entry_items[i].sublabel = entry_subs[i];
                entry_items[i].thumbnail = dup_entries[i].thumbnail ? dup_entries[i].thumbnail : default_thumb;
            }

            pakkit_hint entry_hints[] = {
                {"B", "BACK"},
                {"A", "KEEP"},
            };
            pakkit_list_opts entry_opts = {
                .title = "Merge To Which Entry?",
                .hints = entry_hints,
                .hint_count = 2,
                .initial_index = 0,
            };
            pakkit_list_result entry_result;
            pakkit_list(&entry_opts, entry_items, dup_entry_count, &entry_result);

            if (entry_result.action == PAKKIT_ACTION_BACK) {
                free_dup_thumbnails();
                continue;
            }

            int keep_idx = entry_result.selected_index;
            int keep_id = dup_entries[keep_idx].rom_id;

            char merge_msg[384];
            snprintf(merge_msg, sizeof(merge_msg),
                     "Merge %d entries into:\n\n%s\n\nAll play stats will be combined.",
                     dup_entry_count, dup_entries[keep_idx].file_path);

            if (pakkit_confirm(merge_msg, "MERGE", "NAH")) {
                free_dup_thumbnails();
                merge_into(keep_id, dup_groups[gi].name);
                free_thumbnails();
                load_games();
                if (game_count == 0) {
                    pakkit_message("All clean! Nothing to see here.", "OK");
                    break;
                }
                load_thumbnails();
                sort_games();
                cursor = 0;
            } else {
                free_dup_thumbnails();
            }
            continue;
        }

        cursor = result.selected_index;
        game_entry *g = &games[cursor];

        char confirm_msg[384];
        char time_str[32];
        format_time(time_str, sizeof(time_str), g->play_time_total);
        snprintf(confirm_msg, sizeof(confirm_msg),
                 "You can't undo this.\nYou sure you wanna fuhgeddaboudit?\n\n%s\n%s across %d plays",
                 g->name, time_str, g->play_count);

        if (pakkit_confirm(confirm_msg, "FUHGEDDABOUDIT", "NAH")) {
            delete_game(g->rom_id);
            free_thumbnails();
            load_games();

            if (game_count == 0) {
                pakkit_message("All clean! Nothing to see here.", "OK");
                break;
            }

            load_thumbnails();
            sort_games();
            if (cursor >= game_count)
                cursor = game_count - 1;
        }
    }

    free_thumbnails();
    if (default_thumb) SDL_DestroyTexture(default_thumb);
    ap_quit();
    return 0;
}
