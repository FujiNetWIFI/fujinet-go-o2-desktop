/*
 * The Preferences dialog: a programmatic AdwPreferencesDialog, no .ui file.
 *
 * The analog switch, the volume and the aspect apply LIVE. Everything else
 * is read when the machine powers up: whether the imported BIOS runs is the
 * console you would switch off to change, and the host options (FujiNet,
 * audio, gamepads) are what runs beside it. Those mark the dialog dirty, and
 * closing it power-cycles the session once (o2session_restart, which
 * restarts FujiNet and the devices only if one of their switches changed) --
 * an opened cartridge boots again.
 *
 * The BIOS is never needed for CONFIG or a FujiNet app (the app's own
 * stand-in boots them), only for games: the Machine group says which BIOS is
 * imported, offers Import BIOS, and has the switch that runs the stand-in
 * even when one is.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "prefs.h"

#include "fujilog.h"
#include "window.h"

#include <string.h>

typedef struct {
    O2Window *window;
    o2session *session;
    void (*restart)(O2Window *window);
    gboolean dirty;
    AdwDialog *dialog;
    GtkWidget *pad_group;
    GtkWidget *pad_rows[8];
    int pad_row_count;
    unsigned pad_generation;
    guint pad_timer;

    GtkWidget *bios_row;        /* the imported BIOS, as its subtitle */
} PrefsState;

/* What a row does when it changes. */
enum {
    ROW_RESTART = -1,   /* read at power-up: power-cycle on close */
    ROW_ANALOG = -2,    /* live */
    ROW_ASPECT = -3,    /* live */
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

/* ---- the BIOS ------------------------------------------------------------- */

/* Which of MAME's images make up the system ROM now, by the table's order:
 * the 2K set, the 1K system ROM, ic11 and ic12. */
enum { BIOS_SET = 0, BIOS_SYSTEM = 1, BIOS_IC11 = 2, BIOS_IC12 = 3 };

static void refresh_bios_row(PrefsState *state)
{
    o2session *s = state->session;
    const char *text;

    if (o2session_bios_available(s, BIOS_SET))
        text = "Imported (2K set)";
    else if (o2session_bios_available(s, BIOS_SYSTEM))
        text = "Imported (1K system ROM)";
    else if (o2session_have_bios(s))
        text = "Imported (MAME's ic11 and ic12)";
    else if (o2session_bios_available(s, BIOS_IC11))
        text = "Half imported \xe2\x80\x94 ic11 is here; import 84933.ic12 too";
    else if (o2session_bios_available(s, BIOS_IC12))
        text = "Half imported \xe2\x80\x94 ic12 is here; import 84932.ic11 too";
    else
        text = "Not imported \xe2\x80\x94 CONFIG and FujiNet apps run without it; "
               "games need it";
    adw_action_row_set_subtitle(ADW_ACTION_ROW(state->bios_row), text);
}

static void on_bios_file(GObject *src, GAsyncResult *res, gpointer user_data)
{
    AdwDialog *dialog = user_data;
    PrefsState *state = g_object_get_data(G_OBJECT(dialog), "prefs-state");
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;

    /* the dialog may have closed while the file chooser was up */
    if (state && file && (path = g_file_get_path(file)) != NULL) {
        const int before = o2session_have_bios(state->session);
        if (o2_show_bios_import(GTK_WIDGET(dialog), state->session, path) >= 0) {
            /* the console's system ROM just became whole: closing powers
             * up on it */
            if (!before && o2session_have_bios(state->session) &&
                o2session_get_int(state->session, "use_bios", 1))
                state->dirty = TRUE;
            refresh_bios_row(state);
        }
    }
    g_object_unref(dialog);
}

static void on_import_bios(GtkButton *b, gpointer user_data)
{
    PrefsState *state = user_data;
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *images = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    static const char *const exts[] = { "bin", "rom", "ic11", "ic12", "ic13", "ic14", NULL };
    int i;
    (void)b;

    gtk_file_filter_set_name(images,
        "Magnavox Odyssey 2 BIOS images (*.bin, *.rom, *.ic11 \xe2\x80\x93 *.ic14)");
    for (i = 0; exts[i]; i++)
        gtk_file_filter_add_suffix(images, exts[i]);
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, images);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, "Import BIOS");
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    gtk_file_dialog_open(dlg, GTK_WINDOW(state->window), NULL, on_bios_file,
                         g_object_ref(state->dialog));
    g_object_unref(images);
    g_object_unref(all);
    g_object_unref(filters);
    g_object_unref(dlg);
}

