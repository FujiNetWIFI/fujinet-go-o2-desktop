/*
 * window.c -- the main window: the display, a header-bar menu, keyboard
 * capture, the console's CLEAR button, the "needs the BIOS" alert, gamepad
 * hot-plug toasts and the FujiNet status.
 *
 * Keyboard events are translated by hardware keycode (evdev, via the
 * session's HID table) rather than by GDK keyval, so a binding names the
 * physical key whatever Shift is doing and whatever layout is active -- the
 * same path the Qt, Win32 and AppKit frontends take, which is what lets one
 * tested table serve all four.
 *
 * The console's own button is a machine key, not a menu accelerator: F3 is
 * CLEAR through the bindings (MAME's key), so it can be remapped and a
 * gamepad's Start drives it too. The menu items name whatever key is bound
 * to them now.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "window.h"

#include "display.h"
#include "fujilog.h"
#include "prefs.h"
#include "debugger/dbg_window.h"
#include "keypad/keypad_window.h"

#include <string.h>

struct _O2Window {
    AdwApplicationWindow parent_instance;

    o2session *session;
    GtkWidget *display;
    GtkWidget *toast_overlay;
    GtkWidget *status;          /* the FujiNet link indicator */
    GtkWidget *status_dot;
    GMenu *console_menu;        /* the Console section, relabelled live */
    char console_labels[256];   /* what it last showed */
    guint status_id;
    guint sysact_id;
    gboolean sysact_down[O2_SYSACT_COUNT];
    gboolean needs_bios_shown;  /* the alert is up for this stand-in park */
    unsigned pad_generation;
    gboolean fullscreen;
};

G_DEFINE_FINAL_TYPE(O2Window, o2_window, ADW_TYPE_APPLICATION_WINDOW)

void o2_window_toast(O2Window *self, const char *text)
{
    adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(self->toast_overlay),
                                adw_toast_new(text));
}

/* ---- the accent ------------------------------------------------------------ */

static GtkCssProvider *s_accent_css;

static void load_accent_css(gboolean dark)
{
    char buf[1024];
    /* The tan is pale: it fills with dark text everywhere, and as a text
     * colour it is only readable on a dark theme -- on a light one the
     * darker tan stands in. */
    g_snprintf(buf, sizeof buf,
        ".o2-accent { background: #%06x; color: #000000; }\n"
        ".o2-accent:hover { background: shade(#%06x, 1.08); }\n"
        ".o2-accent-text { color: #%06x; font-weight: bold; }\n"
        ".o2-pill { padding: 1px 8px; border-radius: 9px; font-weight: bold; }\n"
        ".o2-dot { border-radius: 6px; min-width: 12px; min-height: 12px; }\n"
        ".o2-dot-on { background: #%06x; }\n"
        ".o2-dot-off { background: #808080; }\n",
        O2SESSION_ACCENT_RGB, O2SESSION_ACCENT_RGB,
        dark ? O2SESSION_ACCENT_RGB : O2SESSION_ACCENT_DARK_RGB,
        O2SESSION_ACCENT_RGB);
    gtk_css_provider_load_from_string(s_accent_css, buf);
}

static void on_dark_changed(AdwStyleManager *sm, GParamSpec *ps, gpointer ud)
{
    (void)ps; (void)ud;
    load_accent_css(adw_style_manager_get_dark(sm));
}

void o2_install_accent_css(void)
{
    AdwStyleManager *sm = adw_style_manager_get_default();
    if (s_accent_css)
        return;
    s_accent_css = gtk_css_provider_new();
    load_accent_css(adw_style_manager_get_dark(sm));
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(s_accent_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_signal_connect(sm, "notify::dark", G_CALLBACK(on_dark_changed), NULL);
}

/* ---- input ---------------------------------------------------------------- */

static guint32 keysym_of(guint keyval, guint keycode)
{
    /* the hardware key first; a keyval only for keys evdev has no HID
     * usage for (rare: media keys) */
    guint32 k = keycode >= 8 ? o2session_keysym_from_evdev(keycode - 8) : 0;
    return k ? k : keyval;
}

void o2_window_run_sysaction(O2Window *self, int sa)
{
    switch (sa) {
    case O2_SYSACT_RESET_CONFIG:
        o2session_sysaction(self->session, sa);
        o2_window_toast(self, "Back to the FujiNet CONFIG client");
        break;
    case O2_SYSACT_DEBUG_STOP:
        /* the debugger window attaches, which stops the machine */
        o2_debugger_show(GTK_WINDOW(self), self->session);
        break;
    default:
        break;
    }
}

/* An open dialog (Preferences, About, an alert) or menu has the keyboard:
 * Escape closes it and the arrows and Return move through it, instead of
 * driving the machine. The key capture sits on the window, ahead of them. */
static gboolean ui_has_keyboard(O2Window *self)
{
    GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(self));
    if (adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(self)))
        return TRUE;
    return focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER) != NULL;
}

