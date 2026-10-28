#include "theme.h"

#include <QApplication>
#include <QGuiApplication>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSettings>
#include <QEvent>
#include <QWidget>
#include <QDebug>
#include <QTimer>

#include "qcustomplot.h"

#ifdef Q_OS_WASM
#include <emscripten/val.h>
#endif


Theme *Theme::instance()
{
    static Theme *theme = new Theme(qApp);
    return theme;
}

Theme::Theme(QObject *parent) : QObject(parent)
{
}

QString Theme::modeToString(Mode mode)
{
    switch (mode) {
    case Mode::Light: return QStringLiteral("light");
    case Mode::Dark:  return QStringLiteral("dark");
    case Mode::System: break;
    }
    return QStringLiteral("system");
}

Theme::Mode Theme::modeFromString(const QString &s)
{
    if (s == QLatin1String("light")) return Mode::Light;
    if (s == QLatin1String("dark")) return Mode::Dark;
    return Mode::System;
}

void Theme::init()
{
    m_lightStyleName = QApplication::style()->name();

    QSettings settings;
    m_mode = modeFromString(settings.value("Main/Theme", "system").toString());

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Follow the OS / browser while the user has not picked a theme.
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this]() {
        if (m_mode == Mode::System && !m_applying) apply();
    });
#endif
#ifdef Q_OS_WASM
    // The browser can switch prefers-color-scheme while the page is open, and Qt's wasm platform
    // may not report it: check the media query now and then while following the browser.
    {
        QTimer *poll = new QTimer(this);
        poll->setInterval(2000);
        connect(poll, &QTimer::timeout, this, [this]() {
            if (m_mode == Mode::System && !m_applying && systemPrefersDark() != s_dark) apply();
        });
        poll->start();
    }
#endif

    qApp->installEventFilter(this);
    apply();
}

void Theme::setMode(Mode mode)
{
    QSettings().setValue("Main/Theme", modeToString(mode));
    m_mode = mode;
    apply();
}

bool Theme::systemPrefersDark() const
{
#ifdef Q_OS_WASM
    using emscripten::val;
    val mq = val::global("window").call<val>("matchMedia", std::string("(prefers-color-scheme: dark)"));
    if (!mq.isUndefined() && !mq.isNull()) return mq["matches"].as<bool>();
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    // Older Qt: guess from the platform palette.
    const QPalette pal = QGuiApplication::palette();
    return pal.color(QPalette::Window).lightness() < pal.color(QPalette::WindowText).lightness();
#endif
}

QPalette Theme::darkPalette()
{
    QPalette p;
    const QColor window(0x2d, 0x2d, 0x30);
    const QColor base(0x1e, 0x1e, 0x1e);
    const QColor altBase(0x26, 0x26, 0x29);
    const QColor text(0xe0, 0xe0, 0xe0);
    const QColor disabledText(0x80, 0x80, 0x80);
    const QColor button(0x37, 0x37, 0x3b);
    const QColor highlight(0x26, 0x6e, 0xc4);

    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, altBase);
    p.setColor(QPalette::ToolTipBase, QColor(0x3c, 0x3c, 0x40));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x90, 0x90, 0x90));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, button);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(0xff, 0x6b, 0x6b));
    p.setColor(QPalette::Light, QColor(0x4a, 0x4a, 0x50));
    p.setColor(QPalette::Midlight, QColor(0x3e, 0x3e, 0x44));
    p.setColor(QPalette::Mid, QColor(0x2a, 0x2a, 0x2e));
    p.setColor(QPalette::Dark, QColor(0x19, 0x19, 0x1b));
    p.setColor(QPalette::Shadow, QColor(0x0a, 0x0a, 0x0a));
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(0x6c, 0xb4, 0xff));
    p.setColor(QPalette::LinkVisited, QColor(0xb4, 0x8c, 0xff));

    p.setColor(QPalette::Disabled, QPalette::WindowText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Text, disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, disabledText);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x50, 0x50, 0x55));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabledText);
    return p;
}

