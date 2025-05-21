#ifndef ELEVATIONHELPER_H
#define ELEVATIONHELPER_H

#include <QObject>
#include <QString>
#include <QLocalSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <windows.h>

// Common QDataStream version for all IPC communication
#define RPI_IMAGER_IPC_VERSION QDataStream::Qt_6_0

class ElevationHelper : public QObject
{
    Q_OBJECT
public:
    // Singleton instance getter
    static ElevationHelper* instance();
    
    // Helper connection states
    enum ConnectionState {
        Disconnected,       // Not connected to helper
        Connecting,         // Attempting to connect to helper
        HandshakeSending,   // Connected, sending handshake
        HandshakeReceiving, // Waiting for handshake response
        Connected,          // Fully connected and handshaked
        Error               // Connection error state
    };
    
    // Return current connection state
    ConnectionState connectionState() const { return m_connectionState; }
    
    // Public API for operations
    bool runWriteToDrive(const QString &drive, const QString &sourceFile);
    bool runFormatDrive(const QString &drive);
    bool runCustomizeImage(const QString &drive, const QByteArray &config, 
                                  const QByteArray &cmdline, const QByteArray &firstrun,
                                  const QByteArray &cloudinit, const QByteArray &cloudInitNetwork,
                                  const QByteArray &initFormat);
    bool runVerifyImage(const QString &drive, const QString &sourceFile, const QByteArray &expectedHash);
    
    // Legacy method - uses the older one-shot helper approach
    bool runHelperWithArgs(const QString &args);
    
    // Shutdown the helper process
    void shutdownHelper();
    
signals:
    void downloadProgress(qint64 bytesReceived, qint64 bytesTotal);
    void verifyProgress(qint64 bytesReceived, qint64 bytesTotal);
    void writeProgress(qint64 bytesReceived, qint64 bytesTotal);
    void stateChanged(ConnectionState newState); 
    void error(const QString &errorMsg);

private slots:
    void socketReadyRead();
    void handleSocketError(QLocalSocket::LocalSocketError socketError);
    void processPendingEvents();
    
private:
    // Private constructor - use instance() instead
    explicit ElevationHelper(QObject *parent = nullptr);
    ~ElevationHelper();
    
    // State machine helpers
    void changeState(ConnectionState newState);
    QString stateToString(ConnectionState state) const;
    bool validateStateForOperation();
    
    // Helper methods
    bool ensureHelperRunning();
    bool sendCommand(const QString &command);
    QString generateSocketName();
    void disconnectAndCleanup();
    QString getLastErrorAsString(DWORD errorCode = 0);
    bool processNextMessage();
    
    // Single instance
    static ElevationHelper* s_instance;
    
    // Socket and process handling
    QLocalSocket* m_socket;
    QTimer m_eventTimer;
    HANDLE m_helperProcess;
    QString m_socketName;
    
    // State tracking
    ConnectionState m_connectionState;
    bool m_operationComplete;
    QList<QByteArray> m_messageQueue;
    
    // For timeout tracking
    QElapsedTimer m_operationTimer;
    int m_operationTimeoutMs;
};

#endif // ELEVATIONHELPER_H 