/*
 * SettingsDialog -- see SettingsDialog.h.
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <cstring>
#include <vector>

namespace {

const char *const kBiosFilter =
    "Magnavox Odyssey 2 BIOS (*.bin *.rom *.ic11 *.ic12 *.ic13 *.ic14 *.BIN *.ROM *.IC11 *.IC12 *.IC13 *.IC14);;"
    "All files (*)";

QCheckBox *check(const char *text, const char *tip, o2session *s, const char *key, int def)
{
    auto *b = new QCheckBox(QString::fromUtf8(text));
    b->setToolTip(QString::fromUtf8(tip));
    b->setChecked(o2session_get_int(s, key, def) != 0);
    return b;
}

bool commit(o2session *s, const char *key, int def, int value)
{
    if (o2session_get_int(s, key, def) == value) return false;
    o2session_set_int(s, key, value);
    return true;
}

/* Whether the BIOS table's image `name` is imported. */
bool imported(o2session *s, const char *name)
{
    for (int i = 0; i < o2session_bios_count(); ++i) {
        const o2_bios_info *b = o2session_bios_info(i);
        if (b && !strcmp(b->name, name)) return o2session_bios_available(s, i) != 0;
    }
    return false;
}

/* What is imported, in a line: the console runs the system ROM (ic11 and
 * ic12), which the 2K set, the 1K dump or the two ROMs one by one supply. */
QString biosStatus(o2session *s)
{
    if (o2session_have_bios(s)) {
        if (imported(s, "o2")) return QStringLiteral("Imported (2K set)");
        if (imported(s, "system")) return QStringLiteral("Imported (1K system ROM)");
        return QStringLiteral("Imported (ROMs ic11 and ic12)");
    }
    if (imported(s, "84932.ic11") || imported(s, "84933.ic12"))
        return QStringLiteral("Incomplete — the console needs both ic11 and ic12; import the other");
    return QStringLiteral("Not imported — CONFIG and FujiNet apps run without it; games need it");
}

} // namespace

int SettingsDialog::importBios(QWidget *parent, o2session *session, const QString &path)
{
    QString f = path;
    if (f.isEmpty())
        f = QFileDialog::getOpenFileName(parent, QStringLiteral("Import BIOS"), QString(),
                                         QString::fromUtf8(kBiosFilter));
    if (f.isEmpty()) return -1;
    char msg[512];
    const int idx = o2session_import_bios(session, f.toLocal8Bit().constData(), msg, sizeof msg);
    const QString text = QString::fromUtf8(msg[0] ? msg : o2session_last_error(session));
    /* half of the system ROM imported is not a BIOS yet: a warning, as the
     * message says what is missing */
    if (idx < 0)
        QMessageBox::warning(parent, QStringLiteral("Import failed"), text);
    else if (!o2session_have_bios(session))
        QMessageBox::warning(parent, QStringLiteral("Import BIOS"), text);
    else
        QMessageBox::information(parent, QStringLiteral("Import BIOS"), text);
    return idx;
}

