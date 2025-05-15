#ifndef ELEVATIONHELPER_H
#define ELEVATIONHELPER_H

#include <QObject>
#include <QString>
#include <QLocalSocket>
#include <QTimer>
#include <windows.h>

class ElevationHelper : public QObject
{
    Q_OBJECT
public:
    explicit ElevationHelper(QObject *parent = nullptr);
    ~ElevationHelper();
    
    // Run the helper with admin privileges to format a drive
    bool runFormatDrive(const QString &drive);
    
    // Run the helper with admin privileges to write an image to a drive
    bool runWriteToDrive(const QString &drive, const QString &sourceFile);
    
signals:
    void error(const QString &message);
    void downloadProgress(qint64 now, qint64 total);
    void verifyProgress(qint64 now, qint64 total);
    void writeProgress(qint64 now, qint64 total);
    
private slots:
    void socketReadyRead();
    void processPendingEvents();
    void handleSocketError(QLocalSocket::LocalSocketError socketError);
    
private:
    bool runHelperWithArgs(const QString &args);
    QString generateSocketName();
    
    QLocalSocket *m_socket;
    QString m_socketName;
    HANDLE m_helperProcess;
    QTimer m_eventTimer;
    bool m_operationComplete;
};

#endif // ELEVATIONHELPER_H 