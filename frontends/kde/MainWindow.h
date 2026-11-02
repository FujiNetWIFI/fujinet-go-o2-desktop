/*
 * The main window.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QLabel>
#include <QMainWindow>
#include <QTimer>
#include <functional>

#include "o2session.h"

class DisplayView;
class QAction;
class QBoxLayout;
class QFrame;
class QPushButton;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(o2session *session, QWidget *parent = nullptr);

    /* Starts the session with the saved settings: FujiNet first, then the
     * machine. A start that fails for want of a ROM offers Import BIOS. */
    void start();
    /* A dropped (or command-line) file: a ROM (BIOS, character set, speech,
     * or a zip of them) is imported, a cartridge is opened, anything else is
     * copied to FujiNet's SD folder. */
    void loadMedia(const QString &path);
    /* A second launch hands over to this window: raise it, and open what it
     * was asked to. */
    void activateFor(const QString &path);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    void closeEvent(QCloseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override;

private:
    void buildMenus();
    void syncMenus();
    void tick();
    void updateStatus();
    void updateBanner();
    void drainSysactions();
    void runSysaction(int sa);
    void checkNeedsBios();
    void pollGamepads();
    void toast(const QString &text);
    void placeToast();

    void openCart(const QString &path);
    void importRoms(bool folder, const QString &path = QString());
    void afterImport();
    /* A refusal ROMs would cure gets the alert with Import BIOS...; any
     * other is a plain warning. `retry` runs after a successful import
     * (empty: power-cycle onto what was running). */
    void explainRefusal(const QString &title, const QString &why, std::function<void()> retry);

    void toggleControllers();
    void useSoftwareDisplay();
    void applyPicture();
    void showSettings();
    void restartSession();
    void showAbout();
    void openAtLaunch();

    o2session *m_session;
    DisplayView *m_display = nullptr;
    QBoxLayout *m_stack = nullptr;
    QFrame *m_banner = nullptr;
    QLabel *m_bannerText = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_dot = nullptr;
    QLabel *m_toast = nullptr;
    QTimer m_toastTimer;
    QTimer m_tick;
    int m_ticks = 0;
    QAction *m_tvAction = nullptr;
    QAction *m_smoothAction = nullptr;
    QAction *m_pauseAction = nullptr;
    /* the Machine menu's console control, labelled with its bound key */
    QAction *m_resetAction = nullptr;
    unsigned m_padGen = 0;
    /* the "needs the BIOS" alert is up, or was shown for this power cycle */
    bool m_needsBiosShown = false;
    bool m_sysactDown[O2_SYSACT_COUNT] = {};
};