bool SettingsDialog::run(QWidget *parent, o2session *session)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(QStringLiteral("Preferences"));
    dlg.setMinimumWidth(640);
    auto *outer = new QVBoxLayout(&dlg);

    /* Machine (power cycle) */
    auto *machine = new QGroupBox(QStringLiteral("Machine (applied by power-cycling the console)"));
    auto *mform = new QFormLayout(machine);
    /* not word-wrapped: a form row does not grow for a wrapped label's
     * second line, so the dialog widens to the line instead */
    auto *biosLabel = new QLabel(biosStatus(session));
    biosLabel->setToolTip(QStringLiteral("Never shipped, and only games need it: the app's own "
                                         "stand-in boots CONFIG and the FujiNet apps."));
    auto *importBtn = new QPushButton(QStringLiteral("Import BIOS..."));
    auto *biosRow = new QHBoxLayout;
    biosRow->addWidget(biosLabel, 1);
    biosRow->addWidget(importBtn);
    mform->addRow(QStringLiteral("BIOS"), biosRow);
    QCheckBox *useBios = check("Use the imported BIOS",
                               "Off always boots the stand-in, which runs CONFIG and the FujiNet "
                               "apps but no games", session, "use_bios", 1);
    mform->addRow(useBios);
    outer->addWidget(machine);

    /* Importing is a file operation, done now; the BIOS boots at the next
     * power cycle, which OK then gives it. */
    bool didImport = false;
    QObject::connect(importBtn, &QPushButton::clicked, &dlg, [&, session]() {
        if (importBios(&dlg, session) < 0) return;
        didImport = true;
        biosLabel->setText(biosStatus(session));
    });

    /* Display (live) */
    auto *picture = new QGroupBox(QStringLiteral("Display"));
    auto *picForm = new QFormLayout(picture);
    /* "aspect" is 0 for the TV's 4:3 (the default), 1 for square pixels:
     * the box shows the first */
    auto *tv = new QCheckBox(QStringLiteral("TV aspect"));
    tv->setToolTip(QStringLiteral("The picture across a 4:3 television, as the console shows it; "
                                  "off shows square pixels (a tall, narrow picture)"));
    tv->setChecked(o2session_get_int(session, "aspect", 0) == 0);
    QCheckBox *smooth = check("Smooth scaling", "Filter the picture when scaling it; off keeps the pixels sharp", session, "smooth", 0);
    picForm->addRow(tv);
    picForm->addRow(smooth);
    QObject::connect(tv, &QCheckBox::toggled, &dlg,
                     [=](bool on) { o2session_set_int(session, "aspect", on ? 0 : 1); });
    QObject::connect(smooth, &QCheckBox::toggled, &dlg,
                     [=](bool on) { o2session_set_int(session, "smooth", on ? 1 : 0); });
    outer->addWidget(picture);

    /* Audio: the device needs a restart, the volume is live */
    auto *audio = new QGroupBox(QStringLiteral("Audio"));
    auto *audioForm = new QFormLayout(audio);
    QCheckBox *snd = check("Audio output (applied by restarting the session)", "Open the system audio device", session, "enable_audio", 1);
    audioForm->addRow(snd);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(o2session_get_int(session, "volume", 100));
    QObject::connect(volume, &QSlider::valueChanged, &dlg, [=](int v) { o2session_set_volume(session, v); });
    audioForm->addRow(QStringLiteral("Volume"), volume);
    outer->addWidget(audio);

    /* Input: the gamepad thread needs a restart, the rest is live */
    auto *input = new QGroupBox(QStringLiteral("Input"));
    auto *inForm = new QFormLayout(input);
    QCheckBox *gp = check("Gamepads (applied by restarting the session)", "Poll USB/Bluetooth gamepads", session, "enable_gamepad", 1);
    inForm->addRow(gp);
    QCheckBox *aj = check("Analog sticks steer too", "The left stick presses 2 4 6 8 (and 1 3 7 9 on the diagonals), as the D-pad does", session, "analog_joystick", 1);
    QObject::connect(aj, &QCheckBox::toggled, &dlg, [=](bool on) { o2session_set_analog(session, on ? 1 : 0); });
    inForm->addRow(aj);
    outer->addWidget(input);

    /* Gamepads: one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to keypads in connection order unless chosen here)"));
    auto *padForm = new QFormLayout(pads);
    std::vector<QWidget *> padRows;
    unsigned seenGen = o2session_gamepad_generation(session) + 1;
    auto rebuildPads = [&, padForm, session]() mutable {
        for (QWidget *w : padRows) { padForm->removeRow(w); }
        padRows.clear();
        const int n = o2session_gamepad_count(session);
        if (n == 0) {
            auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
            padForm->addRow(l);
            padRows.push_back(l);
            return;
        }
        for (int i = 0; i < n && i < 8; ++i) {
            char name[128];
            o2session_gamepad_name(session, i, name, sizeof name);
            auto *box = new QComboBox;
            box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Keypad A"), QStringLiteral("Keypad B") });
            box->setCurrentIndex(o2session_gamepad_assignment(session, i) + 1);
            const int eff = o2session_gamepad_effective_port(session, i);
            box->setToolTip(eff >= 0 ? QStringLiteral("Driving keypad %1").arg(QChar(eff ? 'B' : 'A'))
                                     : QStringLiteral("Driving no keypad"));
            QObject::connect(box, &QComboBox::currentIndexChanged, box, [=](int idx) {
                o2session_gamepad_assign(session, i, idx - 1);
            });
            padForm->addRow(QString::fromUtf8(name), box);
            padRows.push_back(box);
        }
    };
    auto *padTimer = new QTimer(&dlg);
    QObject::connect(padTimer, &QTimer::timeout, &dlg, [&, session]() mutable {
        const unsigned gen = o2session_gamepad_generation(session);
        if (gen != seenGen) { seenGen = gen; rebuildPads(); }
    });
    padTimer->start(1000);
    seenGen = o2session_gamepad_generation(session);
    rebuildPads();
    outer->addWidget(pads);

    /* FujiNet (restart) */
    auto *fujiBox = new QGroupBox(QStringLiteral("FujiNet"));
    auto *fform = new QFormLayout(fujiBox);
    QCheckBox *fuji = check("Enable FujiNet (applied by restarting the session)", "Run the in-process FujiNet the cartridge dials into. Off means no network and a link-down CONFIG client.", session, "enable_fujinet", 1);
    fform->addRow(fuji);
    QCheckBox *external = check("Use external FujiNet (applied by restarting the session)", "Skip the built-in FujiNet and dial another instance at this address. Set that instance's BoIP listener to match.", session, "fujinet_external", 0);
    fform->addRow(external);
    auto *externalAddr = new QLineEdit(QString::fromUtf8(o2session_get_str(session, "fujinet_external_addr", "")));
    externalAddr->setPlaceholderText(QStringLiteral("127.0.0.1:11520"));
    externalAddr->setEnabled(external->isChecked());
    QObject::connect(external, &QCheckBox::toggled, externalAddr, &QWidget::setEnabled);
    fform->addRow(QStringLiteral("Address:port"), externalAddr);
    const QString url = QString::fromUtf8(o2session_fujinet_webui_url(session));
    auto *web = new QLabel(o2session_fujinet_external(session) ? QStringLiteral("FujiNet is external") : url.toHtmlEscaped());
    web->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *open = new QPushButton(QStringLiteral("Open"));
    open->setToolTip(QStringLiteral("Open FujiNet's web UI in the browser"));
    open->setEnabled(!o2session_fujinet_external(session));
    QObject::connect(open, &QPushButton::clicked, &dlg, [url] { QDesktopServices::openUrl(QUrl(url)); });
    auto *webRow = new QHBoxLayout;
    webRow->addWidget(web, 1);
    webRow->addWidget(open);
    fform->addRow(QStringLiteral("Web UI"), webRow);
    outer->addWidget(fujiBox);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        char host[64];
        int port;
        if (external->isChecked() &&
            o2session_parse_hostport(externalAddr->text().trimmed().toUtf8().constData(), host, sizeof host, &port) != 0) {
            QMessageBox::warning(&dlg, QStringLiteral("External FujiNet"),
                                 QStringLiteral("\"%1\" is not an address:port. Example: 127.0.0.1:11520")
                                     .arg(externalAddr->text()));
            externalAddr->setFocus();
            return;
        }
        dlg.accept();
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    outer->addWidget(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    bool changed = didImport;
    changed |= commit(session, "use_bios", 1, useBios->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_fujinet", 1, fuji->isChecked() ? 1 : 0);
    changed |= commit(session, "fujinet_external", 0, external->isChecked() ? 1 : 0);
    {
        const QByteArray addr = externalAddr->text().trimmed().toUtf8();
        if (addr != QByteArray(o2session_get_str(session, "fujinet_external_addr", ""))) {
            o2session_set_str(session, "fujinet_external_addr", addr.constData());
            changed = true;
        }
    }
    changed |= commit(session, "enable_audio", 1, snd->isChecked() ? 1 : 0);
    changed |= commit(session, "enable_gamepad", 1, gp->isChecked() ? 1 : 0);
    return changed;
}
