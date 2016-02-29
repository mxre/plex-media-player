#include <locale.h>

#include <QGuiApplication>
#include <QFileInfo>
#include <QIcon>
#include <QtQml>
#include <QtWebEngine/qtwebengineglobal.h>
#include <shared/Names.h>

#include "system/SystemComponent.h"
#include "system/UpdateManager.h"
#include "QsLog.h"
#include "Paths.h"
#include "player/PlayerComponent.h"
#include "breakpad/CrashDumps.h"
#include "Version.h"
#include "settings/SettingsComponent.h"
#include "settings/SettingsSection.h"
#include "ui/KonvergoWindow.h"
#include "ui/KonvergoEngine.h"
#include "UniqueApplication.h"
#include "utils/HelperLauncher.h"

#if defined(Q_OS_MAC) || defined(Q_OS_LINUX)
#include "SignalManager.h"
#endif

using namespace QsLogging;

/////////////////////////////////////////////////////////////////////////////////////////
static void preinitQt()
{
  QCoreApplication::setApplicationName(Names::MainName());
  QCoreApplication::setApplicationVersion(Version::GetVersionString());
  QCoreApplication::setOrganizationDomain("plex.tv");

#ifdef Q_OS_WIN32
  QVariant useOpengl = SettingsComponent::readPreinitValue(SETTINGS_SECTION_MAIN, "useOpenGL");

  // Warning: this must be the same as the default value as declared in
  // the settings_description.json file, or confusion will result.
  if (useOpengl.type() != QMetaType::Bool)
    useOpengl = false;

  if (useOpengl.toBool())
    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  else
    QCoreApplication::setAttribute(Qt::AA_UseOpenGLES);
#endif
}

/////////////////////////////////////////////////////////////////////////////////////////
static void qtMessageOutput(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    QByteArray localMsg = msg.toLocal8Bit();
    QString prefix;
    if (context.line)
      prefix = QString("%1:%2:%3: ").arg(context.file).arg(context.line).arg(context.function);
    QString text = prefix + msg;
    switch (type)
    {
      case QtDebugMsg:
        QLOG_DEBUG() << text;
        break;
      case QtInfoMsg:
        QLOG_INFO() << text;
        break;
      case QtWarningMsg:
        QLOG_WARN() << text;
        break;
      case QtCriticalMsg:
        QLOG_ERROR() << text;
        break;
      case QtFatalMsg:
        QLOG_FATAL() << text;
        break;
    }
}

