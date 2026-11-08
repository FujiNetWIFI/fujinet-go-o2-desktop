/*
 * help_test -- the Help covers what the app has: every Preferences setting
 * and choice, every machine, every ROM, every default key and gamepad button,
 * the app's shortcuts (and the Mac's), the two ports, the debugger's tabs
 * and prompt commands, and every context a Help button opens. Every miss is
 * listed before it fails.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "o2help.h"
#include "o2session.h"
#include "test_tmpdir.h"

#define CHECK(c, ...) do { if (!(c)) { fprintf(stderr, "FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } } while (0)

static const unsigned platforms[] = {
    O2HELP_GNOME, O2HELP_KDE, O2HELP_WINDOWS, O2HELP_MACOS
};
static const char *const platform_names[] = { "GNOME", "KDE", "Windows", "macOS" };

/* What the frontends' Help buttons and menus open. */
static const char *const contexts[] = {
    "main", "nobios", "import-sd",
    "prefs/Machine", "prefs/ROMs", "prefs/Display", "prefs/Input", "prefs/FujiNet",
    "debugger/prompt", "debugger/cpu", "debugger/disassembly", "debugger/video", "debugger/ef9340",
    "debugger/io", "debugger/breakpoints", "debugger/cart", "debugger/fujinet",
    "controllers", "fujilog",
};

/* The prompt's commands (core/include/o2debug.h). */
static const char *const debug_commands[] = {
    "step", "over", "out", "run", "stop", "runto", "scanline", "frame",
    "break", "bpr", "bpw", "bpxr", "bpxw", "bpir", "bpiw", "bpvr", "bpvw", "bpin", "bpout", "delete",
    "enable", "disable", "clear", "breaks", "print", "regs", "set", "mem", "poke", "vdc",
    "objects", "ef", "voice", "io", "keys", "cart", "mailbox", "label", "labels",
    "disasm", "trace", "save",
};

/* The Preferences pages: what each one has, as the frontends label it. */
static const struct {
    const char *context;
    const char *labels[10];
} pages[] = {
    { "prefs/Machine", { "Machine", "BIOS", "Use the imported BIOS", "The Voice in front of the cartridge", NULL } },
    { "prefs/ROMs", { "Import BIOS", "Import from a Folder", "Imported", "Not imported", NULL } },
    { "prefs/Display", { "Aspect", "Smooth scaling", "Enable audio", "Volume", NULL } },
    { "prefs/Input", { "Enable gamepads", "Analog sticks steer too", "Gamepads", "Automatic",
                       "Stick 1", "Stick 2", NULL } },
    { "prefs/FujiNet", { "Enable FujiNet", "Use external FujiNet", "Address", "Web UI", NULL } },
};

/* The settings keys the pages write. */
static const char *const settings_keys[] = {
    "machine", "use_bios", "voice", "analog_joystick", "enable_fujinet", "fujinet_external",
    "fujinet_external_addr", "enable_audio", "enable_gamepad", "volume", "cart", "aspect", "smooth",
};

/* The app's shortcuts, as a PC and as a Mac shows them. */
static const struct { const char *pc, *mac; } shortcuts[] = {
    { "Ctrl+O", "⌘O" }, { "Ctrl+R", "⌘R" }, { "Ctrl+,", "⌘," }, { "Ctrl+Q", "⌘Q" },
    { "Ctrl+Shift+H", "⌘?" }, { "F9", "F9" }, { "F11", "F11" }, { "F12", "F12" },
};

static int misses;

/* Lower case, every run of white space one space. */
static void
fold(const char *in, char *out, size_t outsz)
{
    size_t n = 0;
    int space = 0;
    for (; *in && n + 1 < outsz; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == ' ' || c == '\n' || c == '\t') {
            space = 1;
            continue;
        }
        if (space && n)
            out[n++] = ' ';
        space = 0;
        out[n++] = (char) ((c >= 'A' && c <= 'Z') ? c + 32 : c);
    }
    out[n] = 0;
}

static char section[1 << 18];
static char raw[1 << 18];

/* The folded text of a context's (or slug's) section on platform p; raw
 * keeps the lines. */
