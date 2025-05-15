#ifndef DISKWRITER_HELPER_H
#define DISKWRITER_HELPER_H

#include <QObject>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QProcess>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>
#include <QFile>
#include <windows.h>

class DiskWriterHelper : public QObject
{
    Q_OBJECT
public:
    explicit DiskWriterHelper(QObject *parent = nullptr);
    ~DiskWriterHelper();

    // Parse command line arguments and execute the requested operation
    int executeFromCommandLine(const QStringList &args);

private slots:
    void onProgressChanged(qint64 now, qint64 total);
    void onDownloadProgress(qint64 now, qint64 total);
    void onVerifyProgress(qint64 now, qint64 total);
    void onNewConnection();
    void sendProgressUpdate(int progressType, qint64 now, qint64 total);
    void progressTimeout();

private:
    bool formatDrive(const QString &drive);
    bool writeToDrive(const QString &drive, const QString &sourceFile);
    bool writeImageToDevice(const QString &sourceFile, const QString &devicePath);
    bool runHelperWithArgs(const QString &args);
    QString generateSocketName();
    
    QLocalServer *m_server;
    QLocalSocket *m_clientConnection;
    QString m_socketName;
    QTimer m_progressTimer;
    
    // For tracking progress during direct writes
    qint64 m_bytesTotal;
    qint64 m_bytesWritten;
    
    // Progress update types
    enum ProgressType {
        DownloadProgress = 1,
        VerifyProgress = 2,
        WriteProgress = 3
    };

    // Function to check if running as administrator
    bool isRunningAsAdmin() const;
};

#endif // DISKWRITER_HELPER_H 