/////////////////////////////////////////////////////////////////////////////////////////
static void elidePattern(QString& msg, const QString& substring, int chars)
{
  int start = 0;
  while (true)
  {
    start = msg.indexOf(substring, start);
    if (start < 0 || start + substring.length() + chars > msg.length())
      break;
    start += substring.length();
    for (int n = 0; n < chars; n++)
      msg[start + n] = QChar('x');
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
static void processLog(QString& msg)
{
  elidePattern(msg, "X-Plex-Token=", 20);
  elidePattern(msg, "X-Plex-Token%3D", 20);
}

/////////////////////////////////////////////////////////////////////////////////////////
void initLogger()
{
  // Note where the logfile is going to be
  qDebug("Logging to %s", qPrintable(Paths::logDir(Names::MainName() + ".log")));

  // init logging.
  DestinationPtr dest = DestinationFactory::MakeFileDestination(
    Paths::logDir(Names::MainName() + ".log"),
    EnableLogRotationOnOpen,
    MaxSizeBytes(1024 * 1024),
    MaxOldLogCount(9));

  Logger::instance().addDestination(dest);
  Logger::instance().setLoggingLevel(DebugLevel);
  Logger::instance().setProcessingCallback(processLog);

  qInstallMessageHandler(qtMessageOutput);
}

static QsLogging::Level logLevelFromString(const QString& str)
{
  if (str == "trace")     return QsLogging::Level::TraceLevel;
  if (str == "debug")     return QsLogging::Level::DebugLevel;
  if (str == "info")      return QsLogging::Level::InfoLevel;
  if (str == "warn")      return QsLogging::Level::WarnLevel;
  if (str == "error")     return QsLogging::Level::ErrorLevel;
  if (str == "fatal")     return QsLogging::Level::FatalLevel;
  if (str == "disable")   return QsLogging::Level::OffLevel;
  // if not valid, use default
  return QsLogging::Level::DebugLevel;
}

static void updateLogLevel()
{
  QString level = SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "logLevel").toString();
  if (level.size())
  {
    QLOG_INFO() << "Setting log level to:" << level;
    Logger::instance().setLoggingLevel(logLevelFromString(level));
  }
}

/////////////////////////////////////////////////////////////////////////////////////////
char** appendCommandLineArguments(int *argc, char **argv)
{
  static char *newArgs[16];
  QList<QString> argList;

  // Copy argv list to our StringList
  for (int i=0; i < *argc; i++)
  {
    argList << QString(argv[i]);
  }

  // add any required additionnal commandline argument
#if KONVERGO_OPENELEC
  // on RPI with webengine, OpenGL contexts are shared statically with webengine
  // which avoids proper reset when switching display mode
  // On OE we also need that because there is a crash with OZONE otherwise
  argList << "--disable-gpu";
#endif

  // with webengine we need those to have a proper scaling of the webview in the window
  argList << "--enable-viewport";
  argList << "--enable-viewport-meta";

  // Now rebuild our argc, argv list
  *argc = argList.size();

  for(int iarg=0; iarg < argList.size(); iarg++)
  {
    newArgs[iarg] = (char*)malloc(256);
    strcpy(newArgs[iarg], argList.value(iarg).toStdString().c_str());
  }

  return (char**)newArgs;
}

/////////////////////////////////////////////////////////////////////////////////////////
int main(int argc, char *argv[])
{
  bool startHidden = false;

  try
  {
    for (int n = 1; n < argc; n++)
    {
      if (qstrcmp(argv[n], "--licenses") == 0)
      {
        QFile licenses(":/misc/licenses.txt");
        licenses.open(QIODevice::ReadOnly | QIODevice::Text);
        QByteArray contents = licenses.readAll();
        printf("%.*s\n", (int)contents.size(), contents.data());
        return 0;
      }
      else if (qstrcmp(argv[n], "--hidden") == 0)
      {
        startHidden = true;
        fprintf(stdout, "Starting hidden\n");
      }
    }

    int newArgc = argc;
    char **newArgv = appendCommandLineArguments(&newArgc, argv);

    // Suppress SSL related warnings on OSX
    // See https://bugreports.qt.io/browse/QTBUG-43173 for more info
    //
#ifdef Q_OS_MAC
    qputenv("QT_LOGGING_RULES", "qt.network.ssl.warning=false");

    // Request OpenGL 4.1 if possible on OSX, otherwise it defaults to 2.0
    // This needs to be done before we create the QGuiApplication
    //
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setMajorVersion(3);
    format.setMinorVersion(2);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
#endif

    preinitQt();
    QGuiApplication app(newArgc, newArgv);
    app.setWindowIcon(QIcon(":/images/icon.png"));

    // init breakpad.
    setupCrashDumper();

    UniqueApplication* uniqueApp = new UniqueApplication();
    if (!uniqueApp->ensureUnique())
      return EXIT_SUCCESS;

#ifdef Q_OS_UNIX
    // install signals handlers for proper app closing.
    SignalManager signalManager(&app);
    Q_UNUSED(signalManager);
#endif

    initLogger();
    QLOG_INFO() << "Starting Plex Media Player version:" << qPrintable(Version::GetVersionString()) << "build date:" << qPrintable(Version::GetBuildDate());
    QLOG_INFO() << qPrintable(QString("  Running on: %1 [%2] arch %3").arg(QSysInfo::prettyProductName()).arg(QSysInfo::kernelVersion()).arg(QSysInfo::currentCpuArchitecture()));
    QLOG_INFO() << "  Qt Version:" << QT_VERSION_STR << qPrintable(QString("[%1]").arg(QSysInfo::buildAbi()));

    // Quit app and apply update if we find one.
    if (UpdateManager::CheckForUpdates())
    {
      app.quit();
      return 0;
    }

#ifdef Q_OS_WIN32
    initD3DDevice();
#endif

#ifdef Q_OS_UNIX
    setlocale(LC_NUMERIC, "C");
#endif

    // Initialize all the components. This needs to be done
    // early since most everything else relies on it
    //
    ComponentManager::Get().initialize();

    // enable remote inspection if we have the correct setting for it.
    if (SettingsComponent::Get().value(SETTINGS_SECTION_MAIN, "remoteInspector").toBool())
      qputenv("QTWEBENGINE_REMOTE_DEBUGGING", "0.0.0.0:9992");

    QtWebEngine::initialize();

    // Qt and QWebEngineProfile set the locale, which breaks parsing and
    // formatting float numbers in a few countries.
#ifdef Q_OS_UNIX
    setlocale(LC_NUMERIC, "C");
#endif

    // start our helper
    HelperLauncher::Get().connectToHelper();

    // load QtWebChannel so that we can register our components with it.
    updateLogLevel();

    if (!startHidden)
      KonvergoEngine::Get().loadUI();

    // run our application
    int ret = app.exec();

    KonvergoEngine::Get().unload();

    delete uniqueApp;
    return ret;
  }
  catch (FatalException& e)
  {

    QLOG_FATAL() << "Unhandled FatalException:" << qPrintable(e.message());

    QGuiApplication app(argc, argv);
    QString text = e.message() + "<br>" + QObject::tr("Please visit Plex support forums for support.");

    QQmlApplicationEngine* engine = new QQmlApplicationEngine(NULL);
    engine->rootContext()->setContextProperty("errorTitle", QObject::tr("A critical error occurred."));
    engine->rootContext()->setContextProperty("errorText", text);
    engine->load(QUrl(QStringLiteral("qrc:/ui/errormessage.qml")));

    app.exec();
    return 1;

  }
}
