/*
 * KeypadWindow -- see KeypadWindow.h.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KeypadWindow.h"

#include <QComboBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"

/* ---- PadButton ------------------------------------------------------------ */

PadButton::PadButton(const QString &face, int target, QWidget *parent)
    : QPushButton(face, parent), m_target(target), m_face(face)
{
    setFocusPolicy(Qt::NoFocus);
}

void PadButton::setHeld(bool held)
{
    if (held == m_held) return;
    m_held = held;
    /* the tan is pale: dark text on it, whatever the theme */
    setStyleSheet(held ? QStringLiteral("background: %1; color: black;").arg(o2AccentColor().name())
                       : QString());
}

void PadButton::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && !m_down) {
        m_down = true;
        emit pressedTarget(m_target);
    }
    QPushButton::mousePressEvent(e);
}

void PadButton::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::mouseReleaseEvent(e);
}

/* Dragging off a button must release it. */
void PadButton::leaveEvent(QEvent *e)
{
    if (m_down) {
        m_down = false;
        emit releasedTarget(m_target);
    }
    QPushButton::leaveEvent(e);
}

/* ---- KeypadWindow ----------------------------------------------------------- */

/* Fixed button sizes, wide enough for a Map-mode label ("Keypad 0 / A"),
 * so the panel is the same shape in both modes. */
static constexpr int kButtonWidth = 112;
static constexpr int kButtonHeight = 40;

KeypadWindow::KeypadWindow(o2session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a
     * panel of fixed-size buttons. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint
                      | Qt::CustomizeWindowHint), m_session(session)
{
    setWindowTitle(QStringLiteral("Keypad"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its controls and kept there: a resizable panel gets tiled
     * and its buttons stretched, and nothing here wants more room. */
    root->setSizeConstraint(QLayout::SetFixedSize);
    auto *keypads = new QHBoxLayout;
    keypads->addWidget(buildKeypad(0));
    keypads->addWidget(buildKeypad(1));
    root->addLayout(keypads);

    /* The console's own button, CLEAR (it resets whatever the cartridge
     * serves), is a machine control like any other; Reset to CONFIG is the
     * power switch of a FujiNet cartridge. */
    auto *console = new QGroupBox(QStringLiteral("Console"));
    auto *crow = new QHBoxLayout(console);
    crow->addStretch();
    PadButton *clear = control(QStringLiteral("CLEAR"), O2_TARGET_SWITCH(O2_SW_CLEAR));
    PadButton *config = control(QStringLiteral("Reset to CONFIG"), O2_TARGET_SYSACT(O2_SYSACT_RESET_CONFIG));
    const int cw = qMax(kButtonWidth, config->fontMetrics().horizontalAdvance(config->face()) + 24);
    clear->setFixedWidth(cw);
    config->setFixedWidth(cw);
    crow->addWidget(clear);
    crow->addWidget(config);
    crow->addStretch();
    root->addWidget(console);

    /* Gamepads: one row per pad, rebuilt as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads (assigned to keypads in connection order unless chosen here)"));
    m_padForm = new QFormLayout(pads);
    root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        o2session_bindings_reset(m_session);
        refreshLabels();
    });
    m_hint = new QLabel;
    m_hint->setStyleSheet(QStringLiteral("color: gray;"));
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_hint, 1);
    root->addLayout(maprow);

    connect(&m_captureTimer, &QTimer::timeout, this, &KeypadWindow::pollCapture);
    connect(&m_pollTimer, &QTimer::timeout, this, &KeypadWindow::poll);
    rebuildPads();
    setMapState(-2);
}

PadButton *KeypadWindow::control(const QString &face, int target)
{
    auto *b = new PadButton(face, target);
    b->setFixedSize(kButtonWidth, kButtonHeight);
    connect(b, &PadButton::pressedTarget, this, &KeypadWindow::onPressed);
    connect(b, &PadButton::releasedTarget, this, &KeypadWindow::onReleased);
    m_controls.push_back(b);
    return b;
}

QWidget *KeypadWindow::buildKeypad(int pad)
{
    auto *box = new QGroupBox(pad ? QStringLiteral("Keypad B") : QStringLiteral("Keypad A"));
    auto *v = new QVBoxLayout(box);

    /* The console's own layout: 1-9 in reading order, 0 alone under 8. */
    auto key = [this, pad](int act) {
        const int t = O2_TARGET_PORT(pad, act);
        return control(QString::fromUtf8(o2_target_short_name(t)), t);
    };
    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    grid->setSizeConstraint(QLayout::SetFixedSize);
    for (int k = 1; k <= 9; ++k)
        grid->addWidget(key(O2_ACT_0 + k), (k - 1) / 3, (k - 1) % 3);
    grid->addWidget(key(O2_ACT_0), 3, 1);
    auto *gridRow = new QHBoxLayout;
    gridRow->addStretch();
    gridRow->addLayout(grid);
    gridRow->addStretch();
    v->addLayout(gridRow);
    return box;
}

void KeypadWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) m_padForm->removeRow(w);
    m_padRows.clear();
    const int n = o2session_gamepad_count(m_session);
    if (n == 0) {
        auto *l = new QLabel(QStringLiteral("No gamepads connected — plug one in, it is picked up as it appears"));
        l->setStyleSheet(QStringLiteral("color: gray;"));
        m_padForm->addRow(l);
        m_padRows.push_back(l);
        return;
    }
    for (int i = 0; i < n && i < 8; ++i) {
        char name[128];
        o2session_gamepad_name(m_session, i, name, sizeof name);
        auto *box = new QComboBox;
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Keypad A"), QStringLiteral("Keypad B") });
        box->setCurrentIndex(o2session_gamepad_assignment(m_session, i) + 1);
        const int eff = o2session_gamepad_effective_port(m_session, i);
        box->setToolTip(eff >= 0 ? QStringLiteral("Driving keypad %1").arg(QChar(eff ? 'B' : 'A'))
                                 : QStringLiteral("Driving no keypad"));
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            o2session_gamepad_assign(m_session, i, idx - 1);
        });
        m_padForm->addRow(QString::fromUtf8(name), box);
        m_padRows.push_back(box);
    }
}