static gboolean on_key_pressed(GtkEventControllerKey *ctrl, guint keyval,
                               guint keycode, GdkModifierType state,
                               gpointer user_data)
{
    O2Window *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl;

    if (ui_has_keyboard(self))
        return FALSE;

    /* The window's own keys, deliberately not bindable: they are how you
     * reach the panels that do the binding. */
    if (keyval == GDK_KEY_F9) {
        o2_keypad_window_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F12) {
        o2_debugger_toggle(GTK_WINDOW(self), self->session);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        gtk_widget_activate_action(GTK_WIDGET(self), "win.fullscreen", NULL);
        return TRUE;
    }
    /* The menu accelerators are all Ctrl. Alt is not a modifier here: Left
     * Alt is keypad A's 0 (MAME's key), and a game may want it held while
     * another key is pressed. */
    if (state & GDK_CONTROL_MASK)
        return FALSE;

    keysym = keysym_of(keyval, keycode);
    /* A system action is checked BEFORE the machine keys, so Escape always
     * gets back to CONFIG whatever else the key table says. Leading edge
     * only: GTK4 has no repeat flag. */
    sa = o2session_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        if (!self->sysact_down[sa]) {
            self->sysact_down[sa] = TRUE;
            o2_window_run_sysaction(self, sa);
        }
        return TRUE;
    }
    return o2session_key(self->session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *ctrl, guint keyval,
                                guint keycode, GdkModifierType state,
                                gpointer user_data)
{
    O2Window *self = user_data;
    guint32 keysym;
    int sa;
    (void)ctrl; (void)state;

    if (keyval == GDK_KEY_F9 || keyval == GDK_KEY_F11 || keyval == GDK_KEY_F12)
        return TRUE;
    keysym = keysym_of(keyval, keycode);
    if (ui_has_keyboard(self)) {
        /* still let go of a machine key held into the dialog */
        o2session_key(self->session, keysym, 0);
        return FALSE;
    }
    sa = o2session_key_sysaction(self->session, keysym);
    if (sa >= 0) {
        self->sysact_down[sa] = FALSE;
        return TRUE;
    }
    return o2session_key(self->session, keysym, 0) ? TRUE : FALSE;
}

/* Losing focus with keys held would leave the machine believing they are
 * still down. */
static void on_focus_leave(GtkEventControllerFocus *ctrl, gpointer user_data)
{
    O2Window *self = user_data;
    (void)ctrl;
    o2session_release_all(self->session);
    memset(self->sysact_down, 0, sizeof self->sysact_down);
}

/* ---- the Console menu: labels that follow the bindings ----------------------- */

/* "CLEAR (F3)": the item's name and whatever key drives its target now (a
 * remap in the Keypad window shows here within a quarter second). The
 * mnemonics are unique across the whole menu. */
static void item_label(O2Window *self, char *dst, int dstsz, const char *name,
                       int target)
{
    o2_binding b = o2session_binding_get(self->session, target);
    char key[32];
    if (b.keysym && o2session_keysym_name(b.keysym, key, sizeof key) > 0)
        g_snprintf(dst, dstsz, "%s (%s)", name, key);
    else
        g_snprintf(dst, dstsz, "%s", name);
}

static void sync_console_menu(O2Window *self)
{
    char clear[96], config[96], all[256];

    item_label(self, clear, sizeof clear, "_CLEAR",
               O2_TARGET_SWITCH(O2_SW_CLEAR));
    item_label(self, config, sizeof config, "Reset to CO_NFIG",
               O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG));
    g_snprintf(all, sizeof all, "%s|%s", clear, config);

    if (strcmp(all, self->console_labels) == 0)
        return;
    g_strlcpy(self->console_labels, all, sizeof self->console_labels);
    g_menu_remove_all(self->console_menu);
    g_menu_append(self->console_menu, clear, "win.clear");
    g_menu_append(self->console_menu, config, "win.reset-config");
}