static const char *
section_text(int p, const char *context)
{
    int t, b;
    section[0] = 0;
    raw[0] = 0;
    if (o2help_find(context, &t, &b) == 0) {
        o2help_plain_text(platforms[p], t, b, raw, sizeof raw);
        fold(raw, section, sizeof section);
    }
    return section;
}

static void
require(int p, const char *context, const char *what, const char *text)
{
    char want[512];
    fold(text, want, sizeof want);
    if (!strstr(section_text(p, context), want)) {
        fprintf(stderr, "MISSING (%s) %s: %s \"%s\"\n", platform_names[p], context, what, text);
        misses++;
    }
}

/* A line of the section (a table row: cells joined by " | ") with every one
 * of these, folded, in that order. */
static void
require_row(int p, const char *context, const char *a, const char *b, const char *c)
{
    char line[1024], fa[256], fb[256], fc[256];
    const char *s, *e;
    int ok = 0;

    section_text(p, context);
    fold(a, fa, sizeof fa);
    fold(b ? b : "", fb, sizeof fb);
    fold(c ? c : "", fc, sizeof fc);
    for (s = raw; *s && !ok; s = *e ? e + 1 : e) {
        char *x, *y;
        size_t n;
        e = strchr(s, '\n');
        if (!e)
            e = s + strlen(s);
        n = (size_t) (e - s) < sizeof line - 1 ? (size_t) (e - s) : sizeof line - 1;
        memcpy(line, s, n);
        line[n] = 0;
        fold(line, line, sizeof line);
        x = strstr(line, fa);
        if (!x)
            continue;
        y = b && *fb ? strstr(x + strlen(fa), fb) : x + strlen(fa);
        if (!y)
            continue;
        if (c && *fc && !strstr(y + (b && *fb ? strlen(fb) : 0), fc))
            continue;
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "MISSING (%s) %s: a row \"%s\" | \"%s\" | \"%s\"\n", platform_names[p], context,
                a, b ? b : "", c ? c : "");
        misses++;
    }
}

static void
check_prefs(o2session *s, int p)
{
    char buf[128];

    for (size_t k = 0; k < sizeof pages / sizeof *pages; k++)
        for (int i = 0; pages[k].labels[i]; i++)
            require(p, pages[k].context, "label", pages[k].labels[i]);

    for (int m = 0; m < O2_MACHINE_COUNT; m++)
        require(p, "machines", "machine", o2_machine_name(m));
    for (int i = 0; i < o2session_rom_item_count(); i++) {
        const o2_rom_item *it = o2session_rom_item(i);
        snprintf(buf, sizeof buf, "%s", it->file);
        require(p, "bios-and-roms", "ROM file", buf);
        require(p, "prefs/ROMs", "ROM group", it->group);
    }
    for (size_t k = 0; k < sizeof settings_keys / sizeof *settings_keys; k++)
        require(p, "the-settings-file", "setting", settings_keys[k]);
    (void) s;
}

/* Every default key and gamepad button, the console's buttons and the
 * shortcuts. */
static void
check_controls(o2session *s, int p)
{
    char name[64];

    CHECK(O2_TARGET_COUNT == 77, "the Help says 77 controls");
    require(p, "changing-the-controls", "count", "77 in all");
    for (int port = 0; port < 2; port++)
        for (int a = 0; a < O2_ACT_PER_PORT; a++) {
            o2_binding b = o2session_binding_get(s, O2_TARGET_PORT(port, a));
            if (b.keysym) {
                o2session_keysym_name(b.keysym, name, sizeof name);
                require(p, "default-keys", "stick key", name);
            }
        }
    require_row(p, "default-keys", "RESET", "F1", NULL);
    require_row(p, "default-keys", "Reset to CONFIG", "Escape", NULL);
    require_row(p, "gamepads", "D-pad", "stick", NULL);
    require_row(p, "gamepads", "South", "Fire", NULL);
    CHECK(o2session_binding_get(s, O2_TARGET_PORT(0, O2_ACT_FIRE)).button == O2_PAD_BTN_SOUTH,
          "the gamepad rows are the default bindings");

    for (size_t k = 0; k < sizeof shortcuts / sizeof *shortcuts; k++)
        require(p, "default-keys", "shortcut", platforms[p] == O2HELP_MACOS ? shortcuts[k].mac : shortcuts[k].pc);
}