void KeypadWindow::onPressed(int target)
{
    if (m_mapState == -1) { setMapState(target); return; }
    if (m_mapState >= 0) return;
    m_mouseHeld = target;
    if (target >= O2_TARGET_SYSACT(0)) return;   /* fires on release */
    o2session_press(m_session, target, 1);
}

/* A system action is posted rather than run here, so the main window takes
 * it as it does a gamepad's: with its status message, and with the
 * debugger window for Debugger Stop. */
void KeypadWindow::onReleased(int target)
{
    if (m_mapState != -2) return;
    if (m_mouseHeld == target) m_mouseHeld = -1;
    if (target >= O2_TARGET_SYSACT(0)) {
        o2session_sysaction_post(m_session, target - O2_TARGET_SYSACT(0));
        return;
    }
    o2session_press(m_session, target, 0);
}

void KeypadWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_captureTimer.stop();
        o2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QString());
    } else if (state == -1) {
        m_captureTimer.stop();
        o2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Cancel"));
        m_mapButton->setStyleSheet(QStringLiteral("background: %1; color: black;").arg(o2AccentColor().name()));
        m_hint->setText(QStringLiteral("Click a control to remap"));
    } else {
        m_hint->setText(QStringLiteral("Press a key or gamepad button for %1")
                            .arg(QString::fromUtf8(o2_target_name(state))));
        o2session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    refreshLabels();
}

void KeypadWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (o2session_gamepad_capture_poll(m_session, &button)) {
        char stolen[128];
        o2session_binding_set_button(m_session, m_mapState, button, stolen, sizeof stolen);
        const QString msg = stolen[0]
            ? QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(o2_pad_button_name(button)), QString::fromUtf8(stolen))
            : QString();
        setMapState(-1);
        if (!msg.isEmpty()) m_hint->setText(msg);
    }
}

void KeypadWindow::refreshLabels()
{
    for (PadButton *b : m_controls) {
        if (m_mapState != -2) {
            const o2_binding bind = o2session_binding_get(m_session, b->target());
            char key[32];
            o2session_keysym_name(bind.keysym, key, sizeof key);
            QString text = key[0] ? QString::fromUtf8(key) : QStringLiteral("—");
            if (bind.button != O2_PAD_BTN_NONE)
                text += QStringLiteral(" / ") + QString::fromUtf8(o2_pad_button_name(bind.button));
            b->setText(text);
            b->setToolTip(QString::fromUtf8(o2_target_name(b->target())));
            b->setHeld(b->target() == m_mapState);
        } else {
            b->setText(b->face());
            b->setToolTip(QString());
            b->setHeld(false);
        }
    }
}

/* What the machine sees held, from every source, lit in the accent colour;
 * the gamepad list follows hot-plugging. */
void KeypadWindow::poll()
{
    const unsigned gen = o2session_gamepad_generation(m_session);
    if (gen != m_padGen) { m_padGen = gen; rebuildPads(); }
    if (m_mapState != -2) return;
    const unsigned held[2] = { o2session_buttons_held(m_session, 0), o2session_buttons_held(m_session, 1) };
    for (PadButton *b : m_controls) {
        const int t = b->target();
        bool on = t == m_mouseHeld;
        if (t < 2 * O2_ACT_PER_PORT)
            on = on || (held[t / O2_ACT_PER_PORT] & (1u << (t % O2_ACT_PER_PORT)));
        else if (t < O2_TARGET_SYSACT(0))
            on = on || o2session_switch_held(m_session, t - O2_TARGET_SWITCH(0));
        b->setHeld(on);
    }
}

void KeypadWindow::showEvent(QShowEvent *e)
{
    poll();
    m_pollTimer.start(50);
    QWidget::showEvent(e);
}

void KeypadWindow::hideEvent(QHideEvent *e)
{
    m_pollTimer.stop();
    setMapState(-2);
    QWidget::hideEvent(e);
}

void KeypadWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = o2KeysymFromQt(e);
    if (m_mapState >= 0) {
        if (!ks) return;
        char stolen[128], name[32];
        o2session_binding_set_key(m_session, m_mapState, ks, stolen, sizeof stolen);
        o2session_keysym_name(ks, name, sizeof name);
        setMapState(-1);
        if (stolen[0])
            m_hint->setText(QStringLiteral("Bound %1 (was %2)").arg(QString::fromUtf8(name), QString::fromUtf8(stolen)));
        return;
    }
    if (m_mapState == -1) return;
    if (e->key() == Qt::Key_F9) { hide(); return; }
    if (!ks) { QWidget::keyPressEvent(e); return; }
    const int sa = o2session_key_sysaction(m_session, ks);
    if (sa >= 0) { o2session_sysaction_post(m_session, sa); return; }
    if (!o2session_key(m_session, ks, 1)) QWidget::keyPressEvent(e);
}

void KeypadWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat() || m_mapState != -2) return;
    const uint32_t ks = o2KeysymFromQt(e);
    if (!ks || !o2session_key(m_session, ks, 0)) QWidget::keyReleaseEvent(e);
}
