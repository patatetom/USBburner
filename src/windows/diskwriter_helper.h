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
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <windows.h>

// Common QDataStream version for all IPC communication
#define RPI_IMAGER_IPC_VERSION QDataStream::Qt_6_0

class DiskWriterHelper : public QObject
{
    Q_OBJECT
public:
    explicit DiskWriterHelper(QObject *parent = nullptr);
    ~DiskWriterHelper();

    // Parse command line arguments and execute the requested operation
    int executeFromCommandLine(const QStringList &args);
    
    // Start in daemon mode, listening for commands
    int startDaemonMode(const QString &socketName);

    // Helper connection states
    enum ConnectionState {
        Idle,               // Initial state, waiting for connection
        Connected,          // Client connected, but handshake not yet complete
        HandshakeSending,   // Sending initial handshake
        HandshakeReceiving, // Waiting for handshake response
        Ready,              // Handshake complete, ready to receive commands
        Processing,         // Currently processing a command
        Error               // Error state
    };
    
    // Progress update types
    enum ProgressType {
        DownloadProgress = 1,
        VerifyProgress = 2,
        WriteProgress = 3
    };

private slots:
    void onProgressChanged(qint64 now, qint64 total);
    void onDownloadProgress(qint64 now, qint64 total);
    void onVerifyProgress(qint64 now, qint64 total);
    void onNewConnection();
    void onClientDataReceived();
    void onClientDisconnected();
    void progressTimeout();

private:
    // Core operations
    bool formatDrive(const QString &drive);
    bool writeToDrive(const QString &drive, const QString &sourceFile);
    bool writeImageToDevice(const QString &sourceFile, const QString &devicePath);
    bool runFat32Format(const QString &driveLetter);
    bool customizeImage(const QString &drive, const QByteArray &config, 
                      const QByteArray &cmdline, const QByteArray &firstrun,
                      const QByteArray &cloudinit, const QByteArray &cloudInitNetwork,
                      const QByteArray &initFormat);
    bool verifyImage(const QString &drive, const QString &sourceFile, const QByteArray &expectedHash);
    
    // Command and message handling
    bool processCommand(const QString &command);
    void sendProgressUpdate(int progressType, qint64 now, qint64 total);
    void sendCompletionStatus(const QString &status);
    
    // State machine helpers
    void changeState(ConnectionState newState);
    QString stateToString(ConnectionState state) const;
    bool validateStateForCommand();
    void resetState();
    
    // Helper methods
    bool runHelperWithArgs(const QString &args);
    QString generateSocketName();
    bool isRunningAsAdmin() const;
    
    // Server and connection
    QLocalServer *m_server;
    QLocalSocket *m_clientConnection;
    QString m_socketName;
    QTimer m_progressTimer;
    bool m_daemonMode;
    
    // For tracking progress during direct writes
    qint64 m_bytesTotal;
    qint64 m_bytesWritten;
    
    // For verification
    QCryptographicHash m_verifyHash;
    QByteArray m_sourceHash;
    
    // State tracking
    ConnectionState m_connectionState;
    QElapsedTimer m_operationTimer;
    int m_operationTimeoutMs;
    bool m_currentOperationSuccess;
    QString m_currentCommand;
    
    // Last processed physical drive number
    int m_lastDriveNumber;
};

#endif // DISKWRITER_HELPER_H 