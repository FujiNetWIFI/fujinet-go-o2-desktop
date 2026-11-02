/*
 * SettingsDialog -- see SettingsDialog.h.
 *
 * Pages, like the GNOME and Windows frontends: Machine, ROMs, Display,
 * Audio, Input, FujiNet. The settings keys are the session's (the shared
 * settings.ini): machine use_bios voice aspect smooth enable_audio volume
 * enable_gamepad analog_joystick enable_fujinet fujinet_external
 * fujinet_external_addr.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSlider>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <vector>

namespace {

const char *const kRomFilter =
    "BIOS images and ROM sets (*.zip *.rom *.bin *.ZIP *.ROM *.BIN);;All files (*)";

QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

/* A hint under a row: word-wrapped, dimmed. */
QLabel *note(const QString &text)
{
    auto *l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color: palette(placeholder-text);"));
    return l;
}

QCheckBox *check(const QString &text, const QString &tip, o2session *s, const char *key, int def)
{
    auto *b = new QCheckBox(text);
    b->setToolTip(tip);
    b->setChecked(o2session_get_int(s, key, def) != 0);
    return b;
}

/* A page that scrolls when the dialog is smaller than it. */
QWidget *scrolled(QWidget *page)
{
    auto *area = new QScrollArea;
    area->setWidget(page);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    return area;
}

/* What needs a power cycle to take effect. */
struct RestartState {
    int machine, useBios, voice, fujinet, external, audio, gamepad;
    QString addr;
    bool operator==(const RestartState &o) const
    {
        return machine == o.machine && useBios == o.useBios && voice == o.voice && fujinet == o.fujinet
               && external == o.external && audio == o.audio && gamepad == o.gamepad && addr == o.addr;
    }
};

RestartState restartState(o2session *s)
{
    return { o2session_get_int(s, "machine", O2_MACHINE_ODYSSEY2), o2session_get_int(s, "use_bios", 1),
             o2session_get_int(s, "voice", 0), o2session_get_int(s, "enable_fujinet", 1),
             o2session_get_int(s, "fujinet_external", 0), o2session_get_int(s, "enable_audio", 1),
             o2session_get_int(s, "enable_gamepad", 1),
             local(o2session_get_str(s, "fujinet_external_addr", "")) };
}

class PrefsDialog : public QDialog {
public:
    PrefsDialog(QWidget *parent, o2session *session);
    bool restartNeeded() const { return m_imported || !(m_start == restartState(m_s)); }

protected:
    /* Closing with the external FujiNet switched on and an address that is
     * not host:port would only fail at the restart: say so now. */
    void done(int r) override
    {
        char host[64];
        int port;
        if (m_external->isChecked() &&
            o2session_parse_hostport(m_addr->text().trimmed().toUtf8().constData(), host, sizeof host, &port) != 0) {
            QMessageBox::warning(this, QStringLiteral("External FujiNet"),
                                 QStringLiteral("\"%1\" is not an address:port. Example: 127.0.0.1:%2")
                                     .arg(m_addr->text()).arg(O2SESSION_BOIP_PORT));
            m_addr->setFocus();
            return;
        }
        QDialog::done(r);
    }

private:
    QWidget *machinePage();
    QWidget *romsPage();
    QWidget *displayPage();
    QWidget *audioPage();
    QWidget *inputPage();
    QWidget *fujinetPage();
    void refreshMachineNotes();
    void refreshRoms();
    void rebuildPads();
    void importRomsAndRefresh(bool folder);

    o2session *m_s;
    RestartState m_start;
    bool m_imported = false;

    QComboBox *m_machine = nullptr;
    QLabel *m_machineNote = nullptr, *m_voiceNote = nullptr;
    std::vector<QLabel *> m_romState;
    QFormLayout *m_padForm = nullptr;
    std::vector<QWidget *> m_padRows;
    unsigned m_padGen = 0;
    QCheckBox *m_external = nullptr;
    QLineEdit *m_addr = nullptr;
};