/* The two ports, in the FujiNet topic. */
static void
check_ports(int p)
{
    char port[16];

    snprintf(port, sizeof port, "%d", O2SESSION_BOIP_PORT);
    require(p, "fujinet", "BoIP port", port);
    snprintf(port, sizeof port, "%d", O2SESSION_WEBUI_PORT);
    require(p, "fujinet", "web UI port", port);
    require(p, "an-external-fujinet", "default address", "127.0.0.1:11520");
}

/* Each command starts a `code` span in the prompt's section. */
static void
check_debugger(int p)
{
    int t, b;
    CHECK(o2help_find("debugger/prompt", &t, &b) == 0, "no debugger/prompt");
    const o2help_topic *tp = o2help_topic_at(t);
    int end = o2help_section_end(t, b);
    for (size_t k = 0; k < sizeof debug_commands / sizeof *debug_commands; k++) {
        const char *cmd = debug_commands[k];
        size_t n = strlen(cmd);
        int found = 0;
        for (int bl = b; bl < end && !found; bl++) {
            const o2help_block *blk = &tp->blocks[bl];
            if (!(blk->platforms & platforms[p]))
                continue;
            for (int i = 0; i < blk->nspans && !found; i++) {
                const char *c = blk->spans[i].text;
                if (blk->spans[i].kind != O2HELP_CODE)
                    continue;
                found = !strncmp(c, cmd, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '/');
            }
        }
        if (!found) {
            fprintf(stderr, "MISSING (%s) debugger/prompt: command `%s`\n", platform_names[p], cmd);
            misses++;
        }
    }
    /* the eight tabs */
    require(p, "debugger", "tabs", "nine tabs");
}

int
main(void)
{
    char cfg[512], data[512];
    o2session_paths paths;
    o2help_hit hits[16];
    o2session *s;
    int t, b;

    test_tmpdir(cfg, sizeof cfg, "hcfg");
    test_tmpdir(data, sizeof data, "hdata");
    memset(&paths, 0, sizeof paths);
    paths.config_dir = cfg;
    paths.data_dir = data;
    paths.fujinet_lib = "";
    s = o2session_new(&paths);
    CHECK(s, "new");
    CHECK(o2help_topic_count() > 0, "no topics");

    for (size_t k = 0; k < sizeof contexts / sizeof *contexts; k++)
        if (o2help_find(contexts[k], &t, &b) != 0) {
            fprintf(stderr, "MISSING context %s\n", contexts[k]);
            misses++;
        }
    for (int p = 0; p < 4; p++) {
        check_prefs(s, p);
        check_controls(s, p);
        check_ports(p);
        check_debugger(p);
    }

    /* lookup and search */
    CHECK(o2help_find("#preferences", &t, &b) == 0 && b == -1, "#preferences");
    CHECK(o2help_find("#the-settings-file", &t, &b) == 0 && b >= 0, "#the-settings-file");
    CHECK(o2help_search(O2HELP_GNOME, "mailbox", hits, 16) > 0, "search mailbox");
    CHECK(o2help_search(O2HELP_GNOME, "zzqxj", hits, 16) == 0, "search nonsense");
    int n = o2help_search(O2HELP_KDE, "breakpoints", hits, 16);
    CHECK(n > 0 && o2help_find("debugger/breakpoints", &t, &b) == 0
          && hits[0].topic == t && hits[0].block == b, "breakpoints is the first hit");
    for (int i = 1; i < n; i++)
        CHECK(hits[i - 1].score >= hits[i].score, "hits best first");

    /* the Mac sees its own keys */
    {
        char text[4096];
        CHECK(o2help_find("default-keys", &t, &b) == 0, "default-keys");
        o2help_plain_text(O2HELP_MACOS, t, b, text, sizeof text);
        CHECK(strstr(text, "⌘O") != NULL, "the Mac's Open Cartridge key");
        o2help_plain_text(O2HELP_GNOME, t, b, text, sizeof text);
        CHECK(strstr(text, "Ctrl+O") != NULL && !strstr(text, "⌘O"), "the PC's");
    }

    o2session_free(s);
    CHECK(misses == 0, "%d missing from data/help/help.md", misses);
    printf("help ok: %d topics\n", o2help_topic_count());
    return 0;
}
