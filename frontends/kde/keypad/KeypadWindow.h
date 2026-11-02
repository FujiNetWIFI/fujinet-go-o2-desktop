/*
 * The Keypad window: both Odyssey 2 keypads on screen (they press the
 * machine's keys and light up with whatever the keyboard, a gamepad or the
 * mouse is holding), the console's CLEAR button and Reset to CONFIG, each
 * gamepad's keypad assignment, and Map mode for rebinding any control to a
 * key or a gamepad button.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QWidget>
#include <vector>

#include "o2session.h"

/* A button that reports press and release, not "clicked": a keypad key is
 * HELD, since the console scans its keypads once per frame. */
class PadButton : public QPushButton {
    Q_OBJECT
public:
    PadButton(const QString &face, int target, QWidget *parent = nullptr);
    int target() const { return m_target; }
    QString face() const { return m_face; }
    void setHeld(bool held);
signals:
    void pressedTarget(int target);
    void releasedTarget(int target);
protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void leaveEvent(QEvent *e) override;
private:
    int m_target;
    QString m_face;
    bool m_down = false;
    bool m_held = false;
};

class KeypadWindow : public QWidget {
    Q_OBJECT
public:
    explicit KeypadWindow(o2session *session, QWidget *parent = nullptr);

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;

private:
    QWidget *buildKeypad(int pad);
    PadButton *control(const QString &face, int target);
    void onPressed(int target);
    void onReleased(int target);
    void setMapState(int state);
    void refreshLabels();
    void pollCapture();
    void poll();
    void rebuildPads();

    o2session *m_session;
    std::vector<PadButton *> m_controls;
    QPushButton *m_mapButton = nullptr;
    QLabel *m_hint = nullptr;
    QFormLayout *m_padForm = nullptr;
    std::vector<QWidget *> m_padRows;
    unsigned m_padGen = ~0u;
    int m_mouseHeld = -1;      /* the target the mouse is holding, if any */
    QTimer m_captureTimer;
    QTimer m_pollTimer;
    /* -2 idle, -1 armed and waiting for a target, >=0 waiting for a key or
     * gamepad button. */
    int m_mapState = -2;
};