/* ---- the BIOS a game needs ------------------------------------------------------ */

static void open_cart_path(O2Window *self, const char *path);

/* A dialog for anything the user has to read: a toast would vanish before
 * the reason was read, and the reason is the whole point. */
static void show_message(GtkWidget *over, const char *heading, const char *body)
{
    AdwDialog *dlg = adw_alert_dialog_new(heading, body);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "ok", "OK");
    adw_dialog_present(dlg, over);
}

static GtkFileDialog *file_dialog(const char *title, const char *filter_name,
                                  const char *const *suffixes, const char *folder)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    GtkFileFilter *images = gtk_file_filter_new();
    GtkFileFilter *all = gtk_file_filter_new();
    int i;

    /* suffixes match case-insensitively: GAME.ST2 is as good as game.st2 */
    gtk_file_filter_set_name(images, filter_name);
    for (i = 0; suffixes[i]; i++)
        gtk_file_filter_add_suffix(images, suffixes[i]);
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    g_list_store_append(filters, images);
    g_list_store_append(filters, all);
    gtk_file_dialog_set_title(dlg, title);
    gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
    if (folder && *folder) {
        g_autoptr(GFile) dir = g_file_new_for_path(folder);
        gtk_file_dialog_set_initial_folder(dlg, dir);
    }
    g_object_unref(images);
    g_object_unref(all);
    g_object_unref(filters);
    return dlg;
}

/* The 2K set, the 1K system ROM, or MAME's four 512-byte ROMs. */
static GtkFileDialog *bios_dialog(void)
{
    static const char *const exts[] = { "bin", "rom", "ic11", "ic12", "ic13", "ic14", NULL };
    return file_dialog("Import BIOS",
        "Magnavox Odyssey 2 BIOS images (*.bin, *.rom, *.ic11 \xe2\x80\x93 *.ic14)", exts, NULL);
}

/* Import BIOS says what it did in every case -- that it boots at the next
 * power cycle, that ic11 still wants ic12, or that its CRC is not one MAME
 * knows -- and that is worth more than a toast's few seconds. */
int o2_show_bios_import(GtkWidget *over, o2session *session, const char *path)
{
    char msg[1024];
    int idx = o2session_import_bios(session, path, msg, sizeof msg);
    show_message(over, idx < 0 ? "Cannot Import BIOS" : "BIOS Imported",
                 msg[0] ? msg : o2session_last_error(session));
    return idx;
}

/* Import BIOS out of the "needs the BIOS" alert: once the console's system
 * ROM is complete, carry straight on -- open the refused cartridge again,
 * or (a game CONFIG booted from the network) power-cycle back to CONFIG on
 * the real BIOS, so booting the game again just works. Anything short of
 * that (half the system ROM, a bad file) is said in a dialog instead. */
static void on_needed_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    O2Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    const char *retry = g_object_get_data(src, "retry");
    char msg[1024];
    int idx;

    if (!file || !(path = g_file_get_path(file)))
        return;
    idx = o2session_import_bios(self->session, path, msg, sizeof msg);
    if (idx < 0 || !o2session_have_bios(self->session)) {
        show_message(GTK_WIDGET(self), idx < 0 ? "Cannot Import BIOS" : "BIOS Imported",
                     msg[0] ? msg : o2session_last_error(self->session));
        return;
    }
    if (retry && *retry) {
        open_cart_path(self, retry);
        return;
    }
    o2session_reset_to_config(self->session);
    o2_window_toast(self, "BIOS imported \xe2\x80\x94 back to CONFIG on the real BIOS");
}

static void on_bios_needed_response(AdwAlertDialog *dlg, const char *response,
                                    gpointer user_data)
{
    O2Window *self = user_data;
    GtkFileDialog *fd;
    if (strcmp(response, "import") != 0)
        return;
    fd = bios_dialog();
    g_object_set_data_full(G_OBJECT(fd), "retry",
                           g_strdup(g_object_get_data(G_OBJECT(dlg), "retry")), g_free);
    gtk_file_dialog_open(fd, GTK_WINDOW(self), NULL, on_needed_bios_chosen, self);
    g_object_unref(fd);
}