void Theme::apply()
{
    if (m_applying) return;
    m_applying = true;

    bool dark;
    switch (m_mode) {
    case Mode::Dark:  dark = true; break;
    case Mode::Light: dark = false; break;
    case Mode::System:
    default:
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
        QGuiApplication::styleHints()->unsetColorScheme(); // read the OS's own choice
#endif
        dark = systemPrefersDark();
        break;
    }
    s_dark = dark;

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Tell the platform too (title bars, native dialogs where there are any).
    QGuiApplication::styleHints()->setColorScheme(dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif

    if (dark) {
        if (QApplication::style()->name().compare("fusion", Qt::CaseInsensitive) != 0)
            QApplication::setStyle(QStyleFactory::create("Fusion"));
        QApplication::setPalette(darkPalette());
    } else {
        if (QApplication::style()->name().compare(m_lightStyleName, Qt::CaseInsensitive) != 0) {
            QStyle *style = QStyleFactory::create(m_lightStyleName);
            if (style) QApplication::setStyle(style);
        }
        QApplication::setPalette(QApplication::style()->standardPalette());
    }

    // Re-theme every plot that already exists.
    const QList<QWidget *> widgets = QApplication::allWidgets();
    for (QWidget *w : widgets) {
        if (QCustomPlot *plot = qobject_cast<QCustomPlot *>(w)) {
            applyToPlot(plot);
            plot->replot();
        }
    }

    qDebug() << "Theme:" << modeToString(m_mode) << (dark ? "(dark)" : "(light)");
    m_applying = false;
    emit changed(dark);
}

bool Theme::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::Show) {
        if (QCustomPlot *plot = qobject_cast<QCustomPlot *>(obj)) {
            applyToPlot(plot);
            plot->replot();
        }
    }
    return QObject::eventFilter(obj, event);
}

void Theme::applyToPlot(QCustomPlot *plot)
{
    if (!plot) return;

    const QColor background = s_dark ? QColor(0x1e, 0x1e, 0x1e) : QColor(Qt::white);
    const QColor fg = s_dark ? QColor(0xd0, 0xd0, 0xd0) : QColor(Qt::black);
    const QColor grid = s_dark ? QColor(0x4a, 0x4a, 0x4a) : QColor(200, 200, 200);
    const QColor subGrid = s_dark ? QColor(0x36, 0x36, 0x36) : QColor(220, 220, 220);

    plot->setBackground(QBrush(background));

    const QList<QCPAxisRect *> rects = plot->axisRects();
    for (QCPAxisRect *rect : rects) {
        rect->setBackground(Qt::NoBrush);
        const QList<QCPAxis *> axes = rect->axes();
        for (QCPAxis *axis : axes) {
            axis->setBasePen(QPen(fg, 0));
            axis->setTickPen(QPen(fg, 0));
            axis->setSubTickPen(QPen(fg, 0));
            axis->setTickLabelColor(fg);
            axis->setLabelColor(fg);
            axis->setSelectedBasePen(QPen(QColor(0x50, 0x90, 0xff), 2));
            axis->setSelectedTickLabelColor(QColor(0x50, 0x90, 0xff));
            axis->grid()->setPen(QPen(grid, 0, Qt::DotLine));
            axis->grid()->setSubGridPen(QPen(subGrid, 0, Qt::DotLine));
            axis->grid()->setZeroLinePen(QPen(grid, 0, Qt::SolidLine));
        }
    }

    if (plot->legend) {
        plot->legend->setBrush(QBrush(background));
        plot->legend->setBorderPen(QPen(fg));
        plot->legend->setTextColor(fg);
        plot->legend->setSelectedTextColor(QColor(0x50, 0x90, 0xff));
    }

    // Titles and other text elements in the layout.
    for (int i = 0; i < plot->plotLayout()->elementCount(); ++i) {
        if (QCPTextElement *text = qobject_cast<QCPTextElement *>(plot->plotLayout()->elementAt(i)))
            text->setTextColor(fg);
    }
}
