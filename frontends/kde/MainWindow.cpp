/*
 * MainWindow -- see MainWindow.h.
 *
 * Plain Qt6 Widgets, deliberately not KDE Frameworks: it picks up Breeze
 * through the platform theme anyway, and staying framework-free keeps this
 * frontend usable outside a KDE session.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "MainWindow.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QStatusBar>
#include <QUrl>
#include <QVBoxLayout>

#include "DisplayWidget.h"
#include "FujiNetWindows.h"
#include "KeyForward.h"
#include "SettingsDialog.h"
#include "SoftDisplayWidget.h"
#include "controllers/ControllersWindow.h"
#include "debugger/DebuggerWindow.h"

namespace {

const char *const kCartFilter =
    "Odyssey 2 / Videopac cartridges (*.bin *.rom *.o2 *.BIN *.ROM *.O2);;All files (*)";

QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

bool envOn(const char *name)
{
    const QByteArray v = qgetenv(name);
    return !v.isEmpty() && v[0] != '0';
}

/* A status line that elides instead of widening the window. */
class ElidedLabel : public QLabel {
public:
    QSize minimumSizeHint() const override { return QSize(40, QLabel::minimumSizeHint().height()); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        const QRect r = contentsRect();
        p.setPen(palette().color(foregroundRole()));
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, r.width()));
    }
};

} // namespace

MainWindow::MainWindow(o2session *session, QWidget *parent)
    : QMainWindow(parent), m_session(session)
{
    setWindowTitle(QStringLiteral("FujiNet Go Odyssey 2"));
    /* the 243 lines three times over, at the TV's 4:3, and the chrome */
    resize(972, 729 + 60);
    setAcceptDrops(true);

    /* the banner over the picture: ROMs the running machine lacks */
    auto *central = new QWidget;
    m_stack = new QVBoxLayout(central);
    m_stack->setContentsMargins(0, 0, 0, 0);
    m_stack->setSpacing(0);
    m_banner = new QFrame;
    m_banner->setStyleSheet(QStringLiteral("QFrame { background: rgba(193, 18, 24, 0.16); "
                                           "border-bottom: 1px solid rgba(193, 18, 24, 0.55); }"));
    auto *bh = new QHBoxLayout(m_banner);
    bh->setContentsMargins(10, 4, 10, 4);
    m_bannerText = new QLabel;
    m_bannerText->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
    auto *bannerImport = new QPushButton(QStringLiteral("Import BIOS..."));
    bannerImport->setFocusPolicy(Qt::NoFocus);
    connect(bannerImport, &QPushButton::clicked, this, [this] { importRoms(false); });
    bh->addWidget(m_bannerText, 1);
    bh->addWidget(bannerImport);
    m_banner->hide();
    m_stack->addWidget(m_banner);

    auto *gpu = new DisplayWidget(session, central);
    m_display = gpu;
    m_stack->addWidget(gpu, 1);
    setCentralWidget(central);
    /* No usable GPU device: fall back to painting in software */
    connect(gpu, &QRhiWidget::renderFailed, this, &MainWindow::useSoftwareDisplay, Qt::QueuedConnection);

    m_dot = new QLabel(QStringLiteral("●"));
    auto *status = new ElidedLabel;
    status->setText(QStringLiteral("Starting..."));
    status->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_status = status;
    statusBar()->addWidget(m_dot);
    statusBar()->addWidget(m_status, 1);

    m_toast = new QLabel(this);
    m_toast->setWordWrap(true);
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setStyleSheet(QStringLiteral(
        "background: rgba(30, 30, 30, 235); color: white; border-radius: 8px; padding: 8px 14px;"));
    m_toast->hide();
    m_toastTimer.setSingleShot(true);
    connect(&m_toastTimer, &QTimer::timeout, m_toast, &QWidget::hide);

    buildMenus();
    applyPicture();

    /* One timer for the news the other threads leave: the gamepad thread
     * cannot call into Qt, so it posts system actions and gamepad events and
     * this takes them. */
    m_padGen = o2session_gamepad_generation(session);
    connect(&m_tick, &QTimer::timeout, this, &MainWindow::tick);
    m_tick.start(100);
    updateStatus();
}