/* "This cartridge needs the Magnavox Odyssey 2 BIOS": commercial games are
 * written in the BIOS's own byte-code, and only the real BIOS runs them --
 * the app's stand-in runs CONFIG and FujiNet apps only. `retry` is a
 * cartridge Open refused (opened again after the import), or NULL for a
 * game CONFIG booted from the network. */
static void show_bios_needed(O2Window *self, const char *retry)
{
    AdwDialog *dlg;

    if (o2session_have_bios(self->session)) {
        /* imported, but switched off: importing again would not help */
        show_message(GTK_WIDGET(self), "This cartridge needs the Magnavox Odyssey 2 BIOS",
            "A BIOS is imported, but Preferences has \xe2\x80\x9cUse the imported "
            "BIOS\xe2\x80\x9d switched off, so the app's stand-in is running. It runs "
            "CONFIG and FujiNet apps, not games.");
        return;
    }
    dlg = adw_alert_dialog_new("This cartridge needs the Magnavox Odyssey 2 BIOS",
        "Odyssey 2 games are written in the BIOS's own byte-code, so only the real "
        "BIOS runs them; CONFIG and FujiNet apps run without it. Import your own "
        "copy: the 2K set, the 1K system ROM, or MAME's 84932.ic11 and 84933.ic12.");
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg),
                                   "later", "_Not Now",
                                   "import", "_Import BIOS\xe2\x80\xa6", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "import",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "import");
    adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "later");
    g_object_set_data_full(G_OBJECT(dlg), "retry", g_strdup(retry), g_free);
    g_signal_connect(dlg, "response", G_CALLBACK(on_bios_needed_response), self);
    adw_dialog_present(dlg, GTK_WIDGET(self));
}

/* The gamepad thread cannot call into GTK; it posts system actions and this
 * timer takes them. It also watches for gamepads coming and going, and says
 * so: a pad that drops off Bluetooth mid-game should not be a mystery. And
 * it watches for the stand-in meeting a game it cannot run (CONFIG booted
 * one from the network with no BIOS imported): one alert per time that
 * happens. */
static gboolean sysact_drain_tick(gpointer user_data)
{
    O2Window *self = user_data;
    unsigned gen;
    int sa;
    while (o2session_sysaction_take(self->session, &sa))
        o2_window_run_sysaction(self, sa);

    gen = o2session_gamepad_generation(self->session);
    if (gen != self->pad_generation) {
        char text[160];
        self->pad_generation = gen;
        if (o2session_gamepad_last_event(self->session, text, sizeof text) > 0)
            o2_window_toast(self, text);
    }

    if (o2session_needs_bios(self->session)) {
        if (!self->needs_bios_shown) {
            self->needs_bios_shown = TRUE;
            show_bios_needed(self, NULL);
        }
    } else {
        self->needs_bios_shown = FALSE;
    }
    sync_console_menu(self);
    return G_SOURCE_CONTINUE;
}

/* ---- status --------------------------------------------------------------- */

