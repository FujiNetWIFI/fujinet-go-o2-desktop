/*
 * The Keypad window -- both Odyssey 2 keypads side by side, each laid out
 * like the one on the console (1 2 3 / 4 5 6 / 7 8 9, then 0 under the 8),
 * the console's CLEAR button and Reset to CONFIG, Map mode, and the
 * gamepads' keypad assignments.
 *
 * Every control is pressed with a raw GtkGestureClick (press AND release),
 * not GtkButton's "clicked": the console reads a keypad one key at a time
 * (OUT 2 latches the key, EF3/EF4 report it), polling, so a value present
 * only for the instant of a click falls between two reads and a game never
 * sees it. A key on screen is HELD for as long as the mouse button is down,
 * exactly like the real one -- and so is CLEAR, which holds the CPU in reset
 * for as long as it is down.
 *
 * The gesture's "cancel" is wired to the same release path as "released":
 * dragging off a key must not leave the machine believing it is still down
 * forever.
 *
 * What the console sees held -- keyboard, gamepads and this window together
 * (o2session_buttons_held, o2session_switch_held) -- is painted in the
 * accent colour, so the window doubles as an input tester for a new
 * gamepad.
 *
 * SINGLETON, hidden rather than destroyed, so a remap survives closing it.
 * Not resizable: under a tiling compositor a resizable panel is tiled to
 * half a monitor and its keys stretch, so the keys have fixed sizes, wide
 * and tall enough for Map mode's two-line labels, and the window floats at
 * the size they make.
 *
 * MAP MODE: the Map button arms a two-step rebind -- click any control to
 * pick the target, then press a keyboard key OR a gamepad button. While
 * armed, the same gestures pick the target instead of injecting input, the
 * key handler intercepts the next keystroke, and a timer polls the session's
 * gamepad capture. Rebinding steals the key from whatever held it.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "keypad_window.h"

#include "../window.h"

#include <string.h>

/* A key's face, sized for Map mode's widest default labels ("Keypad 0\nA",
 * about 100 px in the default bold button font) so arming Map does not
 * resize the window */
#define KEY_W 104
#define KEY_H 52
#define CONSOLE_W 160

/* ---- singleton state ------------------------------------------------------ */

static GtkWidget *g_window;
static GtkWindow *g_parent;     /* the main window: system actions go through it */
static o2session *g_session;

/* Map mode: -2 idle, -1 armed and waiting for a target, >= 0 waiting for a
 * key or pad button to bind to that target. */
static int g_map_state = -2;
static GtkWidget *g_map_button;
static GtkWidget *g_map_hint;
static guint g_capture_timer;
static guint g_held_timer;
static guint g_pad_timer;
static GtkWidget *g_pad_box;
static unsigned g_pad_generation = (unsigned)-1;
static gboolean g_sysact_down[O2_SYSACT_COUNT];

typedef struct {
    GtkWidget *button;
    int target;
} control;

static control g_controls[O2_TARGET_COUNT];
static int g_ncontrols;

static void refresh_labels(void);

/* ---- pressing ------------------------------------------------------------- */

static void run_sysaction(int sa)
{
    if (g_parent)
        o2_window_run_sysaction(O2_WINDOW(g_parent), sa);
    else
        o2session_sysaction(g_session, sa);
}

static void press_target(int target, int down)
{
    if (target >= O2_TARGET_SYSACT(0)) {
        /* System actions fire on release, like a real button: pressing and
         * dragging off should not power-cycle the cartridge. */
        if (!down)
            run_sysaction(target - O2_TARGET_SYSACT(0));
        return;
    }
    o2session_press(g_session, target, down);
}

static void stop_capture(void)
{
    if (g_capture_timer) {
        g_source_remove(g_capture_timer);
        g_capture_timer = 0;
    }
    o2session_gamepad_capture_cancel(g_session);
}

