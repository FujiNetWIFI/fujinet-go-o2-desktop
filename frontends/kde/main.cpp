/*
 * FujiNet Go Odyssey 2 -- the KDE (Qt6 Widgets) frontend.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QApplication>
#include <QIcon>

#include "MainWindow.h"
#include "o2session.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("fujinet-go-o2-kde"));
    app.setApplicationDisplayName(QStringLiteral("FujiNet Go Odyssey 2"));
    app.setDesktopFileName(QStringLiteral("online.fujinet.go.o2.kde"));
    /* The installed icon is named after the desktop-entry id; running out
     * of the build tree, the in-tree artwork stands in. */
    {
        QIcon icon = QIcon::fromTheme(QStringLiteral("online.fujinet.go.o2.kde"));
        if (icon.isNull()) {
            QIcon::setThemeSearchPaths(QIcon::themeSearchPaths()
                                       << QStringLiteral(O2_SOURCE_ICON_DIR));
            icon = QIcon::fromTheme(QStringLiteral("fujinet-go-o2"));
        }
        if (!icon.isNull()) app.setWindowIcon(icon);
    }

    o2session *session = o2session_new(nullptr);
    if (!session) {
        qCritical("Could not create the session (unusable config/data dirs?)");
        return 1;
    }

    int rc;
    /* The windows go before the session does: the debugger and the
     * Keypad window still call into it as they are hidden, and the main
     * window can close with either of them up. */
    {
        MainWindow win(session);

        /* A path on the command line is routed like a dropped file: a
         * cartridge boots in place of CONFIG; a BIOS (which may well be
         * named .bin, as a cartridge can be) is imported, and anything else
         * goes to the SD folder, once the window is up to say so. */
        const char *arg = argc > 1 ? argv[1] : nullptr;
        const bool argIsCart = arg && !o2session_media_is_bios(arg)
                               && o2session_media_is_cartridge(arg);

        o2session_start_opts opts;
        o2session_default_opts(session, &opts);
        if (argIsCart) opts.cart_path = arg;

        if (o2session_start(session, &opts) != 0)
            qWarning("%s", o2session_last_error(session));
        win.show();
        if (arg && !argIsCart) win.loadMedia(QString::fromLocal8Bit(arg));

        rc = app.exec();
    }
    o2session_stop(session);
    o2session_free(session);
    return rc;
}
