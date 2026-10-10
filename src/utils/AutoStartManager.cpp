#ifdef USE_PORTALS
#include <libportal-qt6/portal-qt6.h>
#endif

#include "AutoStartManager.h"

#include "config/AppConfig.h"
#include "config/ConfigContainer.h"
#include "config/ConfigIO.h"

#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSettings>
#include <QMainWindow>
#include <QCoreApplication>

#define DESKTOP_FILE "jdsp-gui.desktop"
#define AUTOSTART_KEY "JamesDSP"
#define AUTOSTART_REGISTRY_PATH "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"

AutostartManager::AutostartManager(QMainWindow* parent) : QObject(parent)
{
#ifdef _WIN32
    // The Run entry is authoritative, including changes made by the registration script.
    AppConfig::instance().set(AppConfig::AutoStartEnabled, isEnabled());
#endif
}

#ifdef USE_PORTALS
static void onPortalBackgroundRequest(GObject *source_object, GAsyncResult *res, gpointer data) {
    Q_UNUSED(source_object)
    Q_UNUSED(data)

    g_autoptr(GError) error = nullptr;

    if (xdp_portal_request_background_finish(XdpQt::globalPortalObject(), res, &error) == 0) {
        QString reason;
        QString explanation;

        if (error != nullptr && error->code == 19) {
            reason = QObject::tr("Auto-start permission has been denied");
            explanation = QObject::tr("Please run 'flatpak permission-set background background %1 yes' and reenable auto-start.").arg(FLATPAK_APP_ID);
        } else {
            reason = QObject::tr("Unknown error");
            explanation = QObject::tr("Please make sure a XDG Background Portal implementation is available and active to use auto-start. %1").arg((error) != nullptr ? error->message : "");
        }

        Log::error(reason);
        Log::error(explanation);
        // QMessageBox::critical(nullptr, reason, explanation);

        Log::error("Auto-start disabled");
        AppConfig::instance().set(AppConfig::AutoStartEnabled, false);
    }

    Log::information("Background request granted");
}
#endif

void AutostartManager::setEnabled(bool enabled)
{
    AppConfig::instance().set(AppConfig::AutoStartEnabled, enabled);
    setup();
}

void AutostartManager::setup()
{
    bool enabled = AppConfig::instance().get<bool>(AppConfig::AutoStartEnabled);

#ifdef USE_PORTALS
    QMainWindow* window = qobject_cast<QMainWindow*>(parent());
    // Important: windowHandle returns null if the window has not yet finished initialising!
    assert(window && window->windowHandle() && "AutostartManager class must be instantiated using the visible calling window as the parent");
    XdpParent* xdpParent = xdp_parent_new_qt(window->windowHandle());

    g_autoptr(GPtrArray) commandLine = nullptr;
    if(enabled) {
        commandLine = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(commandLine, g_strdup("jamesdsp"));
        g_ptr_array_add(commandLine, g_strdup("--tray"));
    }

    xdp_portal_request_background(
                XdpQt::globalPortalObject(),
                xdpParent,
                tr("Manage auto-start permission for JamesDSP").toLocal8Bit().data(),
                commandLine,
                enabled ? XdpBackgroundFlags::XDP_BACKGROUND_FLAG_AUTOSTART : XdpBackgroundFlags::XDP_BACKGROUND_FLAG_NONE,
                nullptr,
                onPortalBackgroundRequest,
                nullptr
                );
#else
#ifdef _WIN32
    QSettings settings(AUTOSTART_REGISTRY_PATH, QSettings::NativeFormat);
    if(enabled) {
        const QString execPath = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        settings.setValue(AUTOSTART_KEY, QString("\"%1\" --tray").arg(execPath));
    }
    else {
        settings.remove(AUTOSTART_KEY);
    }
    settings.sync();
    if (settings.status() != QSettings::NoError)
    {
        Log::error("Could not update the Windows startup registration");
        AppConfig::instance().set(AppConfig::AutoStartEnabled, isEnabled());
        QMessageBox::critical(qobject_cast<QMainWindow*>(parent()),
                              tr("Windows startup"),
                              tr("Could not update startup registration for the current Windows user."));
    }
#else
    if(!enabled && QFile::exists(getAutostartPath())) {
        QFile::remove(getAutostartPath());
    }
    else if(enabled && !QFile::exists(getAutostartPath())) {
        ConfigContainer *conf = new ConfigContainer();
        conf->setValue("Exec", QString("%0 --tray").arg(AppConfig::instance().get<QString>(AppConfig::ExecutablePath)));
        conf->setValue("Name",                      "JamesDSP for Linux");
        conf->setValue("Icon",                      "jamesdsp");
        conf->setValue("StartupNotify",             false);
        conf->setValue("Terminal",                  false);
        conf->setValue("Type",                      "Application");
        conf->setValue("Version",                   "1.0");
        conf->setValue("X-GNOME-Autostart-Delay",   6);
        conf->setValue("X-GNOME-Autostart-enabled", true);
        conf->setValue("X-KDE-autostart-after",     "panel");
        conf->setValue("X-KDE-autostart-phase",     2);
        conf->setValue("X-MATE-Autostart-Delay",    6);
        ConfigIO::writeFile(getAutostartPath(), conf->getConfigMap(), "[Desktop Entry]");
    }
#endif
#endif
}

bool AutostartManager::isEnabled()
{
#ifdef USE_PORTALS
    return AppConfig::instance().get<bool>(AppConfig::AutoStartEnabled);
#else
#ifdef _WIN32
    QSettings settings(AUTOSTART_REGISTRY_PATH, QSettings::NativeFormat);
    return settings.contains(AUTOSTART_KEY);
#else
    return QFile::exists(getAutostartPath());
#endif
#endif
}

QString AutostartManager::getAutostartPath()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if(path.isEmpty())
    {
        path = QString("%1/.config/").arg(QDir::homePath());
    }

    return QString("%1/autostart/%2").arg(path, DESKTOP_FILE);
}