static gboolean capture_tick(gpointer d)
{
    int button;
    (void)d;
    if (g_map_state < 0) { g_capture_timer = 0; return G_SOURCE_REMOVE; }
    if (o2session_gamepad_capture_poll(g_session, &button)) {
        char stolen[128], msg[200];
        o2session_binding_set_button(g_session, g_map_state, button, stolen, sizeof stolen);
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", o2_pad_button_name(button), stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", o2_pad_button_name(button));
        g_capture_timer = 0;
        g_map_state = -1;     /* stay armed: remapping several in a row is normal */
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        refresh_labels();
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void set_map_state(int state)
{
    g_map_state = state;
    if (state == -2) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Map");
        gtk_widget_remove_css_class(g_map_button, "o2-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "");
    } else if (state == -1) {
        stop_capture();
        gtk_button_set_label(GTK_BUTTON(g_map_button), "Done");
        gtk_widget_add_css_class(g_map_button, "o2-accent");
        gtk_label_set_text(GTK_LABEL(g_map_hint), "Click a control to remap");
    } else {
        char msg[128];
        g_snprintf(msg, sizeof msg, "Press a key or gamepad button for %s",
                   o2_target_name(state));
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        o2session_gamepad_capture_begin(g_session);
        if (!g_capture_timer)
            g_capture_timer = g_timeout_add(50, capture_tick, NULL);
    }
    refresh_labels();
}

static void on_pressed(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;

    if (g_map_state == -1) { set_map_state(target); return; }
    if (g_map_state >= 0) return;
    press_target(target, 1);
}

static void on_released(GtkGestureClick *g, int n, double x, double y, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)n; (void)x; (void)y;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static void on_cancelled(GtkGesture *g, GdkEventSequence *seq, gpointer user_data)
{
    int target = GPOINTER_TO_INT(user_data);
    (void)g; (void)seq;
    if (g_map_state != -2) return;
    press_target(target, 0);
}

static GtkWidget *control_button(const char *label, int target, int w, int h)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    GtkGesture *g = gtk_gesture_click_new();

    gtk_widget_set_size_request(b, w, h);
    /* Map mode's two-line labels (key, then pad button) read centred */
    gtk_label_set_justify(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(b))), GTK_JUSTIFY_CENTER);
    g_signal_connect(g, "pressed", G_CALLBACK(on_pressed), GINT_TO_POINTER(target));
    g_signal_connect(g, "released", G_CALLBACK(on_released), GINT_TO_POINTER(target));
    g_signal_connect(g, "cancel", G_CALLBACK(on_cancelled), GINT_TO_POINTER(target));
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    /* Not focusable: clicking a key must not steal focus, and tabbing
     * through 22 buttons is nobody's idea of input. */
    gtk_widget_set_focusable(b, FALSE);
    g_object_set_data_full(G_OBJECT(b), "face", g_strdup(label), g_free);

    if (g_ncontrols < O2_TARGET_COUNT) {
        g_controls[g_ncontrols].button = b;
        g_controls[g_ncontrols].target = target;
        g_ncontrols++;
    }
    return b;
}

/* In Map mode each control shows what is bound to it, so choosing what to
 * change does not require remembering the whole table. */
static void refresh_labels(void)
{
    int i;
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        GtkWidget *b = g_controls[i].button;
        if (g_map_state != -2) {
            o2_binding bind = o2session_binding_get(g_session, t);
            char key[32], text[80];
            o2session_keysym_name(bind.keysym, key, sizeof key);
            if (bind.button != O2_PAD_BTN_NONE)
                g_snprintf(text, sizeof text, "%s\n%s", *key ? key : "\xe2\x80\x94",
                           o2_pad_button_name(bind.button));
            else
                g_snprintf(text, sizeof text, "%s", *key ? key : "\xe2\x80\x94");
            gtk_button_set_label(GTK_BUTTON(b), text);
            if (t == g_map_state) gtk_widget_add_css_class(b, "o2-accent");
            else gtk_widget_remove_css_class(b, "o2-accent");
        } else {
            gtk_widget_remove_css_class(b, "o2-accent");
            gtk_button_set_label(GTK_BUTTON(b), g_object_get_data(G_OBJECT(b), "face"));
        }
    }
}

