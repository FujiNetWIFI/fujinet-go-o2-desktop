/*
 * The Preferences dialog: a programmatic AdwPreferencesDialog, no .ui file.
 *
 * The analog switch, the volume, the aspect and smooth scaling apply LIVE.
 * Everything else is read when the machine powers up: which console it is,
 * whether the imported BIOS runs and whether The Voice is in front of the
 * cartridge are the console you would switch off to change, and the host
 * options (FujiNet, audio, gamepads) are what runs beside it. Those mark the
 * dialog dirty, and closing it power-cycles the session once
 * (o2session_restart, which restarts FujiNet and the devices only if one of
 * their switches changed) -- an opened cartridge boots again.
 *
 * Nothing the console needs from Magnavox or Philips is shipped: the ROMs
 * page lists every ROM image the app takes, imported or not, and imports
 * them from files, a zip or a folder, each recognised by its checksums. CONFIG
 * and the FujiNet apps never need them (the app's own stand-in boots them);
 * games need the BIOS, and every machine wants its video chip's character
 * set to show text.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "fujilog.h"
#include "window.h"
#include "help/help_window.h"

#include <string.h>

#define MAX_ROM_ROWS 16
#define MAX_PAD_ROWS 8

typedef struct {
    O2Window *window;
    o2session *session;
    void (*restart)(O2Window *window);
    gboolean dirty;
    gboolean syncing;           /* a row is being set back: not a change */
    AdwDialog *dialog;
    GtkWidget *pad_group;
    GtkWidget *pad_rows[MAX_PAD_ROWS + 1];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;

    GtkWidget *bios_row;        /* what the chosen machine has imported, as its subtitle */
    GtkWidget *rom_rows[MAX_ROM_ROWS];
    GtkWidget *rom_state[MAX_ROM_ROWS];
} PrefsState;

/* What a row does when it changes. */
enum {
    ROW_RESTART = -1,   /* read at power-up: power-cycle on close */
    ROW_ANALOG = -2,    /* live */
    ROW_ASPECT = -3,    /* live */
    ROW_SMOOTH = -4,    /* live */
    ROW_MACHINE = -5,   /* read at power-up, and says what it needs */
};

typedef struct {
    PrefsState *state;
    const char *key;
    int def;
    int kind;
} RowBinding;

static void binding_free(gpointer p, GClosure *closure)
{
    (void)closure;
    g_free(p);
}

static RowBinding *binding_new(PrefsState *state, const char *key, int def, int kind)
{
    RowBinding *b = g_new0(RowBinding, 1);
    b->state = state;
    b->key = key;
    b->def = def;
    b->kind = kind;
    return b;
}

static const char *aspect_name(int i)
{
    static const char *const names[] = { "Television (4:3)", "Square pixels", NULL };
    return (i >= 0 && i < 2) ? names[i] : NULL;
}

static const char *machine_name(int i)
{
    return i >= 0 && i < O2_MACHINE_COUNT ? o2_machine_name(i) : NULL;
}

static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

/* ---- the ROMs ------------------------------------------------------------- */

static int machine_now(PrefsState *state)
{
    int m = o2session_get_int(state->session, "machine", O2_MACHINE_ODYSSEY2);
    return m >= 0 && m < O2_MACHINE_COUNT ? m : O2_MACHINE_ODYSSEY2;
}

/* What the chosen machine has of what it wants, as the BIOS row's subtitle. */
static void refresh_bios_row(PrefsState *state)
{
    o2session *s = state->session;
    const int m = machine_now(state);
    char text[300];

    g_snprintf(text, sizeof text, "BIOS %s, character set %s. CONFIG and FujiNet "
               "apps run without any; games need the BIOS, text needs the character set.",
               o2session_have_bios(s, m) ? "imported" : "not imported",
               o2session_have_font(s, m) ? "imported" : "not imported");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(state->bios_row), text);
}

static void refresh_roms(PrefsState *state)
{
    int i;
    for (i = 0; i < o2session_rom_item_count() && i < MAX_ROM_ROWS; i++) {
        gboolean have = o2session_rom_item_available(state->session, i) != 0;
        if (!state->rom_rows[i]) continue;
        adw_action_row_set_subtitle(ADW_ACTION_ROW(state->rom_rows[i]),
                                    have ? "Imported" : "Not imported");
        gtk_widget_remove_css_class(state->rom_state[i], have ? "dim-label" : "o2-accent-text");
        gtk_widget_add_css_class(state->rom_state[i], have ? "o2-accent-text" : "dim-label");
        gtk_image_set_from_icon_name(GTK_IMAGE(state->rom_state[i]),
                                     have ? "emblem-ok-symbolic" : "action-unavailable-symbolic");
    }
    refresh_bios_row(state);
}

