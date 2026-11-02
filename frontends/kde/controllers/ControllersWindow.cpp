/*
 * ControllersWindow -- see ControllersWindow.h.
 *
 * The membrane keyboard is painted from the session's layout table
 * (o2session_keyboard_layout: every key cap at its x/y/w/h in key units), so
 * every frontend shows the same keys for the machine, and the Videopac+
 * G7400's extra keys appear when that machine is selected. Under it, the two
 * joysticks as the console's are: a black base, the red fire button in the
 * corner, a stick with four direction pads.
 *
 * Keys and stick controls act on mouse press AND release, not "clicked": the
 * console scans its keyboard and samples its sticks once a frame, so a press
 * present only for the instant of a click would fall between frames. A key
 * is HELD while the mouse is down on it; dragging off it releases it, and so
 * do hiding the window and losing the focus (only what the mouse holds here:
 * the main window releases its own keys).
 *
 * What the console sees held -- host keyboard, gamepads and this window -- is
 * lit in the accent (the Odyssey 2's red), so the window doubles as an input
 * tester for a new pad. Keys typed with it active go to the console as in the
 * main window.
 *
 * FIXED SIZE: the drawing is a fixed number of pixels per key unit, Map
 * legends are elided into the caps, and gamepad names are elided, so neither
 * Map mode nor a long name resizes the window; only a machine change does.
 * A singleton, hidden rather than destroyed, so a half-done remap survives
 * closing it. A timer runs only while it is visible.
 *
 * MAP MODE: Map arms a two-step rebind -- click a key cap or a stick control,
 * then press a host key or a gamepad button. Rebinding steals the key from
 * whatever held it (named in the hint). Esc cancels; while Map is armed
 * nothing is typed into the console.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ControllersWindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPolygonF>
#include <QPushButton>
#include <QRadialGradient>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QtMath>

#include <functional>
#include <utility>
#include <vector>

#include "../FujiNetWindows.h"
#include "../KeyForward.h"
#include "../debugger/DebuggerWindow.h"

namespace {

constexpr int kUnit = 42;        /* pixels per key unit */
constexpr int kLeft = 26;        /* the keys' origin inside the panel */
constexpr int kTop = 30;
constexpr int kPadRight = 26;
constexpr int kPadBottom = 22;
constexpr int kGroupGapY = 16;   /* the printed groups are set apart */
constexpr int kGroupGapX = 12;
constexpr int kStickW = 200;
constexpr int kStickH = 150;
constexpr int kMaxPads = 8;

const QString kGray = QStringLiteral("color: gray;");

QString local(const char *s) { return QString::fromUtf8(s ? s : ""); }

QFont scaledFont(QFont f, qreal scale, bool bold = false)
{
    f.setPointSizeF((f.pointSizeF() > 0 ? f.pointSizeF() : 10) * scale);
    f.setBold(bold);
    return f;
}

/* Lines centred in r, the font shrunk until they fit. */
void drawFitted(QPainter &p, const QRectF &r, const QString &text, QFont f)
{
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (;;) {
        const QFontMetricsF fm(f);
        qreal w = 0;
        for (const QString &l : lines) w = qMax(w, fm.horizontalAdvance(l));
        if ((w <= r.width() && fm.lineSpacing() * lines.size() <= r.height() + 2) || f.pointSizeF() <= 5.0)
            break;
        f.setPointSizeF(f.pointSizeF() - 0.5);
    }
    p.setFont(f);
    p.drawText(r, Qt::AlignCenter, text);
}

/* A cap's binding target: its matrix position, or the console's RESET. */
int capTarget(const o2_keycap &c)
{
    return c.target >= 0 ? O2_TARGET_KEY(c.target) : O2_TARGET_SWITCH(O2_SW_RESET);
}

/* Whether the console sees `target` held now, from any source, or the mouse
 * is holding it here. */
bool heldNow(o2session *s, int target, int mouseTarget)
{
    if (target == mouseTarget) return true;
    if (target < 2 * O2_ACT_PER_PORT)
        return ((o2session_buttons_held(s, target / O2_ACT_PER_PORT) >> (target % O2_ACT_PER_PORT)) & 1u) != 0;
    if (target < O2_TARGET_SWITCH(0)) return o2session_key_held(s, target - O2_TARGET_KEY(0)) != 0;
    return o2session_switch_held(s, target - O2_TARGET_SWITCH(0)) != 0;
}

QString keyName(o2session *s, int target)
{
    const o2_binding b = o2session_binding_get(s, target);
    char key[48] = "";
    if (b.keysym) o2session_keysym_name(b.keysym, key, sizeof key);
    return local(key);
}

/* Map mode on a control: the key over the gamepad button. */
QString bindingText(o2session *s, int target)
{
    const o2_binding b = o2session_binding_get(s, target);
    const QString key = keyName(s, target);
    QString text = key.isEmpty() ? QStringLiteral("—") : key;
    if (b.button != O2_PAD_BTN_NONE) text += QLatin1Char('\n') + local(o2_pad_button_name(b.button));
    return text;
}