static gboolean update_status(gpointer user_data)
{
    O2Window *self = user_data;
    char text[1200], st[160];
    gboolean on = FALSE;

    if (!o2session_is_running(self->session)) {
        g_snprintf(text, sizeof text, "Stopped");
    } else {
        const char *cart = o2session_cart_path(self->session);
        const char *slash = cart ? strrchr(cart, '/') : NULL;
        int link = o2session_cart_link_up(self->session);
        o2session_cart_status(self->session, st, sizeof st);
        on = link == 1;
        if (cart && *cart)
            g_snprintf(text, sizeof text, "%s \xe2\x80\x94 FujiNet %s",
                       slash ? slash + 1 : cart, st);
        else
            g_snprintf(text, sizeof text, "FujiNet %s", st);
    }
    gtk_label_set_text(GTK_LABEL(self->status), text);
    if (on) {
        gtk_widget_add_css_class(self->status_dot, "o2-dot-on");
        gtk_widget_remove_css_class(self->status_dot, "o2-dot-off");
    } else {
        gtk_widget_add_css_class(self->status_dot, "o2-dot-off");
        gtk_widget_remove_css_class(self->status_dot, "o2-dot-on");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- cartridges and BIOSes ------------------------------------------------- */

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* A cartridge the FujiNet cartridge cannot plan says why in a dialog. A
 * game refused for want of the BIOS gets the alert that offers to import
 * one instead. */
static void open_cart_path(O2Window *self, const char *path)
{
    char msg[1200], why[256];
    if (o2session_load_cart(self->session, path) != 0) {
        /* refused by the BIOS rule: a plannable game, and no BIOS to run */
        if (o2session_is_running(self->session) &&
            o2session_check_cart(path, why, sizeof why) &&
            !o2session_cart_is_app(path) &&
            !(o2session_get_int(self->session, "use_bios", 1) &&
              o2session_have_bios(self->session))) {
            show_bios_needed(self, path);
            return;
        }
        show_message(GTK_WIDGET(self), "Cannot Open Cartridge",
                     o2session_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Running %s", base_name(path));
    o2_window_toast(self, msg);
}

/* A dropped file: a BIOS is imported, a cartridge runs, anything else goes
 * where it belongs. The BIOS test comes first: BIOS dumps are .bin and .rom
 * too, and they are recognised by size and CRC, not by name. */
void o2_window_load_media(O2Window *self, const char *path)
{
    char dest[1024];

    if (o2session_media_is_bios(path)) {
        o2_show_bios_import(GTK_WIDGET(self), self->session, path);
        return;
    }
    if (o2session_media_is_cartridge(path)) {
        open_cart_path(self, path);
        return;
    }
    if (o2session_import_media(self->session, path, dest, sizeof dest) != 0) {
        o2_window_toast(self, o2session_last_error(self->session));
        return;
    }
    o2_window_toast(self, "Copied to FujiNet's SD folder \xe2\x80\x94 mount it "
                           "from the CONFIG client");
}

static GtkFileDialog *cart_dialog(O2Window *self, const char *title)
{
    static const char *const exts[] = { "st2", "bin", "rom", NULL };
    const char *cart = o2session_cart_path(self->session);
    g_autofree char *dir = cart && *cart ? g_path_get_dirname(cart) : NULL;
    /* where the last cartridge came from: the next one is usually beside it */
    return file_dialog(title, "Magnavox Odyssey 2 cartridges (*.st2, *.bin, *.rom)",
                       exts, dir);
}

static void on_cart_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    O2Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) open_cart_path(self, path);
}

static void action_open(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    GtkFileDialog *dlg = cart_dialog(self, "Open Cartridge");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_cart_chosen, self);
    g_object_unref(dlg);
}

static void on_sd_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    O2Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    char dest[1024], msg[1200];
    if (!file) return;
    path = g_file_get_path(file);
    if (!path) return;
    if (o2session_import_cart_to_sd(self->session, path, dest, sizeof dest) != 0) {
        o2_window_toast(self, o2session_last_error(self->session));
        return;
    }
    g_snprintf(msg, sizeof msg, "Copied to %s \xe2\x80\x94 boot it from the "
               "CONFIG client", dest);
    o2_window_toast(self, msg);
}

static void action_import_sd(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    GtkFileDialog *dlg = cart_dialog(self, "Import Cartridge to SD");
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_sd_chosen, self);
    g_object_unref(dlg);
}

static void on_bios_chosen(GObject *src, GAsyncResult *res, gpointer user_data)
{
    O2Window *self = user_data;
    g_autoptr(GFile) file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(src), res, NULL);
    g_autofree char *path = NULL;
    if (!file) return;
    path = g_file_get_path(file);
    if (path) o2_show_bios_import(GTK_WIDGET(self), self->session, path);
}

static void action_import_bios(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    GtkFileDialog *dlg = bios_dialog();
    (void)a; (void)p;
    gtk_file_dialog_open(dlg, GTK_WINDOW(self), NULL, on_bios_chosen, self);
    g_object_unref(dlg);
}

