#include "controller.h"
#include "cluster_architecture.h"
#include "table_userid.h"
#include "ranks_instances.h"
#include "customlinegeometry.h"
#include "heatmap_matrix.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QObject>
#include <QQuickWindow>

#include <QQuickView>

#include <QtQuick3D/qquick3d.h>

#include <QLoggingCategory>
#include <QFile>
#include <QStandardPaths>
#include <QVariantMap>

// Dev-Modus (CMake-Option EDUMPI_DEV_MODE): Verbindungswerte des lokalen Dev-Clusters aus der
// Datei in EDUMPI_DEV_ENV (EduMPI/dev/gui.env). Ohne Flag oder Datei: enabled = false.
static QVariantMap loadDevConfig()
{
    QVariantMap config{{"enabled", false}};
#ifdef EDUMPI_DEV_MODE
    const QString path = qEnvironmentVariable("EDUMPI_DEV_ENV");
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "[DEV] EDUMPI_DEV_ENV nicht gesetzt oder nicht lesbar - Dev-Modus aus"
                   << "(vorher 'source EduMPI/dev/ssh/activate')";
        return config;
    }
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        const qsizetype eq = line.indexOf('=');
        if (line.startsWith('#') || eq <= 0)
            continue;
        const QString key = line.left(eq);
        // Gesetzte Umgebungsvariablen haben Vorrang, wie bei dev/compose.sh
        config[key] = qEnvironmentVariable(key.toUtf8().constData(), line.mid(eq + 1));
    }
    // Die GUI ruft ssh fest mit -p 22 auf; nur der Wrapper leitet auf den Dev-Cluster um
    const QString ssh = QStandardPaths::findExecutable("ssh");
    config["sshWrapper"] = ssh.contains("/dev/ssh/bin/");
    if (!config["sshWrapper"].toBool())
        qWarning() << "[DEV] ssh ist nicht der Wrapper aus EduMPI/dev/ssh/bin:" << ssh;
    config["enabled"] = true;
#endif
    return config;
}

int main(int argc, char *argv[])
{
    qputenv("QT_IM_MODULE", QByteArray());
    qputenv("QT_WAYLAND_DISABLE_TEXT_INPUT", "1");
    qputenv("QT_LOGGING_RULES", "qt.qpa.wayland.textinput=false");

    #ifdef Q_OS_MACOS
    //Prevent using the macOS global menubar integration
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeMenuBar);
    #endif


    //QLoggingCategory::setFilterRules(QStringLiteral("qt.scenegraph.general=true"));


    //QScopedPointer<Database_Connection> db(new Database_Connection);

    QGuiApplication app(argc, argv);

    QQmlApplicationEngine engine;

    //engine.addImportPath("qrc:/");
    //engine.addImportPath("qrc:/Sidebar/"); // Speziell für Sidebar

    const char *homeDir = getenv("HOME");
    if (homeDir == nullptr) {
        homeDir = ""; // Fallback, falls HOME nicht definiert ist
    }

    // Übergabe des Home-Verzeichnisses an QML
    engine.rootContext()->setContextProperty("homeDirectory", QString(homeDir));

    // Set default pictures path for screenshots
    engine.rootContext()->setContextProperty("picturesDirectory",
                                             QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));

    engine.rootContext()->setContextProperty("devConfig", loadDevConfig());

    //qmlRegisterType<CustomLineGeometry>("CustomGeometry", 1, 0, "CustomLineGeometry");


    //qmlRegisterType<Controller>("appGui_Cluster.Controller", 1, 0, "Controller");
    //qmlRegisterType<Cluster_Architecture>("appGui_Cluster.Nodes", 1, 0, "Nodes_List");
    //qmlRegisterType<Table_UserID>("Qt.db.models", 1, 0, "TableUserID");

    //const QUrl url(u"qrc:/GUI_Cluster/main.qml"_qs);

    const QUrl url(QStringLiteral("qrc:/GUI_Cluster/main.qml"));

    /*QObject::connect(&engine, &QQmlApplicationEngine::objectCreated,
                     &app, [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);*/
    engine.load(url);
    //qmlRegisterSingletonInstance("Qt.db.qobjectSingleton", 1, 0, "NodesList", db.get());

    //Database_Connection dbi;

    //QQmlContext* context = engine.rootContext();

    //context->setContextProperty("NodesList", &dbi);
    //qDebug() << "Current thread Main Main:" << QThread::currentThread();

    return app.exec();
}