QString bindingTip(o2session *s, int target)
{
    const o2_binding b = o2session_binding_get(s, target);
    const QString key = keyName(s, target);
    QString tip = QStringLiteral("%1: %2").arg(local(o2_target_name(target)),
                                               key.isEmpty() ? QStringLiteral("no key") : key);
    if (b.button != O2_PAD_BTN_NONE) tip += QStringLiteral(", ") + local(o2_pad_button_name(b.button));
    return tip;
}

/* ---- ElidedLabel ------------------------------------------------------------ */

/* A label that never widens the window: one line elides, more wrap into a
 * fixed height. */
class ElidedLabel : public QWidget {
public:
    explicit ElidedLabel(bool dim, int lines = 1) : m_dim(dim), m_lines(lines)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    }
    void setText(const QString &text)
    {
        if (text == m_text) return;
        m_text = text;
        update();
    }
    QSize sizeHint() const override { return QSize(0, fontMetrics().lineSpacing() * m_lines); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setPen(m_dim ? QColor(Qt::gray) : palette().color(QPalette::WindowText));
        if (m_lines > 1)
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, m_text);
        else
            p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(m_text, Qt::ElideRight, width()));
    }

private:
    QString m_text;
    bool m_dim;
    int m_lines;
};

/* ---- ConsoleWidget: the membrane keyboard ---------------------------------------- */

class ConsoleWidget : public QWidget {
public:
    ConsoleWidget(o2session *session, std::function<void(int, bool)> onPress)
        : m_session(session), m_onPress(std::move(onPress))
    {
        setFocusPolicy(Qt::NoFocus);
    }

    /* The machine changed: its keys (the Videopac+ G7400 has more). */
    void load(int machine)
    {
        int n = 0;
        float w = 0, h = 0;
        const o2_keycap *caps = o2session_keyboard_layout(machine, &n, &w, &h);
        m_groups = machine != O2_MACHINE_VIDEOPAC_PLUS;   /* its printed groups are known only for the O2 */
        m_caps.clear();
        qreal right = 0, bottom = 0;
        for (int i = 0; i < n; ++i) {
            Cap c;
            c.cap = caps[i];
            c.target = capTarget(caps[i]);
            c.rect = capRect(caps[i]);
            right = qMax(right, c.rect.right());
            bottom = qMax(bottom, c.rect.bottom());
            m_caps.push_back(c);
        }
        m_held.assign(m_caps.size(), 0);
        m_pressed = -1;
        setFixedSize(qCeil(right) + kPadRight, qCeil(bottom) + kPadBottom);
        update();
    }
    /* -2 idle, -1 armed, >= 0 waiting for a key or pad button for that target */
    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void setKeyNames(bool on)
    {
        m_keyNames = on;
        update();
    }
    /* What the console sees held (and the cap the mouse holds); repaints only
     * on a change. */
    void pollHeld(int mouseTarget)
    {
        bool changed = false;
        for (size_t i = 0; i < m_caps.size(); ++i) {
            const char on = heldNow(m_session, m_caps[i].target, mouseTarget);
            if (on != m_held[i]) {
                m_held[i] = on;
                changed = true;
            }
        }
        if (changed) update();
    }
    /* The target of the cap at pos, or -1. */
    int targetAt(const QPointF &pos) const
    {
        const int i = capAt(pos);
        return i < 0 ? -1 : m_caps[(size_t)i].target;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        /* the console's silver panel, and the black keyboard plate in it */
        const QRectF panel(0.5, 0.5, width() - 1.0, height() - 1.0);
        QLinearGradient silver(0, 0, 0, height());
        silver.setColorAt(0, QColor(0xDB, 0xD9, 0xD7));
        silver.setColorAt(1, QColor(0xB1, 0xAE, 0xAC));
        p.setPen(QColor(0x8A, 0x87, 0x85));
        p.setBrush(silver);
        p.drawRoundedRect(panel, 12, 12);
        p.setPen(QColor(0x05, 0x05, 0x05));
        p.setBrush(QColor(0x16, 0x16, 0x16));
        p.drawRoundedRect(panel.adjusted(10, 10, -10, -10), 9, 9);

        if (m_groups) paintGroups(p);
        for (size_t i = 0; i < m_caps.size(); ++i) paintCap(p, i);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int i = capAt(e->position());
        if (i < 0) return;
        m_pressed = i;
        m_onPress(m_caps[(size_t)i].target, true);
    }

    /* Dragging off a key must release it. */
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_pressed >= 0 && !m_caps[(size_t)m_pressed].rect.contains(e->position())) release();
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) release();
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int i = capAt(he->pos());
            if (i < 0) {
                QToolTip::hideText();
            } else {
                const Cap &c = m_caps[(size_t)i];
                QString tip = bindingTip(m_session, c.target);
                if (c.cap.target == 44 || c.cap.target == 45)
                    tip += QStringLiteral(" (YES and Y are one key on the console, and so are NO and N)");
                QToolTip::showText(he->globalPos(), tip, this, c.rect.toRect());
            }
            return true;
        }
        return QWidget::event(e);
    }

