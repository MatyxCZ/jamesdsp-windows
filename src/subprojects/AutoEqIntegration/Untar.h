#ifndef UNTAR_H
#define UNTAR_H

#include <QDebug>
#ifdef _WIN32
#include <QProcess>
#else
#include <archive.h>
#include <archive_entry.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

class Untar
{
public:
    static int extract(const QString& qFilename, const QDir& outputPath, QString& returnState)
    {
#ifdef _WIN32
        QProcess extractor;
        extractor.start("tar", QStringList() << "-xvf" << qFilename << "-C" << outputPath.path());
        if (!extractor.waitForStarted() || !extractor.waitForFinished(-1) ||
            extractor.exitStatus() != QProcess::NormalExit || extractor.exitCode() != 0)
        {
            returnState = QString::fromLocal8Bit(extractor.readAllStandardError());
            if (returnState.trimmed().isEmpty())
            {
                if (extractor.error() != QProcess::UnknownError)
                    returnState = extractor.errorString();
                else
                    returnState = QStringLiteral("tar failed with exit code %1 (exit status %2)")
                                      .arg(extractor.exitCode())
                                      .arg(static_cast<int>(extractor.exitStatus()));
            }
            return 1;
        }
        return 0;
#else
        const QByteArray filenameBytes = qFilename.toUtf8();
        const char* filename = filenameBytes.constData();

        struct archive *a;
        struct archive *ext;
        struct archive_entry *entry;
        int flags;
        int r;

        /* Select which attributes we want to restore. */
        flags = ARCHIVE_EXTRACT_TIME;

        a = archive_read_new();
        archive_read_support_format_all(a);
        archive_read_support_filter_all(a);
        ext = archive_write_disk_new();
        archive_write_disk_set_options(ext, flags);
        archive_write_disk_set_standard_lookup(ext);
        if ((r = archive_read_open_filename(a, filename, 10240)))
        {
            returnState = archive_error_string(a);
            qWarning() << returnState << "\n";
            return (1);
        }
        for (;;) {
            r = archive_read_next_header(a, &entry);
            if (r == ARCHIVE_EOF)
                break;
            if (r < ARCHIVE_OK)
                qWarning() << archive_error_string(a);
            if (r < ARCHIVE_WARN)
            {
                returnState = archive_error_string(a);
                return 1;
            }

            const char* currentFile = archive_entry_pathname(entry);
            const QString fullOutputPath = outputPath.path() + QDir::separator() + currentFile;
            archive_entry_set_pathname(entry, fullOutputPath.toLatin1().constData());

            r = archive_write_header(ext, entry);
            if (r < ARCHIVE_OK)
                qWarning() << archive_error_string(ext);
            else if (archive_entry_size(entry) > 0) {
                r = copy_data(a, ext);
                if (r < ARCHIVE_OK)
                    qWarning() << archive_error_string(ext);
                if (r < ARCHIVE_WARN)
                {
                    returnState = archive_error_string(ext);
                    return 1;
                }
            }
            r = archive_write_finish_entry(ext);
            if (r < ARCHIVE_OK)
                qWarning() << archive_error_string(ext);
            if (r < ARCHIVE_WARN)
            {
                returnState = archive_error_string(ext);
                return 1;
            }
        }
        archive_read_close(a);
        archive_read_free(a);
        archive_write_close(ext);
        archive_write_free(ext);
        return 0;
#endif
    }

private:
#ifndef _WIN32
    static int copy_data(struct archive *ar, struct archive *aw)
    {
        int r;
        const void *buff;
        size_t size;
        int64_t offset;

        for (;;)
        {
            r = archive_read_data_block(ar, &buff, &size, &offset);
            if (r == ARCHIVE_EOF)
                return ARCHIVE_OK;
            if (r < ARCHIVE_OK)
            {
                qWarning() << "copyData(): " << archive_error_string(ar) << "\n";
                return r;
            }
            r = archive_write_data_block(aw, buff, size, offset);
            if (r < ARCHIVE_OK)
            {
                qWarning() << "copyData(): " << archive_error_string(aw) << "\n";
                return r;
            }
        }
    }
#endif

};

#endif // UNTAR_H