/* ---- generic rows ------------------------------------------------------------ */

static void combo_changed(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    RowBinding *b = user_data;
    int sel = (int)adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    (void)pspec;
    if (b->kind == ROW_ASPECT) {
        o2session_set_int(b->state->session, b->key, sel);
        o2_window_apply_aspect(b->state->window, sel);
        return;
    }
    if (o2session_get_int(b->state->session, b->key, b->def) == sel)
        return;
    o2session_set_int(b->state->session, b->key, sel);
    b->state->dirty = TRUE;
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
                             int live)
{
    GtkWidget *row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row),
                              o2session_get_int(state->session, key, def));
    g_signal_connect_data(row, "notify::active", G_CALLBACK(switch_changed),
                          binding_new(state, key, def, live ? ROW_ANALOG : ROW_RESTART),
                          binding_free, 0);
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

static GtkWidget *info_row(const char *title, const char *subtitle)
{
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    if (subtitle)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    return row;
}

/* ---- gamepads: one row per pad, with the keypad it drives ------------------ */

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
    static const char *const choices[] = { "Automatic", "Keypad A", "Keypad B", NULL };
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
    for (i = 0; i < n && i < 8; i++) {
        GtkWidget *row = adw_combo_row_new();
        GtkStringList *model = gtk_string_list_new(choices);
        int eff = o2session_gamepad_effective_port(state->session, i);
        o2session_gamepad_name(state->session, i, name, sizeof name);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        if (eff >= 0)
            g_snprintf(sub, sizeof sub, "Driving keypad %c", 'A' + eff);
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
    g_object_set_data(G_OBJECT(dialog), "prefs-state", NULL);
    if (state->pad_timer) g_source_remove(state->pad_timer);
    if (state->dirty && state->restart)
        state->restart(state->window);
    g_free(state);
}