PrefsDialog::PrefsDialog(QWidget *parent, o2session *session)
    : QDialog(parent), m_s(session), m_start(restartState(session))
{
    setWindowTitle(QStringLiteral("Preferences"));
    resize(660, 560);
    auto *outer = new QVBoxLayout(this);
    auto *tabs = new QTabWidget;
    tabs->addTab(scrolled(machinePage()), QStringLiteral("Machine"));
    tabs->addTab(scrolled(romsPage()), QStringLiteral("ROMs"));
    tabs->addTab(scrolled(displayPage()), QStringLiteral("Display"));
    tabs->addTab(scrolled(audioPage()), QStringLiteral("Audio"));
    tabs->addTab(scrolled(inputPage()), QStringLiteral("Input"));
    tabs->addTab(scrolled(fujinetPage()), QStringLiteral("FujiNet"));
    outer->addWidget(tabs, 1);
    outer->addWidget(note(QStringLiteral("Changes apply as you make them; the machine, BIOS, Voice, audio, "
                                         "gamepad and FujiNet options take effect by power-cycling the console "
                                         "when this window closes.")));
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::accept);
    outer->addWidget(buttons);
    refreshMachineNotes();
    refreshRoms();
}

/* ---- Machine --------------------------------------------------------------- */

QWidget *PrefsDialog::machinePage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);

    m_machine = new QComboBox;
    for (int i = 0; i < O2_MACHINE_COUNT; ++i)
        m_machine->addItem(local(o2_machine_name(i)));
    const int cur = o2session_get_int(m_s, "machine", O2_MACHINE_ODYSSEY2);
    m_machine->setCurrentIndex(cur >= 0 && cur < O2_MACHINE_COUNT ? cur : 0);
    connect(m_machine, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (idx < 0) return;
        o2session_set_int(m_s, "machine", idx);
        refreshMachineNotes();
    });
    form->addRow(QStringLiteral("Console"), m_machine);
    m_machineNote = note(QString());
    form->addRow(m_machineNote);

    auto *bios = check(QStringLiteral("Use the imported BIOS"),
                       QStringLiteral("Off always boots the app's own stand-in, which runs CONFIG and the "
                                      "FujiNet apps but no games"), m_s, "use_bios", 1);
    connect(bios, &QCheckBox::toggled, this, [this](bool on) {
        o2session_set_int(m_s, "use_bios", on ? 1 : 0);
    });
    form->addRow(bios);
    form->addRow(note(QStringLiteral("The BIOS is never shipped, and only games need it: CONFIG and the "
                                     "FujiNet apps boot on the app's own stand-in.")));

    auto *voice = check(QStringLiteral("The Voice in front of the cartridge"),
                        QStringLiteral("Plug the speech module in front of the cartridge. It needs its "
                                       "speech ROMs (ROMs page)"), m_s, "voice", 0);
    connect(voice, &QCheckBox::toggled, this, [this](bool on) {
        o2session_set_int(m_s, "voice", on ? 1 : 0);
        refreshMachineNotes();
    });
    form->addRow(voice);
    m_voiceNote = note(QString());
    form->addRow(m_voiceNote);
    return page;
}

void PrefsDialog::refreshMachineNotes()
{
    const int machine = m_machine->currentIndex();
    QString text = o2_machine_is_pal(machine) ? QStringLiteral("PAL, 50 Hz.") : QStringLiteral("NTSC, 60 Hz.");
    if (machine == O2_MACHINE_VIDEOPAC_PLUS) {
        text += QStringLiteral(" The Videopac+ G7400 has its own BIOS and a text chip (EF9340).");
        if (!o2session_have_bios(m_s, machine))
            text += QStringLiteral(" Its BIOS is not imported: import it on the ROMs page before closing this "
                                   "window, or the console will not start.");
    }
    m_machineNote->setText(text);
    const bool wanted = o2session_get_int(m_s, "voice", 0) != 0;
    m_voiceNote->setText(wanted && !o2session_have_voice(m_s)
                             ? QStringLiteral("The speech ROMs are not imported: The Voice would be silent.")
                             : QString());
    m_voiceNote->setVisible(!m_voiceNote->text().isEmpty());
}

