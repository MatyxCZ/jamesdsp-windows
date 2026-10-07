#include "DesktopServices.h"

#include "utils/Log.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <QMessageBox>
#include <QUrl>

#ifdef _WIN32
// QDesktopServices::openUrl used as a fallback when ShellExecuteW fails
#include <QDesktopServices>
#endif

bool DesktopServices::openUrl(const QString &url, QWidget* parent)
{
#ifdef _WIN32
    // Windows: open the URL via ShellExecuteW (xdg-open/gio/kde-open5 do not exist here)
    if((intptr_t)ShellExecuteW(nullptr, L"open", url.toStdWString().c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32)
    {
        return true;
    }

    Log::warning("ShellExecuteW failed. Falling back to QDesktopServices::openUrl.");

    // Fallback: QDesktopServices::openUrl
    return QDesktopServices::openUrl(QUrl(url));
#else
    // QDesktopServices::openUrl broken on some KDE systems with Qt 5.15
    if(system(QString("xdg-open %1").arg(url).toStdString().c_str()) > 0)
    {
        Log::warning("xdg-open failed. Trying gio instead.");
        if(system(QString("gio open %1").arg(url).toStdString().c_str()) > 0)
        {
            Log::warning("gio failed. Trying kde-open5 instead.");
            if(system(QString("kde-open5 %1").arg(url).toStdString().c_str()) > 0)
            {
                if(parent != nullptr)
                {
                    QMessageBox::critical(parent, QObject::tr("Something went wrong"),
                                          QObject::tr("Failed to open URL with default browser.\n Please copy this URL manually: ") + url);
                }
                return false;
            }
        }
    }

    return true;
#endif
}