/* ---- starting ------------------------------------------------------------- */

void MainWindow::start()
{
    if (o2session_start(m_session, nullptr) != 0)
        explainRefusal(QStringLiteral("Cannot Start the Console"), local(o2session_last_error(m_session)),
                       [this] { start(); });
    updateStatus();
    updateBanner();
    syncMenus();
    openAtLaunch();
}

/* The env hooks that open the other windows at launch, once the event loop
 * runs: attaching the debugger before the machine exists would not stop it. */
void MainWindow::openAtLaunch()
{
    if (envOn("O2_OPEN_CONTROLLERS"))
        QTimer::singleShot(0, this, [this] { toggleControllers(); });
    if (envOn("O2_OPEN_DEBUGGER"))
        QTimer::singleShot(0, this, [this] { DebuggerWindow::showFor(this, m_session); });
    if (envOn("O2_OPEN_SETTINGS"))
        QTimer::singleShot(0, this, &MainWindow::showSettings);
}

/* ---- menus ---------------------------------------------------------------- */

void MainWindow::buildMenus()
{
    QMenu *machine = menuBar()->addMenu(QStringLiteral("&Machine"));
    machine->addAction(QStringLiteral("&Open Cartridge..."), QKeySequence(Qt::CTRL | Qt::Key_O), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Open Cartridge"), QString(),
                                                       QString::fromUtf8(kCartFilter));
        if (!f.isEmpty()) openCart(f);
    });
    machine->addAction(QStringLiteral("&Eject Cartridge"), this, [this] {
        if (o2session_eject(m_session) != 0)
            QMessageBox::warning(this, QStringLiteral("Eject failed"), local(o2session_last_error(m_session)));
        else
            toast(QStringLiteral("Cartridge ejected: back to the FujiNet CONFIG client"));
        updateStatus();
    });
    machine->addAction(QStringLiteral("&Import Cartridge to SD..."), this, [this] {
        const QString f = QFileDialog::getOpenFileName(this, QStringLiteral("Import Cartridge to SD"), QString(),
                                                       QString::fromUtf8(kCartFilter));
        if (f.isEmpty()) return;
        char dest[1024];
        if (o2session_import_cart_to_sd(m_session, f.toLocal8Bit().constData(), dest, sizeof dest) != 0)
            QMessageBox::warning(this, QStringLiteral("Import failed"), local(o2session_last_error(m_session)));
        else
            toast(QStringLiteral("%1 is on the SD host: boot it from the CONFIG client")
                      .arg(QFileInfo(local(dest)).fileName()));
    });
    machine->addSeparator();
    machine->addAction(QStringLiteral("Import &BIOS..."), this, [this] { importRoms(false); });
    machine->addAction(QStringLiteral("Import BIOS from a &Folder..."), this, [this] { importRoms(true); });
    machine->addSeparator();
    m_pauseAction = machine->addAction(QStringLiteral("&Pause"));
    m_pauseAction->setCheckable(true);
    connect(m_pauseAction, &QAction::triggered, this, [this](bool on) {
        o2session_set_paused(m_session, on ? 1 : 0);
        updateStatus();
    });
    /* Reset acts through its key binding too (F1 by default, remappable); the
     * menu shows the bound key and does not claim it as a shortcut, so the
     * binding stays the one that acts. */
    m_resetAction = machine->addAction(QStringLiteral("&Reset"), this, [this] {
        o2session_reset(m_session);
        toast(QStringLiteral("Console RESET"));
    });
    machine->addAction(QStringLiteral("Reset to &CONFIG"), QKeySequence(Qt::CTRL | Qt::Key_R), this, [this] {
        if (o2session_reset_to_config(m_session) != 0)
            QMessageBox::warning(this, QStringLiteral("Reset failed"), local(o2session_last_error(m_session)));
        else
            toast(QStringLiteral("Back to the FujiNet CONFIG client"));
        updateStatus();
    });
    connect(machine, &QMenu::aboutToShow, this, &MainWindow::syncMenus);
    machine->addSeparator();
    machine->addAction(QStringLiteral("P&references..."), QKeySequence(Qt::CTRL | Qt::Key_Comma), this,
                       &MainWindow::showSettings);
    machine->addSeparator();
    machine->addAction(QStringLiteral("&Quit"), QKeySequence::Quit, this, [this] { close(); });

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    view->addAction(QStringLiteral("&Controllers"), QKeySequence(Qt::Key_F9), this, &MainWindow::toggleControllers);
    view->addAction(QStringLiteral("&Debugger"), QKeySequence(Qt::Key_F12), this,
                    [this] { DebuggerWindow::toggleFor(this, m_session); });
    view->addSeparator();
    m_tvAction = view->addAction(QStringLiteral("&TV Aspect"));
    m_tvAction->setCheckable(true);
    m_tvAction->setToolTip(QStringLiteral("Show the picture across a 4:3 television, as the console does; "
                                          "off shows square pixels"));
    connect(m_tvAction, &QAction::triggered, this, [this](bool on) {
        /* the family's "aspect": 0 the TV's 4:3, 1 square pixels */
        o2session_set_int(m_session, "aspect", on ? 0 : 1);
        applyPicture();
    });
    m_smoothAction = view->addAction(QStringLiteral("&Smooth Scaling"));
    m_smoothAction->setCheckable(true);
    connect(m_smoothAction, &QAction::triggered, this, [this](bool on) {
        o2session_set_int(m_session, "smooth", on ? 1 : 0);
        applyPicture();
    });
    view->addAction(QStringLiteral("&Fullscreen"), QKeySequence(Qt::Key_F11), this, [this] {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });

    QMenu *fuji = menuBar()->addMenu(QStringLiteral("&FujiNet"));
    fuji->addAction(QStringLiteral("&Web UI"), this, [this] {
        if (o2session_fujinet_external(m_session)) {
            toast(QStringLiteral("FujiNet is external: use its own web UI"));
            return;
        }
        if (!o2session_fujinet_running(m_session)) {
            toast(QStringLiteral("FujiNet is off (Preferences > FujiNet)"));
            return;
        }
        fujinet_config_show(this, m_session);
    });
    fuji->addAction(QStringLiteral("Console &Log"), this, [this] {
        if (o2session_fujinet_external(m_session)) {
            toast(QStringLiteral("FujiNet is external: see its own console"));
            return;
        }
        fujinet_log_show(this, m_session);
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(QStringLiteral("&About FujiNet Go Odyssey 2"), this, &MainWindow::showAbout);
    syncMenus();
}

/* The bound key of the console's RESET as the menu's shortcut text (it
 * follows a rebinding), and the state of the checkable items. */
void MainWindow::syncMenus()
{
    const o2_binding b = o2session_binding_get(m_session, O2_TARGET_SWITCH(O2_SW_RESET));
    char key[48] = "";
    if (b.keysym) o2session_keysym_name(b.keysym, key, sizeof key);
    const QString text = QStringLiteral("&Reset");
    m_resetAction->setText(key[0] ? text + QLatin1Char('\t') + local(key) : text);
    m_pauseAction->setChecked(o2session_is_paused(m_session) != 0);
}

void MainWindow::showAbout()
{
    QMessageBox::about(this, QStringLiteral("About FujiNet Go Odyssey 2"),
        QStringLiteral("<b>FujiNet Go Odyssey 2</b> %1<br><br>"
                       "The Magnavox Odyssey 2 and Philips Videopac, with a built-in FujiNet.<br><br>"
                       "The emulator is MAME's Odyssey 2 -- the Intel 8048, the 8244/8245 video "
                       "and sound chip, the Videopac+ G7400's EF9340/41 text chip and The Voice's "
                       "SP0256 (Peter Trauner, Wilbert Pol, hap, Couriersud, Joseph Zbiciak and "
                       "the MAME team, BSD-3-Clause) -- transposed to C. The FujiNet firmware "
                       "and the cartridge are by Thomas Cherryhomes and the FujiNet "
                       "contributors (GPL-3.0-or-later).<br><br>"
                       "Copyright &copy; 2026 Thomas Cherryhomes -- GPL-3.0-or-later<br>"
                       "<a href=\"https://fujinet.online/\">fujinet.online</a> &middot; "
                       "<a href=\"https://github.com/FujiNetWIFI/fujinet-go-o2-desktop/issues\">"
                       "Report an issue</a>")
            .arg(QStringLiteral(O2_VERSION_STRING)));
}

/* ---- picture, settings ----------------------------------------------------- */

void MainWindow::showSettings()
{
    const bool restart = SettingsDialog::run(this, m_session);
    applyPicture();
    syncMenus();
    if (restart) restartSession();
}

void MainWindow::useSoftwareDisplay()
{
    if (dynamic_cast<SoftDisplayWidget *>(m_display)) return;
    auto *old = m_display->widget();
    auto *soft = new SoftDisplayWidget(m_session, centralWidget());
    m_display = soft;
    m_stack->replaceWidget(old, soft);
    delete old;   /* the GPU display, which stops its feed */
    applyPicture();
}

void MainWindow::applyPicture()
{
    const bool tv = o2session_get_int(m_session, "aspect", 0) == 0;
    const bool smooth = o2session_get_int(m_session, "smooth", 0) != 0;
    m_display->setTvAspect(tv);
    m_display->setSmooth(smooth);
    if (m_tvAction) m_tvAction->setChecked(tv);
    if (m_smoothAction) m_smoothAction->setChecked(smooth);
}

/* A machine option changed: a power cycle with the new settings (a full
 * restart when FujiNet, audio or the gamepads were switched). */
void MainWindow::restartSession()
{
    if (o2session_restart(m_session) != 0) {
        explainRefusal(QStringLiteral("Restart failed"), local(o2session_last_error(m_session)),
                       [this] { restartSession(); });
    } else {
        toast(QStringLiteral("Machine options applied (power cycled)"));
    }
    o2ControllersRefresh();
    updateStatus();
    updateBanner();
    syncMenus();
}

void MainWindow::toggleControllers()
{
    o2ControllersToggle(this, m_session);
}

/* ---- the timer ---------------------------------------------------------------- */

void MainWindow::tick()
{
    drainSysactions();
    checkNeedsBios();
    if (++m_ticks % 3 == 0) pollGamepads();
    if (m_ticks % 10 == 0) {
        updateStatus();
        updateBanner();
    }
}

void MainWindow::drainSysactions()
{
    int sa;
    while (o2session_sysaction_take(m_session, &sa)) runSysaction(sa);
}

void MainWindow::runSysaction(int sa)
{
    switch (sa) {
    case O2_SYSACT_RESET_CONFIG:
        o2session_sysaction(m_session, sa);
        toast(QStringLiteral("Back to the FujiNet CONFIG client"));
        updateStatus();
        break;
    case O2_SYSACT_DEBUG_STOP:
        DebuggerWindow::showFor(this, m_session);
        break;
    default: break;
    }
}

void MainWindow::pollGamepads()
{
    const unsigned gen = o2session_gamepad_generation(m_session);
    if (gen == m_padGen) return;
    m_padGen = gen;
    char msg[160];
    if (o2session_gamepad_last_event(m_session, msg, sizeof msg) > 0) toast(local(msg));
}

void MainWindow::updateStatus()
{
    QString text;
    bool on = false;
    const QString cart = local(o2session_cart_path(m_session));
    if (!o2session_is_running(m_session)) {
        text = QStringLiteral("Stopped");
    } else {
        char st[160];
        o2session_cart_status(m_session, st, sizeof st);
        on = o2session_cart_link_up(m_session) == 1;
        text = QStringLiteral("FujiNet: %1").arg(local(st));
        if (!cart.isEmpty())
            text = QStringLiteral("%1 — %2").arg(QFileInfo(cart).fileName(), text);
        else if (!o2session_cart_booted_game(m_session))
            text = QStringLiteral("CONFIG — %1").arg(text);
        if (o2session_is_paused(m_session)) text = QStringLiteral("Paused — %1").arg(text);
    }
    m_status->setText(text);
    m_status->setToolTip(text);
    m_dot->setStyleSheet(on ? QStringLiteral("color: %1;").arg(o2AccentTextColor(this).name())
                            : QStringLiteral("color: gray;"));
}

/* The banner under the menu: what the running machine lacks and Import BIOS
 * would give it. The 8244's character set is the one every machine needs to
 * show text. */
void MainWindow::updateBanner()
{
    const unsigned missing = o2session_missing(m_session);
    QStringList said;
    if (missing & O2_MISSING_FONT)
        said << QStringLiteral("Character set not imported — text will be blank.");
    if (missing & O2_MISSING_VOICE)
        said << QStringLiteral("The Voice's speech ROMs are not imported — it will be silent.");
    const QString text = said.join(QLatin1Char(' '));
    if (m_bannerText->text() != text) m_bannerText->setText(text);
    m_banner->setVisible(!text.isEmpty());
}

/* ---- toasts ---------------------------------------------------------------- */

void MainWindow::toast(const QString &text)
{
    m_toast->setText(text);
    placeToast();
    m_toast->show();
    m_toast->raise();
    m_toastTimer.start(text.size() > 80 ? 6000 : 3500);
}

void MainWindow::placeToast()
{
    const QRect c = centralWidget()->geometry();
    /* one line if it fits, wrapped within the picture otherwise */
    const QMargins pad = m_toast->contentsMargins();
    const int w = qMin(m_toast->fontMetrics().horizontalAdvance(m_toast->text()) + pad.left() + pad.right() + 40,
                       qMin(600, c.width() - 32));
    m_toast->setFixedSize(w, m_toast->heightForWidth(w));
    m_toast->move(c.x() + (c.width() - m_toast->width()) / 2, c.bottom() - m_toast->height() - 16);
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QMainWindow::resizeEvent(e);
    if (m_toast->isVisible()) placeToast();
}

/* ---- ROMs and cartridges -------------------------------------------------------- */

void MainWindow::explainRefusal(const QString &title, const QString &why, std::function<void()> retry)
{
    /* the session's refusals a ROM import cures all say where to find it */
    if (!why.contains(QStringLiteral("Import BIOS"))) {
        QMessageBox::warning(this, title, why);
        return;
    }
    QString headline, detail;
    if (why.contains(QStringLiteral("Voice"))) {
        headline = QStringLiteral("The Voice's speech ROMs are needed");
        detail = why;
    } else if (why.contains(QStringLiteral("G7400"))) {
        headline = QStringLiteral("The Videopac+ G7400 needs its BIOS");
        detail = QStringLiteral("The Videopac+ runs only on its own BIOS, which is not part of this app. "
                                "Import your own copy and the console starts.");
    } else {
        headline = QStringLiteral("This cartridge needs the Odyssey 2 BIOS");
        detail = QStringLiteral("Games are written for the console's BIOS, which is not part of this app; "
                                "CONFIG and the FujiNet apps run without it. Import your own copy "
                                "(Machine > Import BIOS...) and the console starts the game again.");
    }
    /* not exec(): this can run from the timer, and a nested event loop would
     * run it again */
    auto *box = new QMessageBox(QMessageBox::Information, title, headline, QMessageBox::NoButton, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setInformativeText(detail);
    QPushButton *import = box->addButton(QStringLiteral("Import BIOS..."), QMessageBox::AcceptRole);
    box->addButton(QStringLiteral("Not Now"), QMessageBox::RejectRole);
    box->setDefaultButton(import);
    connect(box, &QMessageBox::buttonClicked, this, [this, import, retry](QAbstractButton *b) {
        if (b != import) return;
        if (SettingsDialog::importRoms(this, m_session, false) <= 0) return;
        if (retry) retry(); else afterImport();
    });
    box->open();
}

void MainWindow::importRoms(bool folder, const QString &path)
{
    if (SettingsDialog::importRoms(this, m_session, folder, path) > 0) afterImport();
}

/* The ROMs are read at power-up: power-cycle onto what was running (a
 * cartridge file again, else CONFIG). */
void MainWindow::afterImport()
{
    if (!o2session_is_running(m_session)) {
        start();
        return;
    }
    const QString cart = local(o2session_cart_path(m_session));
    const int rc = cart.isEmpty() ? o2session_reset_to_config(m_session)
                                  : o2session_load_cart(m_session, cart.toLocal8Bit().constData());
    if (rc != 0)
        QMessageBox::warning(this, QStringLiteral("Could not restart"), local(o2session_last_error(m_session)));
    else
        toast(QStringLiteral("ROMs imported: the console was power-cycled to use them"));
    updateStatus();
    updateBanner();
}

/* CONFIG booted a game from the network with no BIOS to run it: say so
 * once, with the way out. Shown again only after the flag has cleared (a
 * power cycle) and come back. */
void MainWindow::checkNeedsBios()
{
    if (!o2session_needs_bios(m_session)) {
        m_needsBiosShown = false;
        return;
    }
    if (m_needsBiosShown) return;
    m_needsBiosShown = true;
    explainRefusal(QStringLiteral("BIOS needed"),
                   QStringLiteral("This cartridge needs the Odyssey 2 BIOS: Machine > Import BIOS..."), nullptr);
}

void MainWindow::openCart(const QString &path)
{
    if (o2session_load_cart(m_session, path.toLocal8Bit().constData()) != 0) {
        explainRefusal(QStringLiteral("Could not open"), local(o2session_last_error(m_session)),
                       [this, path] { openCart(path); });
        return;
    }
    toast(QStringLiteral("Running %1").arg(QFileInfo(path).fileName()));
    updateStatus();
}

void MainWindow::loadMedia(const QString &path)
{
    const QByteArray p = path.toLocal8Bit();
    /* a ROM first: one may well be named .bin, as a cartridge can be */
    if (o2session_media_is_rom(p.constData())) {
        importRoms(false, path);
        return;
    }
    if (o2session_media_is_cartridge(p.constData())) {
        openCart(path);
        return;
    }
    char dest[1024];
    if (o2session_import_media(m_session, p.constData(), dest, sizeof dest) != 0) {
        QMessageBox::warning(this, QStringLiteral("Import failed"), local(o2session_last_error(m_session)));
        return;
    }
    toast(QStringLiteral("Copied to FujiNet's SD folder: mount it from the CONFIG client"));
}

void MainWindow::activateFor(const QString &path)
{
    setWindowState(windowState() & ~Qt::WindowMinimized);
    show();
    raise();
    activateWindow();
    if (!path.isEmpty()) loadMedia(path);
}

/* ---- keys, drops, focus ------------------------------------------------------------- */

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    if (e->key() == Qt::Key_F9) { toggleControllers(); return; }
    if (e->key() == Qt::Key_F12) { DebuggerWindow::toggleFor(this, m_session); return; }
    if (o2IsAppKey(e)) { QMainWindow::keyPressEvent(e); return; }

    const uint32_t ks = o2KeysymFromQt(e);
    if (!ks) { QMainWindow::keyPressEvent(e); return; }

    const int sa = o2session_key_sysaction(m_session, ks);
    if (sa >= 0) {
        if (!m_sysactDown[sa]) { m_sysactDown[sa] = true; runSysaction(sa); }
        return;
    }
    if (!o2session_key(m_session, ks, 1)) QMainWindow::keyPressEvent(e);
}

void MainWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = o2KeysymFromQt(e);
    if (!ks) { QMainWindow::keyReleaseEvent(e); return; }
    const int sa = o2session_key_sysaction(m_session, ks);
    if (sa >= 0) { m_sysactDown[sa] = false; return; }
    if (!o2session_key(m_session, ks, 0)) QMainWindow::keyReleaseEvent(e);
}

bool MainWindow::event(QEvent *e)
{
    if (e->type() == QEvent::WindowDeactivate) {
        o2session_release_all(m_session);
        for (bool &d : m_sysactDown) d = false;
    }
    return QMainWindow::event(e);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty()) loadMedia(path);
}

void MainWindow::closeEvent(QCloseEvent *e)
{
    m_tick.stop();
    m_toastTimer.stop();
    QMainWindow::closeEvent(e);
}
