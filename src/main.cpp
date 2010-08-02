/* This file is part of Clementine.

   Clementine is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Clementine is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Clementine.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifdef Q_OS_WIN32
#  define _WIN32_WINNT 0x0500
#  include <windows.h>
#  include <iostream>
#endif // Q_OS_WIN32

#include "config.h"
#include "core/commandlineoptions.h"
#include "core/mac_startup.h"
#include "core/networkaccessmanager.h"
#include "core/player.h"
#include "core/potranslator.h"
#include "core/song.h"
#include "engines/enginebase.h"
#include "library/directory.h"
#include "radio/lastfmservice.h"
#include "ui/equalizer.h"
#include "ui/iconloader.h"
#include "ui/mainwindow.h"

#include "qtlocalpeer.h"
#include "qtsingleapplication.h"
#include "qtsinglecoreapplication.h"

#include <QtDebug>
#include <QLibraryInfo>
#include <QTranslator>
#include <QDir>

#include <glib-object.h>
#include <glib/gutils.h>

#ifdef Q_WS_X11
#  include <QDBusConnection>
#  include <QDBusMetaType>
#  include "core/mpris.h"
#  include "widgets/osd.h"
#endif

#ifdef HAVE_GSTREAMER
#  include <gst/gstbuffer.h>
   class GstEnginePipeline;
#endif

// Load sqlite plugin on windows and mac.
#ifdef HAVE_STATIC_SQLITE
# include <QtPlugin>
  Q_IMPORT_PLUGIN(qsqlite)
#endif

void LoadTranslation(const QString& prefix, const QString& path,
                     const QString& override_language = QString()) {
#if QT_VERSION < 0x040700
  // QTranslator::load will try to open and read "clementine" if it exists,
  // without checking if it's a file first.
  // This was fixed in Qt 4.7
  QFileInfo maybe_clementine_directory(path + "/clementine");
  if (maybe_clementine_directory.exists() && !maybe_clementine_directory.isFile())
    return;
#endif

  QString language = override_language.isEmpty() ?
                     QLocale::system().name() : override_language;

  QTranslator* t = new PoTranslator;
  if (t->load(prefix + "_" + language, path))
    QCoreApplication::installTranslator(t);
  else
    delete t;
}

void GLog(const gchar* domain,
          GLogLevelFlags level,
          const gchar* message,
          gpointer user_data) {
  qDebug() << "GLOG" << message;
}

int main(int argc, char *argv[]) {
#ifdef Q_OS_DARWIN
  // Do Mac specific startup to get media keys working.
  // This must go before QApplication initialisation.
  mac::MacMain();
#endif

  QCoreApplication::setApplicationName("Clementine");
  QCoreApplication::setApplicationVersion(CLEMENTINE_VERSION_STRING);
  QCoreApplication::setOrganizationName("Clementine");
  QCoreApplication::setOrganizationDomain("davidsansome.com");

  // This makes us show up nicely in gnome-volume-control
  g_type_init();
  g_set_application_name(QCoreApplication::applicationName().toLocal8Bit());

  g_log_set_default_handler(&GLog, NULL);

  qRegisterMetaType<Directory>("Directory");
  qRegisterMetaType<DirectoryList>("DirectoryList");
  qRegisterMetaType<Subdirectory>("Subdirectory");
  qRegisterMetaType<SubdirectoryList>("SubdirectoryList");
  qRegisterMetaType<SongList>("SongList");
  qRegisterMetaType<PlaylistItemList>("PlaylistItemList");
  qRegisterMetaType<Engine::State>("Engine::State");
  qRegisterMetaType<Engine::SimpleMetaBundle>("Engine::SimpleMetaBundle");
  qRegisterMetaType<Equalizer::Params>("Equalizer::Params");
  qRegisterMetaTypeStreamOperators<Equalizer::Params>("Equalizer::Params");
  qRegisterMetaType<const char*>("const char*");
  qRegisterMetaType<QNetworkReply*>("QNetworkReply*");

#ifdef HAVE_GSTREAMER
  qRegisterMetaType<GstBuffer*>("GstBuffer*");
  qRegisterMetaType<GstEnginePipeline*>("GstEnginePipeline*");
#endif

  lastfm::ws::ApiKey = LastFMService::kApiKey;
  lastfm::ws::SharedSecret = LastFMService::kSecret;

  // Parse commandline options - need to do this before starting the
  // QApplication so it works without an X server
  CommandlineOptions options(argc, argv);
  if (!options.Parse())
    return 1;

  {
    // Only start a core application now so we can check if there's another
    // Clementine running without needing an X server.
    QtSingleCoreApplication a(argc, argv);
    if (a.isRunning()) {
      if (options.is_empty()) {
        qDebug() << "Clementine is already running - activating existing window";
      }
      if (a.sendMessage(options.Serialize(), 5000)) {
        return 0;
      }
      // Couldn't send the message so start anyway
    }
  }

  // Detect technically invalid usage of non-ASCII in ID3v1 tags.
  UniversalEncodingHandler handler;
  TagLib::ID3v1::Tag::setStringHandler(&handler);

  QtSingleApplication a(argc, argv);
#ifdef Q_OS_DARWIN
  QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + "/../PlugIns");
#endif

  a.setQuitOnLastWindowClosed(false);
  a.isRunning(); // Otherwise QtLocalPeer won't lock the lockfile :S

  // Gnome on Ubuntu has menu icons disabled by default.  I think that's a bad
  // idea, and makes some menus in Clementine look confusing.
  QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, false);

  // Resources
  Q_INIT_RESOURCE(data);
  Q_INIT_RESOURCE(translations);

  // Has the user forced a different language?
  QString language = options.language();
  if (language.isEmpty()) {
    QSettings s;
    s.beginGroup("General");
    language = s.value("language").toString();
  }

  // Translations
  LoadTranslation("qt", QLibraryInfo::location(QLibraryInfo::TranslationsPath), language);
  LoadTranslation("clementine", ":/translations", language);
  LoadTranslation("clementine", a.applicationDirPath(), language);
  LoadTranslation("clementine", QDir::currentPath(), language);

  // Icons
  IconLoader::Init();

  NetworkAccessManager network;

  // MPRIS DBus interface.
#ifdef Q_WS_X11
  qDBusRegisterMetaType<DBusStatus>();
  qDBusRegisterMetaType<Version>();
  qDBusRegisterMetaType<QImage>();
  QDBusConnection::sessionBus().registerService("org.mpris.clementine");
  MPRIS mpris;
#endif

  // Window
  MainWindow w(&network, options.engine());

  QObject::connect(&a, SIGNAL(messageReceived(QByteArray)), &w, SLOT(CommandlineOptionsReceived(QByteArray)));
  w.CommandlineOptionsReceived(options);

  return a.exec();
}
