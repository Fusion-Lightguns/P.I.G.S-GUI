#include "guiwindow.h"
#include <QApplication>
#include <QLocale>
#include <QTranslator>
#include <QFile>
#include <QTextStream>

// Function to load and apply the fusion theme
void loadfusionTheme(QApplication &app) {
    QFile file(":/fusion.qss");
    if (file.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&file);
        QString style = ts.readAll();
        app.setStyleSheet(style);
    }
}

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    a.setWindowIcon(QIcon(":/images/pigs_logo.png"));

    QTranslator translator;
    const QStringList uiLanguages = QLocale::system().uiLanguages();
    for (const QString &locale : uiLanguages) {
        const QString baseName = "PIGS-GUImain_" + QLocale(locale).name();
        if (translator.load(":/i18n/" + baseName)) {
            a.installTranslator(&translator);
            break;
        }
    }
    // Load the fusion theme
    loadfusionTheme(a);

    // Create the main window
    guiWindow w;
    w.setWindowState(Qt::WindowMaximized);
    w.show();


    return a.exec();
}