private:
    struct Cap {
        o2_keycap cap;
        int target;
        QRectF rect;
    };

    /* A key cap's rectangle: the layout's, and, for the Odyssey 2's printed
     * groups (numeric, function, input, alpha), a little room between them
     * for their outlines and names. */
    QRectF capRect(const o2_keycap &c) const
    {
        qreal x = c.x * kUnit, y = c.y * kUnit;
        if (m_groups) {
            if (c.y > 0.5f) y += kGroupGapY;
            if (c.y > 1.5f) y += kGroupGapY;
            if (c.y > 0.5f && c.y < 1.5f) {
                if (c.x > 4.95f) x += kGroupGapX;
                if (c.x > 9.3f) x += kGroupGapX;
            }
        }
        return QRectF(kLeft + x, kTop + y, c.w * kUnit, c.h * kUnit);
    }

    int capAt(const QPointF &pos) const
    {
        for (size_t i = 0; i < m_caps.size(); ++i)
            if (m_caps[i].rect.contains(pos)) return (int)i;
        return -1;
    }

    void release()
    {
        if (m_pressed < 0) return;
        const int i = m_pressed;
        m_pressed = -1;
        m_onPress(m_caps[(size_t)i].target, false);
    }

    /* The printed outlines round the groups of keys, each with its name in
     * its top edge (the function and input legends in red, as printed). */
    void paintGroups(QPainter &p)
    {
        static const char *const names[5] = { "NUMERIC", "FUNCTION", "INPUT", "RESET", "ALPHA" };
        QRectF box[5];
        for (const Cap &c : m_caps) {
            int g = 4;
            if (c.cap.kind == O2_KEYCAP_FUNCTION) g = 1;
            else if (c.cap.kind == O2_KEYCAP_INPUT) g = 2;
            else if (c.cap.kind == O2_KEYCAP_RESET) g = 3;
            else if (c.cap.y < 0.5f) g = 0;
            box[g] = box[g].isNull() ? c.rect : box[g].united(c.rect);
        }
        const QFont f = scaledFont(font(), 0.62, true);
        const QFontMetricsF fm(f);
        for (int g = 0; g < 5; ++g) {
            if (box[g].isNull()) continue;
            const QColor line = (g == 0 || g == 4) ? QColor(0xA8, 0xA8, 0xA8) : QColor(0xC8, 0x2A, 0x32);
            const QRectF r = box[g].adjusted(-5, -5, 5, 5);
            const QString name = QString::fromLatin1(names[g]);
            const qreal tw = fm.horizontalAdvance(name) + 8;
            p.setPen(QPen(line, 1.2));
            p.setBrush(Qt::NoBrush);
            /* the outline, broken at the top where its name sits */
            const qreal gapL = r.center().x() - tw / 2, gapR = r.center().x() + tw / 2;
            QPainterPath path;
            path.moveTo(gapL, r.top());
            path.lineTo(r.left() + 6, r.top());
            path.quadTo(r.left(), r.top(), r.left(), r.top() + 6);
            path.lineTo(r.left(), r.bottom() - 6);
            path.quadTo(r.left(), r.bottom(), r.left() + 6, r.bottom());
            path.lineTo(r.right() - 6, r.bottom());
            path.quadTo(r.right(), r.bottom(), r.right(), r.bottom() - 6);
            path.lineTo(r.right(), r.top() + 6);
            path.quadTo(r.right(), r.top(), r.right() - 6, r.top());
            path.lineTo(gapR, r.top());
            p.drawPath(path);
            p.setFont(f);
            p.setPen(line);
            p.drawText(QRectF(gapL, r.top() - fm.height() / 2, tw, fm.height()), Qt::AlignCenter, name);
        }
    }

    void paintCap(QPainter &p, size_t i)
    {
        const Cap &c = m_caps[i];
        const bool mapping = m_map != -2;
        const bool lit = mapping ? c.target == m_map : m_held[i] != 0;
        const bool reset = c.cap.kind == O2_KEYCAP_RESET;
        const QRectF r = c.rect.adjusted(0.5, 0.5, -0.5, -2.5);

        QColor top, bottom, legend;
        if (lit) {
            top = bottom = o2AccentColor();
            legend = Qt::white;
        } else if (reset) {
            top = QColor(0xB0, 0x30, 0x34);
            bottom = o2AccentDarkColor();
            legend = Qt::white;
        } else {
            top = QColor(0xFC, 0xFB, 0xF7);
            bottom = QColor(0xDD, 0xDA, 0xD0);
            legend = c.cap.kind == O2_KEYCAP_FUNCTION ? QColor(0xB3, 0x14, 0x1B) : QColor(0x1B, 0x1B, 0x1B);
        }
        /* the cap's side, then its face */
        p.setPen(Qt::NoPen);
        p.setBrush(lit ? o2AccentDarkColor() : reset ? QColor(0x5C, 0x12, 0x14) : QColor(0x8E, 0x8B, 0x80));
        p.drawRoundedRect(r.translated(0, 2.5), 6, 6);
        QLinearGradient face(r.topLeft(), r.bottomLeft());
        face.setColorAt(0, top);
        face.setColorAt(1, bottom);
        p.setBrush(face);
        p.setPen(lit && reset ? QPen(Qt::white, 1.5) : QPen(QColor(0x2A, 0x2A, 0x2A), 0.8));
        p.drawRoundedRect(r, 6, 6);

        p.setPen(legend);
        const QRectF in = r.adjusted(2, 1, -2, -1);
        const QString label = QString::fromUtf8(c.cap.label);
        if (mapping) {
            drawFitted(p, in, bindingText(m_session, c.target), scaledFont(font(), 0.7, true));
            return;
        }
        if (reset) {
            /* the console's reset key carries a triangle, not a name */
            const QPointF c0 = in.center() + QPointF(0, m_keyNames ? -3 : 0);
            p.setPen(QPen(Qt::white, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPolygon(QPolygonF({ c0 + QPointF(0, -7), c0 + QPointF(-8, 6), c0 + QPointF(8, 6) }));
        } else {
            drawFitted(p, in.adjusted(0, m_keyNames ? -3 : 0, 0, 0), label,
                       scaledFont(font(), label.size() > 1 ? 0.78 : 1.15, true));
        }
        if (m_keyNames) {
            const QString name = keyName(m_session, c.target);
            if (!name.isEmpty()) {
                const QFont nf = scaledFont(font(), 0.6);
                p.setFont(nf);
                p.setPen(lit ? QColor(255, 255, 255, 200) : QColor(0x6A, 0x68, 0x62));
                p.drawText(in.adjusted(0, 0, -1, 0), Qt::AlignRight | Qt::AlignBottom,
                           QFontMetrics(nf).elidedText(name, Qt::ElideRight, qFloor(in.width())));
            }
        }
    }

    o2session *m_session;
    std::function<void(int, bool)> m_onPress;
    std::vector<Cap> m_caps;
    std::vector<char> m_held;
    bool m_groups = true;
    bool m_keyNames = false;
    int m_map = -2;
    int m_pressed = -1;
};

/* ---- StickWidget: one joystick ---------------------------------------------------- */

class StickWidget : public QWidget {
public:
    StickWidget(o2session *session, int port, std::function<void(int, bool)> onPress)
        : m_session(session), m_port(port), m_onPress(std::move(onPress))
    {
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(false);
        setFixedSize(kStickW, kStickH);
    }

    void setMap(int state)
    {
        m_map = state;
        update();
    }
    void pollHeld(int mouseTarget)
    {
        bool changed = false;
        for (int act = 0; act < O2_ACT_PER_PORT; ++act) {
            const char on = heldNow(m_session, O2_TARGET_PORT(m_port, act), mouseTarget);
            if (on != m_held[act]) {
                m_held[act] = on;
                changed = true;
            }
        }
        if (changed) update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF all(0.5, 0.5, width() - 1.0, height() - 1.0);

        /* the controller's black base with its raised top plate */
        QLinearGradient base(0, 0, 0, height());
        base.setColorAt(0, QColor(0x2A, 0x2A, 0x2E));
        base.setColorAt(1, QColor(0x12, 0x12, 0x14));
        p.setPen(QColor(0, 0, 0));
        p.setBrush(base);
        p.drawRoundedRect(all, 12, 12);
        p.setPen(QColor(0x34, 0x34, 0x38));
        p.setBrush(QColor(0x1B, 0x1B, 0x1E));
        p.drawRoundedRect(all.adjusted(6, 6, -6, -6), 9, 9);

        const bool mapping = m_map != -2;
        const QFont small = scaledFont(font(), 0.65, true);

        /* the stick: a socket, and a knob that leans the way it is held */
        const QPointF c = centre();
        p.setPen(QPen(QColor(0x3C, 0x3C, 0x42), 2));
        p.setBrush(QColor(0x0A, 0x0A, 0x0B));
        p.drawEllipse(c, 22, 22);
        const qreal dx = (m_held[O2_ACT_RIGHT] ? 5 : 0) - (m_held[O2_ACT_LEFT] ? 5 : 0);
        const qreal dy = (m_held[O2_ACT_DOWN] ? 5 : 0) - (m_held[O2_ACT_UP] ? 5 : 0);
        QRadialGradient knob(c + QPointF(dx - 3, dy - 3), 18);
        knob.setColorAt(0, QColor(0x7C, 0x7C, 0x84));
        knob.setColorAt(1, QColor(0x1C, 0x1C, 0x20));
        p.setPen(QColor(0, 0, 0));
        p.setBrush(knob);
        p.drawEllipse(c + QPointF(dx, dy), 14, 14);

        /* the four direction pads, and the red fire button in the corner */
        for (int act = 0; act < O2_ACT_PER_PORT; ++act) {
            const int target = O2_TARGET_PORT(m_port, act);
            const QRectF r = padRect(act);
            const bool lit = mapping ? target == m_map : m_held[act] != 0;
            const bool fire = act == O2_ACT_FIRE;
            QColor fill;
            if (lit) fill = o2AccentColor();
            else if (fire) fill = o2AccentDarkColor();
            else fill = QColor(0x2E, 0x2E, 0x33);
            QLinearGradient g(r.topLeft(), r.bottomLeft());
            g.setColorAt(0, fill.lighter(fire ? 135 : 125));
            g.setColorAt(1, fill);
            p.setPen(lit && fire ? QPen(Qt::white, 1.5) : QPen(QColor(0, 0, 0), 1));
            p.setBrush(g);
            p.drawRoundedRect(r, 5, 5);
            p.setPen(Qt::white);
            if (mapping) {
                drawFitted(p, r.adjusted(2, 1, -2, -1), bindingText(m_session, target), small);
            } else if (fire) {
                p.setFont(small);
                p.drawText(r, Qt::AlignCenter, QStringLiteral("FIRE"));
            } else {
                p.setBrush(lit ? QColor(Qt::white) : QColor(0xCF, 0xCF, 0xD4));
                p.setPen(Qt::NoPen);
                p.drawPolygon(arrow(act, r.center()));
            }
        }
        p.setPen(QColor(0x8A, 0x8A, 0x90));
        p.setFont(small);
        p.drawText(QRectF(14, kStickH - 24, 90, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("STICK %1").arg(m_port + 1));
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton) return;
        const int act = actAt(e->position());
        if (act < 0) return;
        m_pressed = act;
        m_onPress(O2_TARGET_PORT(m_port, act), true);
    }

    /* Dragging off a pad releases it; onto another, holds that one. */
    void mouseMoveEvent(QMouseEvent *e) override
    {
        if (m_pressed < 0) return;
        const int act = actAt(e->position());
        if (act == m_pressed) return;
        const int old = m_pressed;
        m_pressed = act;
        m_onPress(O2_TARGET_PORT(m_port, old), false);
        if (act >= 0) m_onPress(O2_TARGET_PORT(m_port, act), true);
    }

    void mouseReleaseEvent(QMouseEvent *e) override
    {
        if (e->button() != Qt::LeftButton || m_pressed < 0) return;
        const int act = m_pressed;
        m_pressed = -1;
        m_onPress(O2_TARGET_PORT(m_port, act), false);
    }

    bool event(QEvent *e) override
    {
        if (e->type() == QEvent::ToolTip) {
            auto *he = static_cast<QHelpEvent *>(e);
            const int act = actAt(he->pos());
            if (act < 0)
                QToolTip::hideText();
            else
                QToolTip::showText(he->globalPos(), bindingTip(m_session, O2_TARGET_PORT(m_port, act)), this,
                                   padRect(act).toRect());
            return true;
        }
        return QWidget::event(e);
    }

private:
    static QPointF centre() { return QPointF(124, 78); }

    /* The pads around the stick (a plus), and the fire button. */
    static QRectF padRect(int act)
    {
        const QPointF c = centre();
        switch (act) {
        case O2_ACT_UP:    return QRectF(c.x() - 23, c.y() - 44, 46, 28);
        case O2_ACT_DOWN:  return QRectF(c.x() - 23, c.y() + 16, 46, 28);
        case O2_ACT_LEFT:  return QRectF(c.x() - 62, c.y() - 14, 46, 28);
        case O2_ACT_RIGHT: return QRectF(c.x() + 16, c.y() - 14, 46, 28);
        default:           return QRectF(14, 12, 58, 30);
        }
    }

    static QPolygonF arrow(int act, QPointF c)
    {
        switch (act) {
        case O2_ACT_UP:    return QPolygonF({ c + QPointF(0, -6), c + QPointF(-8, 5), c + QPointF(8, 5) });
        case O2_ACT_DOWN:  return QPolygonF({ c + QPointF(0, 6), c + QPointF(-8, -5), c + QPointF(8, -5) });
        case O2_ACT_LEFT:  return QPolygonF({ c + QPointF(-7, 0), c + QPointF(5, -7), c + QPointF(5, 7) });
        default:           return QPolygonF({ c + QPointF(7, 0), c + QPointF(-5, -7), c + QPointF(-5, 7) });
        }
    }

    int actAt(const QPointF &pos) const
    {
        for (int act = 0; act < O2_ACT_PER_PORT; ++act)
            if (padRect(act).contains(pos)) return act;
        return -1;
    }

    o2session *m_session;
    int m_port;
    std::function<void(int, bool)> m_onPress;
    char m_held[O2_ACT_PER_PORT] = {};
    int m_map = -2;
    int m_pressed = -1;
};

/* ---- ControllersWindow --------------------------------------------------------------- */

class ControllersWindow : public QWidget {
public:
    ControllersWindow(o2session *session, QWidget *parent);
    void present();
    void refresh();

protected:
    void keyPressEvent(QKeyEvent *e) override;
    void keyReleaseEvent(QKeyEvent *e) override;
    void showEvent(QShowEvent *e) override;
    void hideEvent(QHideEvent *e) override;
    void changeEvent(QEvent *e) override;
    /* Tab is a key of the console's, not focus traversal. */
    bool focusNextPrevChild(bool) override { return false; }

private:
    int machineNow() const;
    void rebuild();
    void onPress(int target, bool down);
    void letGo();
    void setMapState(int state);
    void bound(int target, const QString &what, const char *stolen);
    void pollCapture();
    void pollHeld();
    void pollSlow();
    void rebuildPads();
    void refreshPadPorts();

    o2session *m_session;
    ConsoleWidget *m_console = nullptr;
    StickWidget *m_stick[2] = {};
    QVBoxLayout *m_padBox = nullptr;
    std::vector<QWidget *> m_padRows;
    std::vector<QLabel *> m_padEff;       /* each pad's "drives stick N" */
    unsigned m_padGen = ~0u;
    QPushButton *m_mapButton = nullptr;
    QCheckBox *m_keyNames = nullptr;
    ElidedLabel *m_hint = nullptr;
    QTimer m_captureTimer;
    QTimer m_heldTimer;
    QTimer m_slowTimer;
    int m_builtMachine = -1;              /* what the keyboard was built for */
    int m_mouseTarget = -1;               /* the target the mouse holds here, or -1 */
    uint32_t m_swallow = 0;               /* the key Map took: its repeats and release too */
    /* -2 idle, -1 armed and waiting for a target, >= 0 waiting for a key or
     * gamepad button for that target. */
    int m_mapState = -2;
};

ControllersWindow::ControllersWindow(o2session *session, QWidget *parent)
    /* A dialog-type window, so a tiling compositor floats it: it is a panel
     * of fixed-size keys and buttons. */
    : QWidget(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::CustomizeWindowHint),
      m_session(session)
{
    setObjectName(QStringLiteral("controllers"));
    setWindowTitle(QStringLiteral("Controllers"));
    setFocusPolicy(Qt::StrongFocus);

    auto *root = new QVBoxLayout(this);
    /* Sized to its controls and kept there: a resizable panel gets tiled and
     * its keys stretched, and nothing here wants more room. */
    root->setSizeConstraint(QLayout::SetFixedSize);

    auto press = [this](int target, bool down) { onPress(target, down); };
    m_console = new ConsoleWidget(session, press);
    root->addWidget(m_console, 0, Qt::AlignHCenter);

    auto *sticks = new QHBoxLayout;
    sticks->setSpacing(14);
    sticks->addStretch(1);
    for (int port = 0; port < 2; ++port) {
        m_stick[port] = new StickWidget(session, port, press);
        sticks->addWidget(m_stick[port]);
    }
    sticks->addStretch(1);
    root->addLayout(sticks);

    /* Gamepads: one row per pad, rebuilt as they come and go */
    auto *pads = new QGroupBox(QStringLiteral("Gamepads"));
    m_padBox = new QVBoxLayout(pads);
    root->addWidget(pads);

    auto *maprow = new QHBoxLayout;
    m_mapButton = new QPushButton(QStringLiteral("Map"));
    m_mapButton->setFocusPolicy(Qt::NoFocus);
    m_mapButton->setFixedWidth(qMax(m_mapButton->sizeHint().width(),
                                    m_mapButton->fontMetrics().horizontalAdvance(QStringLiteral("Done")) + 32));
    connect(m_mapButton, &QPushButton::clicked, this, [this] { setMapState(m_mapState == -2 ? -1 : -2); });
    auto *defaults = new QPushButton(QStringLiteral("Defaults"));
    defaults->setFocusPolicy(Qt::NoFocus);
    connect(defaults, &QPushButton::clicked, this, [this] {
        o2session_bindings_reset(m_session);
        setMapState(m_mapState == -2 ? -2 : -1);
        m_hint->setText(QStringLiteral("Every key and stick control is back to its default."));
    });
    m_keyNames = new QCheckBox(QStringLiteral("Key names"));
    m_keyNames->setFocusPolicy(Qt::NoFocus);
    m_keyNames->setToolTip(QStringLiteral("Show the host key bound to each key cap"));
    m_keyNames->setChecked(o2session_get_int(session, "controllers_key_names", 0) != 0);
    connect(m_keyNames, &QCheckBox::toggled, this, [this](bool on) {
        o2session_set_int(m_session, "controllers_key_names", on ? 1 : 0);
        m_console->setKeyNames(on);
    });
    m_console->setKeyNames(m_keyNames->isChecked());
    maprow->addWidget(m_mapButton);
    maprow->addWidget(defaults);
    maprow->addWidget(m_keyNames);
    maprow->addStretch(1);
    root->addLayout(maprow);
    m_hint = new ElidedLabel(true, 2);
    root->addWidget(m_hint);

    connect(&m_captureTimer, &QTimer::timeout, this, [this] { pollCapture(); });
    connect(&m_heldTimer, &QTimer::timeout, this, [this] { pollHeld(); });
    connect(&m_slowTimer, &QTimer::timeout, this, [this] { pollSlow(); });
    setMapState(-2);
    rebuild();
}

int ControllersWindow::machineNow() const
{
    const int m = o2session_get_int(m_session, "machine", O2_MACHINE_ODYSSEY2);
    return m >= 0 && m < O2_MACHINE_COUNT ? m : O2_MACHINE_ODYSSEY2;
}

void ControllersWindow::rebuild()
{
    letGo();
    m_builtMachine = machineNow();
    m_console->load(m_builtMachine);
    pollHeld();
}

/* ---- pressing -------------------------------------------------------------------------- */

/* A key cap or a stick control was pressed or let go with the mouse. */
void ControllersWindow::onPress(int target, bool down)
{
    if (m_mapState != -2) {
        if (down) setMapState(target);    /* armed: picks (or changes) the target */
        return;
    }
    if (down) {
        letGo();
        m_mouseTarget = target;
        o2session_press(m_session, target, 1);
    } else if (m_mouseTarget == target) {
        m_mouseTarget = -1;
        o2session_press(m_session, target, 0);
    }
    pollHeld();
}

/* Lets go of what the mouse holds in this window. */
void ControllersWindow::letGo()
{
    if (m_mouseTarget >= 0) {
        o2session_press(m_session, m_mouseTarget, 0);
        m_mouseTarget = -1;
    }
}

/* ---- Map mode -------------------------------------------------------------------------- */

void ControllersWindow::setMapState(int state)
{
    m_mapState = state;
    if (state == -2) {
        m_captureTimer.stop();
        o2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Map"));
        m_mapButton->setStyleSheet(QString());
        m_hint->setText(QStringLiteral("Click and hold a key or stick control to press it."));
    } else if (state == -1) {
        m_captureTimer.stop();
        o2session_gamepad_capture_cancel(m_session);
        m_mapButton->setText(QStringLiteral("Done"));
        m_mapButton->setStyleSheet(o2AccentFill());
        m_hint->setText(QStringLiteral("Click a key cap or a stick control to remap it. Esc leaves Map."));
    } else {
        letGo();
        m_hint->setText(QStringLiteral("Press a key or a gamepad button for %1 (Esc cancels).")
                            .arg(local(o2_target_name(state))));
        o2session_gamepad_capture_begin(m_session);
        m_captureTimer.start(50);
    }
    m_console->setMap(m_mapState);
    for (StickWidget *s : m_stick) s->setMap(m_mapState);
    pollHeld();
}

/* A binding was made: stay armed, since remapping several in a row is normal. */
void ControllersWindow::bound(int target, const QString &what, const char *stolen)
{
    setMapState(-1);
    m_hint->setText(stolen[0] ? QStringLiteral("%1 is now %2 (taken from %3)").arg(local(o2_target_name(target)), what, local(stolen))
                              : QStringLiteral("%1 is now %2").arg(local(o2_target_name(target)), what));
}

void ControllersWindow::pollCapture()
{
    int button;
    if (m_mapState < 0) { m_captureTimer.stop(); return; }
    if (!o2session_gamepad_capture_poll(m_session, &button)) return;
    char stolen[128] = "";
    const int target = m_mapState;
    o2session_binding_set_button(m_session, target, button, stolen, sizeof stolen);
    bound(target, local(o2_pad_button_name(button)), stolen);
}

/* ---- polling ---------------------------------------------------------------------------- */

/* What the console sees held, from every source, lit (25 Hz while visible). */
void ControllersWindow::pollHeld()
{
    m_console->pollHeld(m_mouseTarget);
    for (StickWidget *s : m_stick) s->pollHeld(m_mouseTarget);
}

/* Hot-plugged pads and auto-assignment changes; the machine changed in
 * Preferences. */
void ControllersWindow::pollSlow()
{
    const unsigned gen = o2session_gamepad_generation(m_session);
    if (gen != m_padGen) {
        m_padGen = gen;
        rebuildPads();
    } else {
        refreshPadPorts();
    }
    if (machineNow() != m_builtMachine) rebuild();
}

/* ---- gamepads --------------------------------------------------------------------------- */

void ControllersWindow::refreshPadPorts()
{
    for (int i = 0; i < (int)m_padEff.size(); ++i) {
        const int eff = o2session_gamepad_effective_port(m_session, i);
        const QString text = eff >= 0 ? QStringLiteral("drives stick %1").arg(eff + 1) : QStringLiteral("drives nothing");
        if (m_padEff[(size_t)i]->text() != text) m_padEff[(size_t)i]->setText(text);
    }
}

/* One row per pad: its name, the stick it drives and the stick it is set to. */
void ControllersWindow::rebuildPads()
{
    for (QWidget *w : m_padRows) delete w;
    m_padRows.clear();
    m_padEff.clear();
    const int n = o2session_gamepad_count(m_session);
    if (n == 0) {
        auto *none = new QLabel(QStringLiteral("No gamepads connected"));
        none->setStyleSheet(kGray);
        m_padBox->addWidget(none);
        m_padRows.push_back(none);
        return;
    }
    for (int i = 0; i < n && i < kMaxPads; ++i) {
        char name[128];
        o2session_gamepad_name(m_session, i, name, sizeof name);
        auto *row = new QWidget;
        auto *h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto *label = new ElidedLabel(false);
        label->setText(local(name));
        label->setToolTip(local(name));
        auto *eff = new QLabel;
        eff->setStyleSheet(kGray);
        /* -1 = automatic (connection order), 0-1 = that stick */
        auto *box = new QComboBox;
        box->setFocusPolicy(Qt::NoFocus);
        box->addItems({ QStringLiteral("Automatic"), QStringLiteral("Stick 1"), QStringLiteral("Stick 2") });
        const int assign = o2session_gamepad_assignment(m_session, i);
        box->setCurrentIndex(assign < 0 || assign > 1 ? 0 : assign + 1);
        connect(box, &QComboBox::currentIndexChanged, this, [this, i](int idx) {
            o2session_gamepad_assign(m_session, i, idx <= 0 ? -1 : idx - 1);
            refreshPadPorts();
        });
        h->addWidget(label, 1);
        h->addWidget(eff);
        h->addWidget(box);
        m_padBox->addWidget(row);
        m_padRows.push_back(row);
        m_padEff.push_back(eff);
    }
    refreshPadPorts();
}

/* ---- the window ------------------------------------------------------------------------- */

void ControllersWindow::present()
{
    if (machineNow() != m_builtMachine) rebuild();
    m_padGen = o2session_gamepad_generation(m_session);
    rebuildPads();
    show();
    raise();
}

void ControllersWindow::refresh()
{
    if (isVisible()) rebuild();
    else m_builtMachine = -1;   /* rebuilt when next shown */
}

void ControllersWindow::showEvent(QShowEvent *e)
{
    if (machineNow() != m_builtMachine) rebuild();
    pollHeld();
    m_heldTimer.start(40);
    m_slowTimer.start(250);
    QWidget::showEvent(e);
}

void ControllersWindow::hideEvent(QHideEvent *e)
{
    m_heldTimer.stop();
    m_slowTimer.stop();
    setMapState(-2);
    letGo();
    QWidget::hideEvent(e);
}

/* Only what this window holds: the main window releases its own keys. */
void ControllersWindow::changeEvent(QEvent *e)
{
    if (e->type() == QEvent::ActivationChange && !isActiveWindow()) {
        letGo();
        o2session_release_all(m_session);
        m_swallow = 0;
    }
    QWidget::changeEvent(e);
}

void ControllersWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_F9) {
        if (!e->isAutoRepeat()) hide();
        return;
    }
    if (e->key() == Qt::Key_F12) {
        if (!e->isAutoRepeat()) DebuggerWindow::toggleFor(parentWidget(), m_session);
        return;
    }
    const uint32_t ks = o2KeysymFromQt(e);
    if (m_swallow && ks == m_swallow) return;

    if (m_mapState != -2) {
        if (e->isAutoRepeat()) return;
        /* Esc backs out one step: a pending target, then Map itself */
        if (e->key() == Qt::Key_Escape) {
            m_swallow = ks;
            setMapState(m_mapState >= 0 ? -1 : -2);
            return;
        }
        if (m_mapState < 0 || !ks) return;      /* armed, or nothing to bind: nothing is typed */
        if (o2IsAppKey(e)) {
            m_hint->setText(QStringLiteral("F9, F11 and F12 are the app's and cannot be bound."));
            return;
        }
        char stolen[128] = "", name[48] = "";
        const int target = m_mapState;
        o2session_binding_set_key(m_session, target, ks, stolen, sizeof stolen);
        o2session_keysym_name(ks, name, sizeof name);
        m_swallow = ks;
        bound(target, local(name), stolen);
        return;
    }
    /* Otherwise what the main window does with it; a system action goes to
     * the main window's queue, as a gamepad's does. */
    if (e->isAutoRepeat()) return;
    if (!ks || o2IsAppKey(e)) { QWidget::keyPressEvent(e); return; }
    const int sa = o2session_key_sysaction(m_session, ks);
    if (sa >= 0) {
        o2session_sysaction_post(m_session, sa);
        return;
    }
    if (!o2session_key(m_session, ks, 1)) QWidget::keyPressEvent(e);
}

void ControllersWindow::keyReleaseEvent(QKeyEvent *e)
{
    if (e->isAutoRepeat()) return;
    const uint32_t ks = o2KeysymFromQt(e);
    if (m_swallow && ks == m_swallow) {
        m_swallow = 0;
        return;
    }
    if (m_mapState != -2 || !ks) return;
    if (!o2session_key(m_session, ks, 0)) QWidget::keyReleaseEvent(e);
}

QPointer<ControllersWindow> g_window;

} // namespace

void o2ControllersToggle(QWidget *parent, o2session *session)
{
    if (!g_window) g_window = new ControllersWindow(session, parent);
    if (g_window->isVisible()) g_window->hide();
    else g_window->present();
}

bool o2ControllersVisible()
{
    return g_window && g_window->isVisible();
}

void o2ControllersRefresh()
{
    if (g_window) g_window->refresh();
}