/* ---- ROMs ------------------------------------------------------------------ */

QWidget *PrefsDialog::romsPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->addWidget(note(QStringLiteral("None of these ROMs is part of the app. Import your own dumps from "
                                     "files, a zip or a folder (MAME's videopac, i8244 and videopacp sets "
                                     "work); each is recognised by its size and checksum, whatever it is "
                                     "called. Kept in:")));
    auto *path = new QLabel(local(o2session_roms_path(m_s)));
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    path->setWordWrap(true);
    v->addWidget(path);

    auto *row = new QHBoxLayout;
    auto *files = new QPushButton(QStringLiteral("Import BIOS..."));
    auto *folder = new QPushButton(QStringLiteral("Import from a Folder..."));
    connect(files, &QPushButton::clicked, this, [this] { importRomsAndRefresh(false); });
    connect(folder, &QPushButton::clicked, this, [this] { importRomsAndRefresh(true); });
    row->addWidget(files);
    row->addWidget(folder);
    row->addStretch(1);
    v->addLayout(row);

    /* one group per `group`, each ROM imported or not */
    QFormLayout *form = nullptr;
    QString last;
    for (int i = 0; i < o2session_rom_item_count(); ++i) {
        const o2_rom_item *item = o2session_rom_item(i);
        if (!item) continue;
        const QString group = local(item->group);
        if (!form || group != last) {
            auto *box = new QGroupBox(group);
            form = new QFormLayout(box);
            v->addWidget(box);
            last = group;
        }
        auto *name = new QLabel(local(item->name));
        name->setToolTip(QStringLiteral("%1, %2 bytes, CRC-32 %3").arg(local(item->file)).arg(item->size)
                             .arg(item->crc, 8, 16, QLatin1Char('0')));
        auto *state = new QLabel;
        form->addRow(name, state);
        m_romState.push_back(state);
    }
    v->addStretch(1);
    return page;
}

void PrefsDialog::refreshRoms()
{
    for (size_t i = 0; i < m_romState.size(); ++i) {
        const bool have = o2session_rom_item_available(m_s, (int)i) != 0;
        m_romState[i]->setText(have ? QStringLiteral("Imported") : QStringLiteral("Not imported"));
        m_romState[i]->setStyleSheet(have ? QString() : QStringLiteral("color: palette(placeholder-text);"));
    }
    if (m_machineNote) refreshMachineNotes();
}

void PrefsDialog::importRomsAndRefresh(bool folder)
{
    if (SettingsDialog::importRoms(this, m_s, folder) > 0) m_imported = true;
    refreshRoms();
}

/* ---- Display, Audio, Input ---------------------------------------------------- */

QWidget *PrefsDialog::displayPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    /* "aspect" is 0 for the TV's 4:3 (the default), 1 for square pixels:
     * the box shows the first */
    auto *tv = new QCheckBox(QStringLiteral("TV aspect"));
    tv->setToolTip(QStringLiteral("Show the picture across a 4:3 television, as the console does; off shows "
                                  "square pixels"));
    tv->setChecked(o2session_get_int(m_s, "aspect", 0) == 0);
    connect(tv, &QCheckBox::toggled, this, [this](bool on) { o2session_set_int(m_s, "aspect", on ? 0 : 1); });
    auto *smooth = check(QStringLiteral("Smooth scaling"),
                         QStringLiteral("Filter the picture when scaling it; off keeps the pixels sharp"),
                         m_s, "smooth", 0);
    connect(smooth, &QCheckBox::toggled, this, [this](bool on) { o2session_set_int(m_s, "smooth", on ? 1 : 0); });
    form->addRow(tv);
    form->addRow(smooth);
    return page;
}

