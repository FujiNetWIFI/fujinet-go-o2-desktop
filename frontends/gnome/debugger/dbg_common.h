/*
 * What the debugger window's files share: the small widget helpers, and the
 * entry points of the two tabs that live in dbg_video.c (the 8244 and the
 * G7400's EF9340).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <adwaita.h>
#include <pango/pangocairo.h>

#include "o2debug.h"

G_BEGIN_DECLS

/* A monospace text view in a scrolled window (the view is returned in *out). */
GtkWidget *dbg_mono_view(GtkTextView **out, gboolean editable);
/* Replaces a view's text where the reader left it; unchanged text leaves the
 * buffer, the scroll position and any selection alone. */
void dbg_set_text(GtkTextView *view, const char *text);
GtkWidget *dbg_labeled(const char *text, GtkWidget *child);
GtkWidget *dbg_heading(const char *text);
GtkWidget *dbg_padded(GtkWidget *w);
/* A dim, wrapped hint with a floor under its width (or the window's minimum
 * height is the label wrapped a word to a line). */
GtkWidget *dbg_hint(const char *text, int width_chars);
/* A monospace label (selectable, left aligned). */
GtkWidget *dbg_mono_label(void);
/* The live refresh does not overwrite a value being typed: while the
 * machine runs, a focused entry keeps what is in it. */
void dbg_set_entry(o2debug *d, GtkEntry *e, const char *text);
/* "$0400", "0x400", "#1024" or bare hex. */
int dbg_parse_num(const char *text, long *out);

void dbg_set_rgb(cairo_t *cr, guint32 rgb, double alpha);
/* The theme's text colour, for lines and labels drawn by hand. */
void dbg_set_fg(cairo_t *cr, GtkWidget *w, double alpha);
/* The accent as a line colour: lightened on a dark theme, the darker red on
 * a light one. */
guint32 dbg_accent_line_rgb(void);
/* Text centred in a cell, in the widget's font (monospace when asked). */
void dbg_draw_centred(cairo_t *cr, GtkWidget *w, const char *text, double x, double y,
                      double cw, double ch, gboolean mono);

/* ---- the Video tab (the 8244/8245) and the EF9340 tab: dbg_video.c ---------- */

typedef struct DbgVideo DbgVideo;
typedef struct DbgEf DbgEf;

/* The tab's widget is returned in *root. */
DbgVideo *dbg_video_new(o2session *session, o2debug *dbg, GtkWidget **root);
/* Redraws what the tab shows, from the machine as it is now. `live` is the
 * machine running (the picture is then redrawn at most every call, and the
 * sections that look at registers are refreshed all the same). Call only
 * while the tab is on screen. */
void dbg_video_refresh(DbgVideo *v, gboolean live);
void dbg_video_free(DbgVideo *v);

DbgEf *dbg_ef_new(o2session *session, o2debug *dbg, GtkWidget **root);
void dbg_ef_refresh(DbgEf *e);
void dbg_ef_free(DbgEf *e);

G_END_DECLS