/* What the console sees held, lit in the accent colour. */
static gboolean held_tick(gpointer d)
{
    unsigned held[2];
    int i;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window))
        return G_SOURCE_CONTINUE;
    if (g_map_state != -2)
        return G_SOURCE_CONTINUE;
    held[0] = o2session_buttons_held(g_session, 0);
    held[1] = o2session_buttons_held(g_session, 1);
    for (i = 0; i < g_ncontrols; i++) {
        int t = g_controls[i].target;
        gboolean on = FALSE;
        if (t < 2 * O2_ACT_PER_PORT)
            on = (held[t / O2_ACT_PER_PORT] >> (t % O2_ACT_PER_PORT)) & 1;
        else if (t < O2_TARGET_SYSACT(0))
            on = o2session_switch_held(g_session, t - O2_TARGET_SWITCH(0)) != 0;
        if (on) gtk_widget_add_css_class(g_controls[i].button, "o2-accent");
        else gtk_widget_remove_css_class(g_controls[i].button, "o2-accent");
    }
    return G_SOURCE_CONTINUE;
}

/* ---- one keypad ----------------------------------------------------------- */

static GtkWidget *build_keypad(int port)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *title;
    char label[32];
    int key;

    g_snprintf(label, sizeof label, "Keypad %c", 'A' + port);
    title = gtk_label_new(label);
    gtk_widget_add_css_class(title, "heading");

    /* The console's layout: 1-9 in reading order, three to a row, and 0
     * alone under the 8. */
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    for (key = 1; key <= 9; key++) {
        char face[4];
        g_snprintf(face, sizeof face, "%d", key);
        gtk_grid_attach(GTK_GRID(grid),
                        control_button(face, O2_TARGET_PORT(port, O2_ACT_0 + key),
                                       KEY_W, KEY_H),
                        (key - 1) % 3, (key - 1) / 3, 1, 1);
    }
    gtk_grid_attach(GTK_GRID(grid),
                    control_button("0", O2_TARGET_PORT(port, O2_ACT_0), KEY_W, KEY_H),
                    1, 3, 1, 1);
    gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), grid);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 14);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_frame_set_child(GTK_FRAME(frame), box);
    return frame;
}

/* ---- gamepads ------------------------------------------------------------- */

static void on_assign_changed(GObject *o, GParamSpec *ps, gpointer user_data)
{
    int idx = GPOINTER_TO_INT(user_data);
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(o));
    (void)ps;
    /* 0 = automatic (connection order), 1 = keypad A, 2 = keypad B */
    o2session_gamepad_assign(g_session, idx, sel == 0 ? -1 : (int)sel - 1);
}

/* Rebuilt whenever a pad comes or goes (the session's generation counter):
 * one row per pad, its name and which keypad it drives. */
static void refresh_pads(void)
{
    static const char *const choices[] = { "Automatic", "Keypad A", "Keypad B", NULL };
    GtkWidget *child;
    int n, i;

    while ((child = gtk_widget_get_first_child(g_pad_box)) != NULL)
        gtk_box_remove(GTK_BOX(g_pad_box), child);

    n = o2session_gamepad_count(g_session);
    if (n == 0) {
        GtkWidget *none = gtk_label_new("No gamepads connected");
        gtk_widget_add_css_class(none, "dim-label");
        gtk_box_append(GTK_BOX(g_pad_box), none);
        return;
    }
    for (i = 0; i < n; i++) {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *name, *dd, *eff;
        char text[128], port[32];
        int assign = o2session_gamepad_assignment(g_session, i);
        int effective = o2session_gamepad_effective_port(g_session, i);

        o2session_gamepad_name(g_session, i, text, sizeof text);
        name = gtk_label_new(text);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_label_set_xalign(GTK_LABEL(name), 0);
        gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
        dd = gtk_drop_down_new_from_strings(choices);
        gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), assign < 0 ? 0 : (guint)assign + 1);
        g_signal_connect(dd, "notify::selected", G_CALLBACK(on_assign_changed), GINT_TO_POINTER(i));
        if (effective >= 0) g_snprintf(port, sizeof port, "drives keypad %c", 'A' + effective);
        else g_snprintf(port, sizeof port, "drives nothing");
        eff = gtk_label_new(port);
        gtk_widget_add_css_class(eff, "dim-label");
        gtk_box_append(GTK_BOX(row), name);
        gtk_box_append(GTK_BOX(row), eff);
        gtk_box_append(GTK_BOX(row), dd);
        gtk_box_append(GTK_BOX(g_pad_box), row);
    }
}

