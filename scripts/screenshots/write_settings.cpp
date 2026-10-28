// Writes SavvyCAN settings (QSettings IniFormat) the way the app does, for screenshot runs:
//   write_settings FILE Group/key=value ... Group/key=@ints:1,2,3 Group/key=@strings:a,b
// "@size:W,H", "@point:X,Y" as QSize/QPoint; "@ints:" stores a QVariantList of ints, "@strings:" a QStringList (as ConnectionWindow::saveConnections).
// Build: g++ -std=c++17 -fPIC write_settings.cpp -I$QT/include -I$QT/include/QtCore -L$QT/lib -lQt6Core -Wl,-rpath,$QT/lib
#include <QCoreApplication>
#include <QSettings>
#include <QStringList>
#include <QVariantList>
#include <QSize>
#include <QPoint>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 3) { std::fprintf(stderr, "usage: write_settings FILE key=value...\n"); return 2; }
    QSettings s(QString::fromLocal8Bit(argv[1]), QSettings::IniFormat);
    for (int i = 2; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        const int eq = arg.indexOf('=');
        if (eq < 1) { std::fprintf(stderr, "bad argument: %s\n", argv[i]); return 2; }
        const QString key = arg.left(eq), value = arg.mid(eq + 1);
        if (value.startsWith("@ints:")) {
            QVariantList list;
            for (const QString &v : value.mid(6).split(',', Qt::SkipEmptyParts)) list << v.toInt();
            s.setValue(key, list);
        } else if (value.startsWith("@size:")) {
            const QStringList p = value.mid(6).split(',');
            s.setValue(key, QSize(p.value(0).toInt(), p.value(1).toInt()));
        } else if (value.startsWith("@point:")) {
            const QStringList p = value.mid(7).split(',');
            s.setValue(key, QPoint(p.value(0).toInt(), p.value(1).toInt()));
        } else if (value.startsWith("@strings:")) {
            s.setValue(key, value.mid(9).split(','));
        } else if (value == "true" || value == "false") {
            s.setValue(key, value == "true");
        } else {
            s.setValue(key, value);
        }
    }
    s.sync();
    return s.status() == QSettings::NoError ? 0 : 1;
}
