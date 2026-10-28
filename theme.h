#ifndef THEME_H
#define THEME_H

/*
 * web-savvycan: the light/dark theme switch.
 *
 * - Light is SavvyCAN's normal look (the platform style with its standard, light palette).
 * - Dark is the Fusion style with a dark palette, plus a dark set of per-ID row colours
 *   (Utility::colorForString) and dark QCustomPlot colours for every plot.
 * - The choice is kept in QSettings ("Main/Theme": "system", "light" or "dark"; in the browser
 *   QSettings is the page's localStorage, see main.cpp). Until the user picks, the app follows the
 *   OS / browser colour scheme (prefers-color-scheme).
 *
 * Plots: every QCustomPlot is themed when it is shown (an application event filter), and again on
 * every switch, so windows enabled in later stages get dark plots without code of their own.
 */

#include <QObject>
#include <QPalette>
#include <QString>

class QCustomPlot;

class Theme : public QObject
{
    Q_OBJECT

public:
    enum class Mode { System, Light, Dark };

    static Theme *instance();

    // Call once, after the QApplication exists and before the first window is created.
    void init();

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);              // stores the choice and applies it

    // The theme in effect now (System resolved to light or dark).
    static bool isDark() { return s_dark; }

    static QString modeToString(Mode mode);
    static Mode modeFromString(const QString &s);

    // Theme a plot: background, axes, ticks, labels, grid, legend. Called automatically for every
    // QCustomPlot that is shown; call it again if a window resets its plot's colours.
    static void applyToPlot(QCustomPlot *plot);

signals:
    void changed(bool dark);

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    explicit Theme(QObject *parent = nullptr);
    void apply();
    bool systemPrefersDark() const;
    static QPalette darkPalette();

    Mode m_mode = Mode::System;
    QString m_lightStyleName;   // the style the app started with (SavvyCAN's normal look)
    bool m_applying = false;
    inline static bool s_dark = false; // in the header: utility.h reads it without linking theme.cpp
};

#endif // THEME_H