static void action_eject(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    o2session_eject(self->session);
    o2_window_toast(self, "Cartridge ejected \xe2\x80\x94 back to CONFIG");
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x,
                        double y, gpointer user_data)
{
    O2Window *self = user_data;
    g_autofree char *path = NULL;
    (void)t; (void)x; (void)y;
    if (!G_VALUE_HOLDS(value, G_TYPE_FILE)) return FALSE;
    path = g_file_get_path(G_FILE(g_value_get_object(value)));
    if (!path) return FALSE;
    o2_window_load_media(self, path);
    return TRUE;
}

/* ---- the console and actions ---------------------------------------------- */

/* CLEAR from the menu: the session holds it for a few frames, long enough
 * for the console to see it (it samples the button once a frame). */
static void action_clear(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    o2session_clear(self->session);
}

static void action_reset_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    o2_window_run_sysaction(user_data, O2_SYSACT_RESET_CONFIG);
}

static void action_keypad(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    o2_keypad_window_toggle(GTK_WINDOW(self), self->session);
}

static void action_debugger(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    o2_debugger_toggle(GTK_WINDOW(self), self->session);
}

static void action_fullscreen(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    self->fullscreen = !self->fullscreen;
    if (self->fullscreen) gtk_window_fullscreen(GTK_WINDOW(self));
    else gtk_window_unfullscreen(GTK_WINDOW(self));
}

static void action_fujinet_config(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    if (o2session_fujinet_external(self->session)) {
        o2_window_toast(self, "FujiNet is external: use its own web UI");
        return;
    }
    if (!o2session_fujinet_running(self->session)) {
        o2_window_toast(self, "FujiNet is not running");
        return;
    }
    o2_fujiconfig_show(GTK_WINDOW(self), self->session);
}

static void action_fujinet_log(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    if (o2session_fujinet_external(self->session)) {
        o2_window_toast(self, "FujiNet is external: see its own console");
        return;
    }
    o2_fujilog_show(GTK_WINDOW(self), self->session);
}

/* Power-cycle onto the settings store's options (a full restart if FujiNet,
 * audio or the gamepads were switched). Preferences hands this to
 * o2_prefs_show() as its close callback. */
static void restart_session(O2Window *self)
{
    if (o2session_restart(self->session) != 0) {
        o2_window_toast(self, o2session_last_error(self->session));
        return;
    }
    o2_window_toast(self, "Machine options applied (power cycled)");
}

static void action_prefs(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    (void)a; (void)p;
    o2_prefs_show(self, self->session, restart_session);
}

static void action_quit(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    (void)a; (void)p;
    gtk_window_close(GTK_WINDOW(user_data));
}

static void action_about(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    AdwDialog *about;
    (void)a; (void)p;
    about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "FujiNet Go Odyssey 2");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), o2_icon_name());
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), O2_VERSION_STRING);
    adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(about), "Thomas Cherryhomes");
    adw_about_dialog_set_website(ADW_ABOUT_DIALOG(about), "https://fujinet.online/");
    adw_about_dialog_set_issue_url(ADW_ABOUT_DIALOG(about),
        "https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/issues");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
        "Magnavox Odyssey 2 with built-in FujiNet. The emulator is MAME's RCA Studio "
        "II, CDP1802 and CDP1861 (Curt Coder, BSD-3-Clause) transposed to C; the "
        "FujiNet firmware and the cartridge are by Thomas Cherryhomes and the "
        "FujiNet contributors (GPL-3.0-or-later).");
    adw_dialog_present(about, GTK_WIDGET(self));
}

static void action_aspect(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    gboolean tv = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(tv));
    o2_display_set_tv_aspect(O2_DISPLAY(self->display), tv);
    /* "aspect": 0 = the television's 4:3, 1 = square pixels */
    o2session_set_int(self->session, "aspect", tv ? 0 : 1);
}

static void action_smooth(GSimpleAction *a, GVariant *p, gpointer user_data)
{
    O2Window *self = user_data;
    gboolean sm = !g_variant_get_boolean(g_action_get_state(G_ACTION(a)));
    (void)p;
    g_simple_action_set_state(a, g_variant_new_boolean(sm));
    o2_display_set_smooth(O2_DISPLAY(self->display), sm);
    o2session_set_int(self->session, "smooth", sm ? 1 : 0);
}

void o2_window_apply_aspect(O2Window *self, int aspect)
{
    GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
    o2_display_set_tv_aspect(O2_DISPLAY(self->display), aspect == 0);
    if (a)
        g_simple_action_set_state(G_SIMPLE_ACTION(a), g_variant_new_boolean(aspect == 0));
}