void o2_prefs_show(O2Window *parent, o2session *session,
                    void (*restart)(O2Window *parent))
{
    PrefsState *state = g_new0(PrefsState, 1);
    AdwPreferencesDialog *dialog = ADW_PREFERENCES_DIALOG(adw_preferences_dialog_new());
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    AdwPreferencesGroup *machine = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *display = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *audio = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *input = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *pads = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    AdwPreferencesGroup *fujinet = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    GtkWidget *import, *external, *addr;

    state->window = parent;
    state->session = session;
    state->restart = restart;
    state->dialog = ADW_DIALOG(dialog);
    state->pad_group = GTK_WIDGET(pads);
    g_object_set_data(G_OBJECT(dialog), "prefs-state", state);

    adw_dialog_set_title(ADW_DIALOG(dialog), "Preferences");
    adw_dialog_set_content_width(ADW_DIALOG(dialog), 600);
    adw_preferences_page_set_title(page, "Machine");
    adw_preferences_page_set_icon_name(page, "applications-games-symbolic");

    /* ---- the machine: applied by a power cycle when the dialog closes */
    adw_preferences_group_set_title(machine, "Machine");
    adw_preferences_group_set_description(machine,
        "Applied when Preferences closes, with a power cycle: an opened "
        "cartridge boots again");
    state->bios_row = info_row("BIOS", NULL);
    import = gtk_button_new_with_label("Import BIOS\xe2\x80\xa6");
    gtk_widget_set_valign(import, GTK_ALIGN_CENTER);
    g_signal_connect(import, "clicked", G_CALLBACK(on_import_bios), state);
    adw_action_row_add_suffix(ADW_ACTION_ROW(state->bios_row), import);
    adw_preferences_group_add(machine, state->bios_row);
    refresh_bios_row(state);
    adw_preferences_group_add(machine,
        switch_row(state, "Use the imported BIOS",
                   "Off: the app's own stand-in runs instead. It boots CONFIG and "
                   "FujiNet apps, not games", "use_bios", 1, 0));

    /* ---- the picture: live */
    adw_preferences_group_set_title(display, "Display");
    adw_preferences_group_add(display,
        combo_row(state, "Aspect", "The 100 x 246 picture's shape",
                  "aspect", 0, aspect_name, ROW_ASPECT));

    /* ---- audio */
    adw_preferences_group_set_title(audio, "Audio");
    adw_preferences_group_add(audio,
        switch_row(state, "Enable audio", "Open the system audio device "
                   "(applied when Preferences closes)", "enable_audio", 1, 0));
    {
        GtkWidget *row = adw_action_row_new();
        GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 5);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Volume");
        gtk_range_set_value(GTK_RANGE(scale), o2session_get_int(session, "volume", 100));
        gtk_widget_set_size_request(scale, 200, -1);
        gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);
        g_signal_connect(scale, "value-changed", G_CALLBACK(volume_changed), state);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), scale);
        adw_preferences_group_add(audio, row);
    }

    /* ---- input */
    adw_preferences_group_set_title(input, "Input");
    adw_preferences_group_add(input,
        switch_row(state, "Enable gamepads", "Poll USB/Bluetooth gamepads "
                   "(applied when Preferences closes)", "enable_gamepad", 1, 0));
    adw_preferences_group_add(input,
        switch_row(state, "Analog sticks steer too",
                   "The left stick presses 2 4 6 8 (and 1 3 7 9 on the diagonals) "
                   "as the D-pad does", "analog_joystick", 1, 1));

    adw_preferences_group_set_title(pads, "Gamepads");
    adw_preferences_group_set_description(pads,
        "Assigned to keypads in connection order unless chosen here");
    rebuild_pad_rows(state);
    state->pad_generation = o2session_gamepad_generation(session);

    /* ---- FujiNet */
    adw_preferences_group_set_title(fujinet, "FujiNet");
    adw_preferences_group_add(fujinet,
        switch_row(state, "Enable FujiNet",
                   "Run the in-process FujiNet the cartridge dials into. Off "
                   "means no network and a link-down CONFIG (applied when "
                   "Preferences closes).",
                   "enable_fujinet", 1, 0));
    external = switch_row(state, "Use external FujiNet",
                          "Skip the built-in FujiNet and dial another instance "
                          "instead. Set that instance's BoIP listener to this "
                          "address (applied when Preferences closes).",
                          "fujinet_external", 0, 0);
    addr = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(addr),
                                  "Address:port, e.g. " EXTERNAL_PLACEHOLDER);
    gtk_editable_set_text(GTK_EDITABLE(addr),
                          o2session_get_str(session, "fujinet_external_addr", ""));
    g_signal_connect(addr, "changed", G_CALLBACK(addr_changed), state);
    gtk_widget_set_sensitive(addr, adw_switch_row_get_active(ADW_SWITCH_ROW(external)));
    g_signal_connect(external, "notify::active", G_CALLBACK(external_toggled), addr);
    adw_preferences_group_add(fujinet, external);
    adw_preferences_group_add(fujinet, addr);
    {
        GtkWidget *row = info_row("Web UI",
                                  o2session_fujinet_external(session)
                                      ? "FujiNet is external"
                                      : o2session_fujinet_webui_url(session));
        GtkWidget *open = gtk_button_new_with_label("Open");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_webui), state);
        adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);
        adw_preferences_group_add(fujinet, row);
    }

    adw_preferences_page_add(page, machine);
    adw_preferences_page_add(page, display);
    adw_preferences_page_add(page, audio);
    adw_preferences_page_add(page, input);
    adw_preferences_page_add(page, pads);
    adw_preferences_page_add(page, fujinet);
    adw_preferences_dialog_add(dialog, page);
    g_signal_connect(dialog, "closed", G_CALLBACK(prefs_closed), state);
    state->pad_timer = g_timeout_add(1000, pad_tick, state);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(parent));
}