/* An import from Preferences does not power-cycle at once: closing the dialog
 * does, with everything else changed here. */
static void on_import_done(int imported, gpointer user_data)
{
    AdwDialog *dialog = user_data;
    /* Preferences may have closed while the file chooser was up */
    PrefsState *state = g_object_get_data(G_OBJECT(dialog), "prefs-state");
    if (state) {
        if (imported > 0)
            state->dirty = TRUE;
        refresh_roms(state);
    }
    g_object_unref(dialog);
}

static void on_import_clicked(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    const gboolean folder = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "o2-folder")) != 0;
    o2_window_import_bios(state->window, GTK_WIDGET(state->dialog), folder, FALSE, NULL,
                          on_import_done, g_object_ref(state->dialog));
}

static void on_open_roms(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    g_autoptr(GFile) dir = g_file_new_for_path(o2session_roms_path(state->session));
    GtkFileLauncher *l = gtk_file_launcher_new(dir);
    (void)b;
    gtk_file_launcher_launch(l, GTK_WINDOW(state->window), NULL, NULL, NULL);
    g_object_unref(l);
}

static AdwPreferencesPage *build_roms_page(PrefsState *state)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *group = NULL;
    const char *last = NULL;
    GtkWidget *box, *files, *folder, *row, *open;
    int i;

    adw_preferences_page_set_title(page, "ROMs");
    adw_preferences_page_set_name(page, "ROMs");
    adw_preferences_page_set_icon_name(page, "media-flash-symbolic");

    /* the buttons and the folder, first */
    group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Import");
    adw_preferences_group_set_header_suffix(group, o2_help_button("prefs/ROMs"));
    row = info_row("Import ROMs",
        "Your own copies: single files, a zip (MAME's videopac.zip, i8244.zip or "
        "videopacp.zip work) or a folder. Each is recognised by its size and "
        "checksum, whatever it is called.");
    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    files = gtk_button_new_with_label("Import BIOS\xe2\x80\xa6");
    folder = gtk_button_new_with_label("Import from a Folder\xe2\x80\xa6");
    gtk_widget_add_css_class(files, "suggested-action");
    g_object_set_data(G_OBJECT(folder), "o2-folder", GINT_TO_POINTER(1));
    g_signal_connect(files, "clicked", G_CALLBACK(on_import_clicked), state);
    g_signal_connect(folder, "clicked", G_CALLBACK(on_import_clicked), state);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), files);
    gtk_box_append(GTK_BOX(box), folder);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), box);
    adw_preferences_group_add(group, row);
    row = info_row("ROM folder", o2session_roms_path(state->session));
    adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
    open = gtk_button_new_with_label("Open");
    gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_roms), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
    adw_preferences_group_add(group, row);
    adw_preferences_page_add(page, group);
    group = NULL;

    for (i = 0; i < o2session_rom_item_count() && i < MAX_ROM_ROWS; i++) {
        const o2_rom_item *item = o2session_rom_item(i);
        GtkWidget *mark, *detail;
        char info[64];
        if (!item) continue;
        if (!last || strcmp(last, item->group) != 0) {
            group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
            adw_preferences_group_set_title(group, item->group);
            adw_preferences_page_add(page, group);
            last = item->group;
        }
        row = adw_action_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), item->name);
        g_snprintf(info, sizeof info, "%u bytes   CRC-32 %08X", item->size, item->crc);
        detail = gtk_label_new(info);
        gtk_widget_add_css_class(detail, "dim-label");
        gtk_widget_add_css_class(detail, "caption");
        gtk_widget_set_valign(detail, GTK_ALIGN_CENTER);
        mark = gtk_image_new();
        adw_action_row_add_prefix(ADW_ACTION_ROW(row), mark);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), detail);
        adw_preferences_group_add(group, row);
        state->rom_rows[i] = row;
        state->rom_state[i] = mark;
    }
    refresh_roms(state);
    return page;
}