static gboolean pad_tick(gpointer d)
{
    unsigned gen;
    (void)d;
    if (!g_window || !gtk_widget_get_visible(g_window)) return G_SOURCE_CONTINUE;
    gen = o2session_gamepad_generation(g_session);
    if (gen != g_pad_generation) {
        g_pad_generation = gen;
        refresh_pads();
    }
    return G_SOURCE_CONTINUE;
}

/* ---- keyboard ------------------------------------------------------------- */

static gboolean on_key_pressed(GtkEventControllerKey *c, guint keyval,
                               guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? o2session_keysym_from_evdev(code - 8) : 0;
    int sa;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;

    if (g_map_state >= 0) {
        char stolen[128], msg[200], name[32];
        o2session_binding_set_key(g_session, g_map_state, keysym, stolen, sizeof stolen);
        o2session_keysym_name(keysym, name, sizeof name);
        set_map_state(-1);   /* stay armed: remapping several in a row is normal */
        if (stolen[0])
            g_snprintf(msg, sizeof msg, "Bound %s (was %s)", name, stolen);
        else
            g_snprintf(msg, sizeof msg, "Bound %s", name);
        gtk_label_set_text(GTK_LABEL(g_map_hint), msg);
        return TRUE;
    }
    if (g_map_state == -1) return TRUE;
    /* an open drop-down (a gamepad's keypad) has the arrows and Return */
    {
        GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(g_window));
        if (focus && gtk_widget_get_ancestor(focus, GTK_TYPE_POPOVER))
            return FALSE;
    }
    if (keyval == GDK_KEY_F9) {
        o2_keypad_window_toggle(NULL, g_session);
        return TRUE;
    }
    /* Otherwise behave exactly like the main window, so playing works
     * whichever window has focus: a system action first, on its leading
     * edge only (GTK4 has no repeat flag). */
    sa = o2session_key_sysaction(g_session, keysym);
    if (sa >= 0) {
        if (!g_sysact_down[sa]) {
            g_sysact_down[sa] = TRUE;
            run_sysaction(sa);
        }
        return TRUE;
    }
    return o2session_key(g_session, keysym, 1) ? TRUE : FALSE;
}

static gboolean on_key_released(GtkEventControllerKey *c, guint keyval,
                                guint code, GdkModifierType st, gpointer d)
{
    guint32 keysym = code >= 8 ? o2session_keysym_from_evdev(code - 8) : 0;
    int sa;
    (void)c; (void)st; (void)d;
    if (!keysym) keysym = keyval;
    if (g_map_state != -2) return TRUE;
    sa = o2session_key_sysaction(g_session, keysym);
    if (sa >= 0) {
        g_sysact_down[sa] = FALSE;
        return TRUE;
    }
    return o2session_key(g_session, keysym, 0) ? TRUE : FALSE;
}

/* As in the main window: keys held into a focus change must not stay down. */
static void on_focus_leave(GtkEventControllerFocus *c, gpointer d)
{
    (void)c; (void)d;
    o2session_release_all(g_session);
    memset(g_sysact_down, 0, sizeof g_sysact_down);
}

/* ---- the window ----------------------------------------------------------- */

static void on_map_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    set_map_state(g_map_state == -2 ? -1 : -2);
}

static void on_defaults_clicked(GtkButton *b, gpointer d)
{
    (void)b; (void)d;
    o2session_bindings_reset(g_session);
    refresh_labels();
}

static gboolean on_close(GtkWindow *w, gpointer d)
{
    (void)d;
    set_map_state(-2);
    gtk_widget_set_visible(GTK_WIDGET(w), FALSE);
    return TRUE;
}

static void on_window_map(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (!g_held_timer)
        g_held_timer = g_timeout_add(33, held_tick, NULL);
    if (!g_pad_timer)
        g_pad_timer = g_timeout_add(250, pad_tick, NULL);
}

static void on_window_unmap(GtkWidget *w, gpointer d)
{
    (void)w; (void)d;
    if (g_held_timer) {
        g_source_remove(g_held_timer);
        g_held_timer = 0;
    }
    if (g_pad_timer) {
        g_source_remove(g_pad_timer);
        g_pad_timer = 0;
    }
}