QWidget *PrefsDialog::audioPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    auto *snd = check(QStringLiteral("Audio output"), QStringLiteral("Open the system audio device"),
                      m_s, "enable_audio", 1);
    connect(snd, &QCheckBox::toggled, this, [this](bool on) { o2session_set_int(m_s, "enable_audio", on ? 1 : 0); });
    form->addRow(snd);
    auto *volume = new QSlider(Qt::Horizontal);
    volume->setRange(0, 100);
    volume->setValue(o2session_get_int(m_s, "volume", 100));
    auto *percent = new QLabel(QStringLiteral("%1%").arg(volume->value()));
    percent->setMinimumWidth(percent->fontMetrics().horizontalAdvance(QStringLiteral("100%")));
    connect(volume, &QSlider::valueChanged, this, [this, percent](int v) {
        o2session_set_volume(m_s, v);
        percent->setText(QStringLiteral("%1%").arg(v));
    });
    auto *row = new QHBoxLayout;
    row->addWidget(volume, 1);
    row->addWidget(percent);
    form->addRow(QStringLiteral("Volume"), row);
    return page;
}

QWidget *PrefsDialog::inputPage()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    auto *top = new QFormLayout;
    auto *gp = check(QStringLiteral("Gamepads"), QStringLiteral("Poll USB and Bluetooth gamepads"),
                     m_s, "enable_gamepad", 1);
    connect(gp, &QCheckBox::toggled, this, [this](bool on) { o2session_set_int(m_s, "enable_gamepad", on ? 1 : 0); });
    top->addRow(gp);
    auto *aj = check(QStringLiteral("Analog sticks steer too"),
                     QStringLiteral("A gamepad's analog stick moves the joystick, as its D-pad does"),
                     m_s, "analog_joystick", 1);
    connect(aj, &QCheckBox::toggled, this, [this](bool on) { o2session_set_analog(m_s, on ? 1 : 0); });
    top->addRow(aj);
    v->addLayout(top);

    /* one row per pad, refreshed as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (given a joystick in connection order unless chosen here)"));
    m_padForm = new QFormLayout(pads);
    v->addWidget(pads);
    v->addStretch(1);
    m_padGen = o2session_gamepad_generation(m_s);
    rebuildPads();
    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] {
        const unsigned gen = o2session_gamepad_generation(m_s);
        if (gen == m_padGen) return;
        m_padGen = gen;
        rebuildPads();
    });
    timer->start(1000);
    return page;
}

void PrefsDialog::rebuildPads()
{
    for (QWidget *w : m_padRows) m_padForm->removeRow(w);
    m_padRows.clear();
    const int n = o2session_gamepad_count(m_s);
    if (n == 0) {
        auto *l = note(QStringLiteral("No gamepads connected: plug one in and it is picked up as it appears."));
        m_padForm->addRow(l);
        m_padRows.push_back(l);
        return;
    }
    for (int i = 0; i < n && i < 8; ++i) {
        char name[128];
        o2session_gamepad_name(m_s, i, name, sizeof name);
        auto *box = new QComboBox;
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Stick 1"), QStringLiteral("Stick 2") });
        box->setCurrentIndex(o2session_gamepad_assignment(m_s, i) + 1);
        const int eff = o2session_gamepad_effective_port(m_s, i);
        box->setToolTip(eff >= 0 ? QStringLiteral("Driving stick %1").arg(eff + 1)
                                 : QStringLiteral("Driving no stick"));
        connect(box, &QComboBox::currentIndexChanged, box, [this, i](int idx) {
            o2session_gamepad_assign(m_s, i, idx - 1);
        });
        m_padForm->addRow(local(name), box);
        m_padRows.push_back(box);
    }
}

/* ---- FujiNet ---------------------------------------------------------------- */