/* ---- generic rows ------------------------------------------------------------ */

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (b->state->syncing)
        return;
    if (b->kind == ROW_MACHINE && sel == O2_MACHINE_VIDEOPAC_PLUS &&
        !o2session_have_bios(b->state->session, sel)) {
        /* it would not start: say why and keep the console it was */
        adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(b->state->dialog),
            adw_toast_new("The Videopac+ G7400 needs its BIOS: import it first (ROMs page)"));
        b->state->syncing = TRUE;
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
            (guint)o2session_get_int(b->state->session, b->key, b->def));
        b->state->syncing = FALSE;
        return;
    }
    if (b->kind == ROW_ASPECT) {
        o2session_set_int(b->state->session, b->key, sel);
        o2_window_apply_aspect(b->state->window, sel);
        return;
    }
    if (o2session_get_int(b->state->session, b->key, b->def) == sel)
        return;
    o2session_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
    if (b->kind == ROW_MACHINE)
        refresh_bios_row(b->state);
}

static void analog_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)row; (void)pspec;
    o2session_set_analog(state->session,
        o2session_get_int(state->session, "analog_joystick", 1));
}

static void switch_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int on = adw_switch_row_get_active(ADW_SWITCH_ROW(row)) ? 1 : 0;
    (void)pspec;
    if (o2session_get_int(b->state->session, b->key, b->def) == on)
        return;
    o2session_set_int(b->state->session, b->key, on);
    if (b->kind == ROW_ANALOG)          /* an analog switch: live */
        analog_changed(row, NULL, b->state);
    else if (b->kind == ROW_SMOOTH)
        o2_window_apply_smooth(b->state->window, on != 0);
    else
        b->state->dirty = TRUE;
}

static void volume_changed(GtkRange *range, gpointer user_data)
{
    PrefsState *state = user_data;
    o2session_set_volume(state->session, (int)gtk_range_get_value(range));
}

static GtkWidget *combo_row(PrefsState *state, const char *title,
                            const char *subtitle, const char *key, int def,
                            const char *(*name_fn)(int), int kind)
{
    GtkWidget *row = adw_combo_row_new();
    GtkStringList *model = gtk_string_list_new(NULL);
    int i;

    for (i = 0; name_fn(i); i++)
        gtk_string_list_append(model, name_fn(i));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
        (guint)o2session_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::selected", G_CALLBACK(combo_changed),
                          binding_new(state, key, def, kind), binding_free, 0);
    g_object_unref(model);
    return row;
}

static GtkWidget *switch_row(PrefsState *state, const char *title,
                             const char *subtitle, const char *key, int def,
                             int kind)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              o2session_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          binding_new(state, key, def, kind), binding_free, 0);
    return row;
}

#define EXTERNAL_PLACEHOLDER "127.0.0.1:11520"

/* The address field: valid text (or none, meaning 127.0.0.1:11520) is saved
 * and marks the machine dirty; anything else is flagged and not saved. */
static void addr_changed(GtkEditable *entry, gpointer user_data)
{
    PrefsState *state = user_data;
    const char *text = gtk_editable_get_text(entry);
    char host[64];
    int port;

    if (o2session_parse_hostport(text, host, sizeof host, &port) != 0) {
        gtk_widget_add_css_class(GTK_WIDGET(entry), "error");
        return;
    }
    gtk_widget_remove_css_class(GTK_WIDGET(entry), "error");
    if (g_strcmp0(o2session_get_str(state->session, "fujinet_external_addr", ""),
                  text) == 0)
        return;
    o2session_set_str(state->session, "fujinet_external_addr", text);
    state->dirty = TRUE;
}

static void external_toggled(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    gtk_widget_set_sensitive(GTK_WIDGET(user_data),
                             adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    (void)pspec;
}

/* ---- gamepads: one row per pad, with the stick it drives ------------------ */

static void pad_assign_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    PrefsState *state = user_data;
    int idx = GPOINTER_TO_INT(g_object_get_data(row, "pad-index"));
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    o2session_gamepad_assign(state->session, idx, sel - 1);
}