static const GActionEntry win_actions[] = {
    { "open", action_open, NULL, NULL, NULL, { 0 } },
    { "eject", action_eject, NULL, NULL, NULL, { 0 } },
    { "import-sd", action_import_sd, NULL, NULL, NULL, { 0 } },
    { "import-bios", action_import_bios, NULL, NULL, NULL, { 0 } },
    { "clear", action_clear, NULL, NULL, NULL, { 0 } },
    { "reset-config", action_reset_config, NULL, NULL, NULL, { 0 } },
    { "keypad", action_keypad, NULL, NULL, NULL, { 0 } },
    { "debugger", action_debugger, NULL, NULL, NULL, { 0 } },
    { "fullscreen", action_fullscreen, NULL, NULL, NULL, { 0 } },
    { "tv-aspect", action_aspect, NULL, "true", NULL, { 0 } },
    { "smooth", action_smooth, NULL, "false", NULL, { 0 } },
    { "fujinet-config", action_fujinet_config, NULL, NULL, NULL, { 0 } },
    { "fujinet-log", action_fujinet_log, NULL, NULL, NULL, { 0 } },
    { "prefs", action_prefs, NULL, NULL, NULL, { 0 } },
    { "about", action_about, NULL, NULL, NULL, { 0 } },
    { "quit", action_quit, NULL, NULL, NULL, { 0 } },
};

/* ---- construction --------------------------------------------------------- */

static GMenu *build_menu(O2Window *self)
{
    GMenu *menu = g_menu_new();
    GMenu *cart = g_menu_new();
    GMenu *view = g_menu_new();
    GMenu *fuji = g_menu_new();
    GMenu *app = g_menu_new();

    g_menu_append(cart, "_Open Cartridge...", "win.open");
    g_menu_append(cart, "_Eject Cartridge", "win.eject");
    g_menu_append(cart, "_Import Cartridge to SD...", "win.import-sd");
    g_menu_append(cart, "Import _BIOS...", "win.import-bios");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(cart));

    /* filled (and kept current) by sync_console_menu */
    self->console_menu = g_menu_new();
    g_menu_append_section(menu, "Console", G_MENU_MODEL(self->console_menu));

    g_menu_append(view, "_Keypad (F9)", "win.keypad");
    g_menu_append(view, "_Debugger (F12)", "win.debugger");
    g_menu_append(view, "_TV Aspect", "win.tv-aspect");
    g_menu_append(view, "S_mooth Scaling", "win.smooth");
    g_menu_append(view, "_Fullscreen (F11)", "win.fullscreen");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(view));

    g_menu_append(fuji, "FujiNet _Web UI", "win.fujinet-config");
    g_menu_append(fuji, "Console _Log", "win.fujinet-log");
    g_menu_append_section(menu, "FujiNet", G_MENU_MODEL(fuji));

    g_menu_append(app, "_Preferences", "win.prefs");
    g_menu_append(app, "_About FujiNet Go Odyssey 2", "win.about");
    g_menu_append(app, "_Quit", "win.quit");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(app));

    g_object_unref(cart);
    g_object_unref(view);
    g_object_unref(fuji);
    g_object_unref(app);
    return menu;
}

static void o2_window_dispose(GObject *object)
{
    O2Window *self = O2_WINDOW(object);
    if (self->status_id) {
        g_source_remove(self->status_id);
        self->status_id = 0;
    }
    if (self->sysact_id) {
        g_source_remove(self->sysact_id);
        self->sysact_id = 0;
    }
    g_clear_object(&self->console_menu);
    G_OBJECT_CLASS(o2_window_parent_class)->dispose(object);
}

static void o2_window_class_init(O2WindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = o2_window_dispose;
}

static void o2_window_init(O2Window *self)
{
    (void)self;
}