QWidget *PrefsDialog::fujinetPage()
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    auto *fuji = check(QStringLiteral("Enable FujiNet"),
                       QStringLiteral("Run the in-process FujiNet the cartridge dials into. Off means no "
                                      "network and a link-down CONFIG client."), m_s, "enable_fujinet", 1);
    connect(fuji, &QCheckBox::toggled, this, [this](bool on) { o2session_set_int(m_s, "enable_fujinet", on ? 1 : 0); });
    form->addRow(fuji);

    m_external = check(QStringLiteral("Use external FujiNet"),
                       QStringLiteral("Skip the built-in FujiNet and dial another instance at this address. "
                                      "Start that instance listening on the same port."),
                       m_s, "fujinet_external", 0);
    connect(m_external, &QCheckBox::toggled, this, [this](bool on) {
        o2session_set_int(m_s, "fujinet_external", on ? 1 : 0);
        m_addr->setEnabled(on);
    });
    form->addRow(m_external);

    m_addr = new QLineEdit(local(o2session_get_str(m_s, "fujinet_external_addr", "")));
    m_addr->setPlaceholderText(QStringLiteral("127.0.0.1:%1").arg(O2SESSION_BOIP_PORT));
    m_addr->setEnabled(m_external->isChecked());
    /* valid text (or none, meaning the default) is saved; anything else is
     * shown in red and not saved */
    connect(m_addr, &QLineEdit::textChanged, this, [this](const QString &text) {
        char host[64];
        int port;
        const QByteArray addr = text.trimmed().toUtf8();
        if (o2session_parse_hostport(addr.constData(), host, sizeof host, &port) != 0) {
            m_addr->setStyleSheet(QStringLiteral("QLineEdit { color: #d83b3b; }"));
            return;
        }
        m_addr->setStyleSheet(QString());
        o2session_set_str(m_s, "fujinet_external_addr", addr.constData());
    });
    form->addRow(QStringLiteral("Address:port"), m_addr);
    form->addRow(note(QStringLiteral("The cartridge dials this BoIP address, as it does the built-in "
                                     "FujiNet's.")));

    const bool external = o2session_fujinet_external(m_s) != 0;
    const QString url = local(o2session_fujinet_webui_url(m_s));
    auto *web = new QLabel(external ? QStringLiteral("FujiNet is external") : url.toHtmlEscaped());
    web->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *open = new QPushButton(QStringLiteral("Open"));
    open->setToolTip(QStringLiteral("Open FujiNet's web UI in the browser"));
    open->setEnabled(!external && o2session_fujinet_running(m_s));
    connect(open, &QPushButton::clicked, this, [url] { QDesktopServices::openUrl(QUrl(url)); });
    auto *row = new QHBoxLayout;
    row->addWidget(web, 1);
    row->addWidget(open);
    form->addRow(QStringLiteral("Web UI"), row);
    return page;
}

} // namespace

/* ---- the public entry points ------------------------------------------------ */

int SettingsDialog::importRoms(QWidget *parent, o2session *session, bool folder, const QString &path)
{
    QStringList inputs;
    if (!path.isEmpty()) {
        inputs << path;
    } else if (folder) {
        const QString dir = QFileDialog::getExistingDirectory(parent, QStringLiteral("Import BIOS from a Folder"));
        if (dir.isEmpty()) return -1;
        inputs << dir;
    } else {
        inputs = QFileDialog::getOpenFileNames(parent, QStringLiteral("Import BIOS"), QString(),
                                               QString::fromUtf8(kRomFilter));
        if (inputs.isEmpty()) return -1;
    }

    int total = 0;
    bool trouble = false;
    QStringList said;
    for (const QString &in : inputs) {
        char msg[1024] = "";
        const int rc = o2session_rom_import(session, in.toLocal8Bit().constData(), msg, sizeof msg);
        if (rc > 0) total += rc;
        if (rc <= 0) trouble = true;
        QString text = local(msg[0] ? msg : o2session_last_error(session));
        if (inputs.size() > 1) text = QStringLiteral("%1: %2").arg(QFileInfo(in).fileName(), text);
        said << text;
    }
    /* the session's message always says what happened: show it whole */
    const QString text = said.join(QStringLiteral("\n\n"));
    if (total == 0)
        QMessageBox::warning(parent, QStringLiteral("Nothing Imported"), text);
    else if (trouble)
        QMessageBox::warning(parent, QStringLiteral("BIOS Imported"), text);
    else
        QMessageBox::information(parent, QStringLiteral("BIOS Imported"), text);
    return total;
}

bool SettingsDialog::run(QWidget *parent, o2session *session)
{
    PrefsDialog dlg(parent, session);
    dlg.exec();
    return dlg.restartNeeded();
}