static void rebuild_pad_rows(PrefsState *state)
{
    static const char *const choices[] = { "Automatic", "Stick 1", "Stick 2", NULL };
    int i, n = o2session_gamepad_count(state->session);
    char name[128], sub[64];

    for (i = 0; i < state->pad_row_count; i++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(state->pad_group), state->pad_rows[i]);
    state->pad_row_count = 0;

    if (n == 0) {
        GtkWidget *row = info_row("No gamepads connected",
                                  "Plug one in: it is picked up as it appears");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        return;
    }
    for (i = 0; i < n && i < MAX_PAD_ROWS; i++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        int eff = o2session_gamepad_effective_port(state->session, i);
        o2session_gamepad_name(state->session, i, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        if (eff >= 0)
            g_snprintf(sub, sizeof sub, "Driving stick %d", eff + 1);
        else
            g_snprintf(sub, sizeof sub, "Driving nothing");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
        adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(model));
        adw_combo_row_set_selected(ADW_COMBO_ROW(row),
            (guint)(o2session_gamepad_assignment(state->session, i) + 1));
        g_object_set_data(G_OBJECT(row), "pad-index", GINT_TO_POINTER(i));
        g_signal_connect(row, "notify::selected", G_CALLBACK(pad_assign_changed), state);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(state->pad_group), row);
        state->pad_rows[state->pad_row_count++] = row;
        g_object_unref(model);
    }
}

static gboolean pad_tick(gpointer user_data)
{
    PrefsState *state = user_data;
    unsigned gen = o2session_gamepad_generation(state->session);
    if (gen != state->pad_generation) {
        state->pad_generation = gen;
        rebuild_pad_rows(state);
    }
    return G_SOURCE_CONTINUE;
}

/* ---- FujiNet ------------------------------------------------------------------ */

static void on_open_webui(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    (void)b;
    if (!o2session_fujinet_running(state->session)) {
        adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(state->dialog),
                                         adw_toast_new(o2session_fujinet_external(state->session)
                                                       ? "FujiNet is external: use its own web UI"
                                                       : "FujiNet is not running"));
        return;
    }
    o2_fujiconfig_show(GTK_WINDOW(state->window), state->session);
}

/* ---- the dialog -------------------------------------------------------------- */

static void prefs_closed(AdwDialog *dialog, gpointer user_data)
{
    PrefsState *state = user_data;
    /* an import's file chooser can still be up: it finds no state */
    g_object_set_data(G_OBJECT(dialog), "prefs-state", NULL);
    if (state->pad_timer) g_source_remove(state->pad_timer);
    state->pad_timer = 0;
    o2session_settings_flush(state->session);
    if (state->dirty && state->restart)
        state->restart(state->window);
    g_free(state);
}

static AdwPreferencesPage *new_page(const char *title, const char *name, const char *icon)
{
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, title);
    adw_preferences_page_set_name(page, name);
    adw_preferences_page_set_icon_name(page, icon);
    return page;
}

static AdwPreferencesGroup *new_group(AdwPreferencesPage *page, const char *title,
                                      const char *description)
{
    AdwPreferencesGroup *g = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(g, title);
    if (description)
        adw_preferences_group_set_description(g, description);
    /* the first group on a page carries the page's Help button */
    if (!g_object_get_data(G_OBJECT(page), "o2-help-done")) {
        char ctx[64];
        g_snprintf(ctx, sizeof ctx, "prefs/%s", adw_preferences_page_get_name(page));
        adw_preferences_group_set_header_suffix(g, o2_help_button(ctx));
        g_object_set_data(G_OBJECT(page), "o2-help-done", GINT_TO_POINTER(1));
    }
    adw_preferences_page_add(page, g);
    return g;
}