GtkWidget *o2_window_new(AdwApplication *app, o2session *session)
{
    O2Window *self = g_object_new(O2_TYPE_WINDOW, "application", app, NULL);
    GtkWidget *box, *header, *menu_button, *toolbar, *status_box;
    GtkEventController *keys, *focus;
    g_autoptr(GMenu) menu = NULL;

    self->session = session;
    o2_install_accent_css();

    gtk_window_set_title(GTK_WINDOW(self), "FujiNet Go Odyssey 2");
    gtk_window_set_icon_name(GTK_WINDOW(self), o2_icon_name());
    /* The picture at the television's 4:3, 246 lines three times over:
     * 984 x 738 under the header bar. */
    gtk_window_set_default_size(GTK_WINDOW(self), 984, 738 + 46);

    g_action_map_add_action_entries(G_ACTION_MAP(self), win_actions,
                                    G_N_ELEMENTS(win_actions), self);
    {
        static const char *const prefs_accels[] = { "<Control>comma", NULL };
        static const char *const open_accels[] = { "<Control>o", NULL };
        static const char *const reboot_accels[] = { "<Control>r", NULL };
        static const char *const quit_accels[] = { "<Control>q", NULL };
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.prefs", prefs_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.open", open_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.reset-config", reboot_accels);
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), "win.quit", quit_accels);
    }

    header = adw_header_bar_new();
    menu = build_menu(self);
    menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_menu_button_set_primary(GTK_MENU_BUTTON(menu_button), TRUE);
    gtk_widget_set_tooltip_text(menu_button, "Main Menu");
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), menu_button);

    status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    self->status_dot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(self->status_dot, "o2-dot");
    gtk_widget_add_css_class(self->status_dot, "o2-dot-off");
    gtk_widget_set_valign(self->status_dot, GTK_ALIGN_CENTER);
    self->status = gtk_label_new("Starting...");
    gtk_widget_add_css_class(self->status, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(self->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_append(GTK_BOX(status_box), self->status_dot);
    gtk_box_append(GTK_BOX(status_box), self->status);
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), status_box);

    self->display = o2_display_new(session);
    o2_display_set_tv_aspect(O2_DISPLAY(self->display),
        o2session_get_int(session, "aspect", 0) == 0);
    o2_display_set_smooth(O2_DISPLAY(self->display),
        o2session_get_int(session, "smooth", 0) != 0);
    {
        GAction *a = g_action_map_lookup_action(G_ACTION_MAP(self), "tv-aspect");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(o2session_get_int(session, "aspect", 0) == 0));
        a = g_action_map_lookup_action(G_ACTION_MAP(self), "smooth");
        g_simple_action_set_state(G_SIMPLE_ACTION(a),
            g_variant_new_boolean(o2session_get_int(session, "smooth", 0) != 0));
    }

    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
        g_signal_connect(drop, "drop", G_CALLBACK(on_drop), self);
        gtk_widget_add_controller(self->display, GTK_EVENT_CONTROLLER(drop));
    }

    self->toast_overlay = adw_toast_overlay_new();
    adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(self->toast_overlay), self->display);

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), self->toast_overlay);
    gtk_box_append(GTK_BOX(box), toolbar);
    gtk_widget_set_vexpand(toolbar, TRUE);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), box);

    /* Capture on the WINDOW so input works no matter what has focus. */
    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), self);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), self);
    gtk_widget_add_controller(GTK_WIDGET(self), keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), self);
    gtk_widget_add_controller(GTK_WIDGET(self), focus);

    self->status_id = g_timeout_add_seconds(1, update_status, self);
    /* The current pad set is the baseline: no toast for pads that were
     * already plugged in at launch. */
    self->pad_generation = o2session_gamepad_generation(session);
    self->sysact_id = g_timeout_add(250, sysact_drain_tick, self);
    sync_console_menu(self);
    update_status(self);

    /* O2_OPEN_KEYPAD=1 / O2_OPEN_DEBUGGER=1 / O2_OPEN_SETTINGS=1
     * open those windows at launch, following the family's convention: the
     * way in when the app misbehaves before the menu is reachable. */
    {
        const char *env = g_getenv("O2_OPEN_KEYPAD");
        if (env && *env && *env != '0')
            o2_keypad_window_toggle(GTK_WINDOW(self), session);
        env = g_getenv("O2_OPEN_DEBUGGER");
        if (env && *env && *env != '0')
            o2_debugger_show(GTK_WINDOW(self), session);
        env = g_getenv("O2_OPEN_SETTINGS");
        if (env && *env && *env != '0')
            o2_prefs_show(self, session, restart_session);
    }
    return GTK_WIDGET(self);
}