static void build_window(GtkWindow *parent)
{
    GtkWidget *root, *pads, *console, *maprow, *toolbar, *header, *defaults, *contitle, *padtitle;
    GtkEventController *keys, *focus;

    g_parent = parent;
    g_window = adw_window_new();
    gtk_window_set_title(GTK_WINDOW(g_window), "Keypad");
    gtk_window_set_transient_for(GTK_WINDOW(g_window), parent);
    /* Same app-id as the main window, so the shell groups and icons it. */
    gtk_window_set_application(GTK_WINDOW(g_window), gtk_window_get_application(parent));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(g_window), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(g_window), FALSE);
    g_signal_connect(g_window, "close-request", G_CALLBACK(on_close), NULL);

    g_ncontrols = 0;

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(root, 12);
    gtk_widget_set_margin_bottom(root, 12);
    gtk_widget_set_margin_start(root, 12);
    gtk_widget_set_margin_end(root, 12);

    pads = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_append(GTK_BOX(pads), build_keypad(0));
    gtk_box_append(GTK_BOX(pads), build_keypad(1));
    gtk_box_append(GTK_BOX(root), pads);

    /* CLEAR is held like a key (the CPU stays in reset while it is down);
     * Reset to CONFIG is a power cycle, fired on release. */
    contitle = gtk_label_new("Console");
    gtk_widget_add_css_class(contitle, "heading");
    gtk_box_append(GTK_BOX(root), contitle);
    console = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(console, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(console), control_button("CLEAR",
                   O2_TARGET_SWITCH(O2_SW_CLEAR), CONSOLE_W, KEY_H));
    gtk_box_append(GTK_BOX(console), control_button("Reset to CONFIG",
                   O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG), CONSOLE_W, KEY_H));
    gtk_box_append(GTK_BOX(root), console);

    padtitle = gtk_label_new("Gamepads");
    gtk_widget_add_css_class(padtitle, "heading");
    gtk_box_append(GTK_BOX(root), padtitle);
    g_pad_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_append(GTK_BOX(root), g_pad_box);

    maprow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    g_map_button = gtk_button_new_with_label("Map");
    g_signal_connect(g_map_button, "clicked", G_CALLBACK(on_map_clicked), NULL);
    defaults = gtk_button_new_with_label("Defaults");
    g_signal_connect(defaults, "clicked", G_CALLBACK(on_defaults_clicked), NULL);
    g_map_hint = gtk_label_new("");
    gtk_widget_add_css_class(g_map_hint, "dim-label");
    gtk_widget_set_hexpand(g_map_hint, TRUE);
    /* a long hint ("Press a key or gamepad button for Keypad B: Key 7")
     * must not widen the window */
    gtk_label_set_ellipsize(GTK_LABEL(g_map_hint), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(g_map_hint), 0);
    gtk_box_append(GTK_BOX(maprow), g_map_button);
    gtk_box_append(GTK_BOX(maprow), defaults);
    gtk_box_append(GTK_BOX(maprow), g_map_hint);
    gtk_box_append(GTK_BOX(root), maprow);

    header = adw_header_bar_new();
    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), root);
    adw_window_set_content(ADW_WINDOW(g_window), toolbar);

    keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key_pressed), NULL);
    g_signal_connect(keys, "key-released", G_CALLBACK(on_key_released), NULL);
    gtk_widget_add_controller(g_window, keys);

    focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "leave", G_CALLBACK(on_focus_leave), NULL);
    gtk_widget_add_controller(g_window, focus);

    /* the two poll timers run only while the window is on screen */
    g_signal_connect(g_window, "map", G_CALLBACK(on_window_map), NULL);
    g_signal_connect(g_window, "unmap", G_CALLBACK(on_window_unmap), NULL);
    set_map_state(-2);
}

static void show_window(void)
{
    g_pad_generation = (unsigned)-1;
    pad_tick(NULL);
    gtk_window_present(GTK_WINDOW(g_window));
    pad_tick(NULL);
}

void o2_keypad_window_toggle(GtkWindow *parent, o2session *session)
{
    g_session = session;
    if (!g_window)
        build_window(parent);

    if (gtk_widget_get_visible(g_window)) {
        set_map_state(-2);
        gtk_widget_set_visible(g_window, FALSE);
    } else {
        show_window();
    }
}

gboolean o2_keypad_window_is_visible(void)
{
    return g_window && gtk_widget_get_visible(g_window);
}