void o2_prefs_show(O2Window *parent, o2session *session,
                    void (*restart)(O2Window *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *machine = new_page("Machine", "Machine", "applications-games-symbolic");
    AdwPreferencesPage *video = new_page("Display & Audio", "Display", "video-display-symbolic");
    AdwPreferencesPage *input = new_page("Input", "Input", "input-gaming-symbolic");
    AdwPreferencesPage *net = new_page("FujiNet", "FujiNet", "network-wireless-symbolic");
    AdwPreferencesGroup *group, *external_group;
    GtkWidget *external, *addr, *row;

    state->window = parent;
    state->session = session;
    state->restart = restart;
    state->dialog = ADW_DIALOG(dialog);
    g_object_set_data(G_OBJECT(dialog), "prefs-state", state);

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 640);

    /* ---- the console: applied by a power cycle when the dialog closes */
    group = new_group(machine, "Console",
        "Applied when Preferences closes, with a power cycle: an opened "
        "cartridge boots again. The Odyssey 2 is NTSC with the 8244; the Videopac "
        "G7000 is PAL with the 8245 on the same BIOS; the Videopac+ G7400 adds a "
        "text chip, its own BIOS and a bigger keyboard.");
    adw_preferences_group_add(group,
        combo_row(state, "Machine", NULL,
                  "machine", O2_MACHINE_ODYSSEY2, machine_name, ROW_MACHINE));
    state->bios_row = info_row("BIOS", NULL);
    adw_preferences_group_add(group, state->bios_row);
    adw_preferences_group_add(group,
        switch_row(state, "Use the imported BIOS",
                   "Off: the app's own stand-in runs instead. It boots CONFIG and "
                   "FujiNet apps, not games", "use_bios", 1, ROW_RESTART));
    adw_preferences_group_add(group,
        switch_row(state, "The Voice in front of the cartridge",
                   "The speech module, as plugged into the console's slot. It needs "
                   "its two speech ROMs (the ROMs page)", "voice", 0, ROW_RESTART));
    adw_preferences_dialog_add(dialog, machine);
    adw_preferences_dialog_add(dialog, build_roms_page(state));

    /* ---- the picture: live */
    group = new_group(video, "Display", NULL);
    adw_preferences_group_add(group,
        combo_row(state, "Aspect", "The 360 x 243 picture's shape",
                  "aspect", 0, aspect_name, ROW_ASPECT));
    adw_preferences_group_add(group,
        switch_row(state, "Smooth scaling", "Blend the pixels when the picture is "
                   "scaled, instead of keeping them sharp", "smooth", 0, ROW_SMOOTH));

    /* ---- audio */
    group = new_group(video, "Audio", NULL);
    adw_preferences_group_add(group,
        switch_row(state, "Enable audio", "Open the system audio device "
                   "(applied when Preferences closes)", "enable_audio", 1, ROW_RESTART));
    row = adw_action_row_new();
    {
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Volume");
        gtk_range_set_value(GTK_RANGE(scale), o2session_get_int(session, "volume", 100));
        gtk_widget_set_size_request(scale, 200, -1);
        gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
        g_signal_connect(scale, "value-changed", G_CALLBACK(volume_changed), state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);
        adw_preferences_group_add(group, row);
    }
    adw_preferences_dialog_add(dialog, video);

    /* ---- input */
    group = new_group(input, "Input", NULL);
    adw_preferences_group_add(group,
        switch_row(state, "Enable gamepads", "Poll USB/Bluetooth gamepads "
                   "(applied when Preferences closes)", "enable_gamepad", 1, ROW_RESTART));
    adw_preferences_group_add(group,
        switch_row(state, "Analog sticks steer too",
                   "The left stick presses the joystick's directions as the D-pad does",
                   "analog_joystick", 1, ROW_ANALOG));

    group = new_group(input, "Gamepads",
        "Assigned to the console's two sticks in connection order unless chosen here. "
        "Rebind keys and buttons in the Controllers window (F9).");
    state->pad_group = GTK_WIDGET(group);
    rebuild_pad_rows(state);
    state->pad_generation = o2session_gamepad_generation(session);
    adw_preferences_dialog_add(dialog, input);

    /* ---- FujiNet */
    external_group = new_group(net, "FujiNet", NULL);
    adw_preferences_group_add(external_group,
        switch_row(state, "Enable FujiNet",
                   "Run the in-process FujiNet the cartridge dials into. Off "
                   "means no network and a link-down CONFIG (applied when "
                   "Preferences closes).",
                   "enable_fujinet", 1, ROW_RESTART));
    external = switch_row(state, "Use external FujiNet",
                          "Skip the built-in FujiNet and dial another instance "
                          "instead. Set that instance's BoIP listener to this "
                          "address (applied when Preferences closes).",
                          "fujinet_external", 0, ROW_RESTART);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "Address:port, e.g. " EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          o2session_get_str(session, "fujinet_external_addr", ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled), addr);
    adw_preferences_group_add(external_group, external);
    adw_preferences_group_add(external_group, addr);
    {
        GtkWidget *web = info_row("Web UI",
                                  o2session_fujinet_external(session)
                                      ? "FujiNet is external"
                                      : o2session_fujinet_webui_url(session));
        GtkWidget *open = gtk_button_new_with_label("Open");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_webui), state);
        adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(web), TRUE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(web), open);
        adw_preferences_group_add(external_group, web);
    }
    adw_preferences_dialog_add(dialog, net);

    refresh_bios_row(state);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
