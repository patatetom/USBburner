#include <QDebug>
#include <QDataStream>
#include <QByteArrayView>
#include <regex>
// Include winsock2.h before windows.h to avoid warnings
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <QTimer>
#include <QFile>
#include <QProcess>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QUuid>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QThread>
#include <QElapsedTimer>
#include <QRegularExpression>

#include "diskwriter_helper.h"
#include "../downloadthread.h"
#include "../driveformatthread.h"
#include "../devicewrapper.h"
#include "../devicewrapperfatpartition.h"
#include "dependencies/drivelist/src/drivelist.hpp"

DiskWriterHelper::DiskWriterHelper(QObject *parent) : QObject(parent),
    m_server(nullptr), m_clientConnection(nullptr), m_bytesTotal(0), m_bytesWritten(0),
    m_daemonMode(false), m_verifyHash(QCryptographicHash::Sha256),
    m_connectionState(Idle), m_operationTimeoutMs(300000), // 5 minutes default
    m_currentOperationSuccess(false)
{
    m_server = new QLocalServer(this);
    
    // Make sure the server permits everyone to connect
    m_server->setSocketOptions(QLocalServer::WorldAccessOption);
    
    connect(m_server, &QLocalServer::newConnection, this, &DiskWriterHelper::onNewConnection);
    connect(&m_progressTimer, &QTimer::timeout, this, &DiskWriterHelper::progressTimeout);
    
    qDebug() << "DiskWriterHelper constructed in state:" << stateToString(m_connectionState);
}

DiskWriterHelper::~DiskWriterHelper()
{
    if (m_clientConnection) {
        m_clientConnection->close();
    }
    
    if (m_server) {
        m_server->close();
    }
    
    qDebug() << "DiskWriterHelper destroyed";
}

void DiskWriterHelper::changeState(ConnectionState newState)
{
    if (m_connectionState == newState) {
        return; // No change
    }
    
    qDebug() << "DiskWriterHelper state transition:" << stateToString(m_connectionState) 
             << "->" << stateToString(newState);
    
    ConnectionState oldState = m_connectionState;
    m_connectionState = newState;
    
    // Handle state-specific actions
    switch (newState) {
        case Idle:
            m_currentCommand.clear();
            m_currentOperationSuccess = false;
            m_progressTimer.stop();
            // Ready to accept connections
            break;
            
        case Connected:
            // Client connected, prepare for handshake
            m_currentCommand.clear();
            m_currentOperationSuccess = false;
            break;
            
        case HandshakeSending:
            // Sending handshake, no special action
            break;
            
        case HandshakeReceiving:
            // Waiting for handshake response
            m_operationTimer.start(); // For timeout tracking
            break;
            
        case Ready:
            // Handshake complete, ready for commands
            m_currentCommand.clear();
            m_currentOperationSuccess = false;
            break;
            
        case Processing:
            // Currently processing a command
            m_operationTimer.start(); // For timeout tracking
            break;
            
        case Error:
            // Error state - try to clean up
            m_progressTimer.stop();
            if (m_clientConnection && m_clientConnection->isOpen()) {
                m_clientConnection->close();
            }
            qDebug() << "Entered Error state, client connection closed";
            break;
    }
}

QString DiskWriterHelper::stateToString(ConnectionState state) const
{
    switch (state) {
        case Idle: return "Idle";
        case Connected: return "Connected";
        case HandshakeSending: return "HandshakeSending";
        case HandshakeReceiving: return "HandshakeReceiving";
        case Ready: return "Ready";
        case Processing: return "Processing";
        case Error: return "Error";
        default: return "Unknown";
    }
}

bool DiskWriterHelper::validateStateForCommand()
{
    if (m_connectionState != Ready) {
        qDebug() << "Command attempted in invalid state:" << stateToString(m_connectionState);
        
        // If in error state, try to reset
        if (m_connectionState == Error) {
            resetState();
            return false;
        }
        
        // If in a transitional state, check for timeout
        if (m_connectionState == HandshakeSending || m_connectionState == HandshakeReceiving) {
            if (m_operationTimer.isValid() && m_operationTimer.elapsed() > 10000) {
                qDebug() << "Timeout in transitional state, resetting";
                resetState();
            }
        }
        
        return false;
    }
    
    return true;
}

void DiskWriterHelper::resetState()
{
    qDebug() << "Resetting state machine";
    
    // Close client connection if open
    if (m_clientConnection) {
        disconnect(m_clientConnection, &QLocalSocket::readyRead, this, &DiskWriterHelper::onClientDataReceived);
        disconnect(m_clientConnection, &QLocalSocket::disconnected, this, &DiskWriterHelper::onClientDisconnected);
        
        if (m_clientConnection->isOpen()) {
            m_clientConnection->close();
        }
        
        m_clientConnection = nullptr;
    }
    
    // Stop timers
    m_progressTimer.stop();
    
    // Clear operation state
    m_currentCommand.clear();
    m_currentOperationSuccess = false;
    
    // Return to idle state
    changeState(Idle);
}

void DiskWriterHelper::onClientDisconnected()
{
    qDebug() << "Client disconnected from helper";
    
    // Cleanup and reset state
    resetState();
    
    // If we're in daemon mode and the client disconnects, we should exit
    // This prevents orphaned helper processes when the main application is closed unexpectedly
    if (m_daemonMode) {
        qDebug() << "Client disconnected while in daemon mode - shutting down helper application immediately";
        // No delay needed - a new instance will be launched when required
        QCoreApplication::quit();
    }
}

void DiskWriterHelper::onNewConnection()
{
    qDebug() << "DiskWriterHelper::onNewConnection - New client connection received";
    
    if (m_clientConnection) {
        qDebug() << "Disconnecting previous client connection";
        disconnect(m_clientConnection, &QLocalSocket::readyRead, this, &DiskWriterHelper::onClientDataReceived);
        disconnect(m_clientConnection, &QLocalSocket::disconnected, this, &DiskWriterHelper::onClientDisconnected);
        m_clientConnection->close();
    }
    
    m_clientConnection = m_server->nextPendingConnection();
    connect(m_clientConnection, &QLocalSocket::readyRead, this, &DiskWriterHelper::onClientDataReceived);
    connect(m_clientConnection, &QLocalSocket::disconnected, this, &DiskWriterHelper::onClientDisconnected);
    
    qDebug() << "Client connected to helper server";
    
    // Update state
    changeState(Connected);
    
    // Wait a moment to ensure the connection is fully established
    QThread::msleep(200);
    
    // Send handshake
    changeState(HandshakeSending);
    
    // Prepare the HELLO message in QDataStream format
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setVersion(RPI_IMAGER_IPC_VERSION);
    out << QString("HELLO");
    
    QByteArray rawHex = block.toHex();
    qDebug() << "Sending HELLO handshake, size:" << block.size() << "bytes, raw data:" << rawHex;
    
    // Send the handshake message
    qint64 bytesWritten = m_clientConnection->write(block);
    bool flushed = m_clientConnection->flush();
    
    qDebug() << "Handshake sent:" << bytesWritten << "bytes, flush:" << flushed;
    
    // Consider it successful if bytes were written, even if flush fails
    if (bytesWritten > 0) {
        // Wait for the data to be transmitted with a longer timeout
        if (!m_clientConnection->waitForBytesWritten(2000)) {
            qDebug() << "Timeout waiting for handshake to be sent, but bytes were written - continuing anyway";
        }
        
        // Successfully sent handshake, now wait for response
        changeState(HandshakeReceiving);
        
        // Set a longer timeout for receiving the handshake response
        QTimer::singleShot(5000, this, [this]() {
            if (m_connectionState == HandshakeReceiving) {
                qDebug() << "Handshake response timeout - no response received within 5 seconds";
                changeState(Error);
            }
        });
    } else {
        qCritical() << "Failed to send handshake - no bytes written";
        changeState(Error);
        return;
    }
}

void DiskWriterHelper::progressTimeout()
{
    // Send progress updates periodically
    onProgressChanged(m_bytesWritten, m_bytesTotal);
}

void DiskWriterHelper::onClientDataReceived()
{
    qDebug() << "DiskWriterHelper::onClientDataReceived - Data received from client, bytes available:" 
             << m_clientConnection->bytesAvailable() << "in state:" << stateToString(m_connectionState);
    
    // Debug: inspect raw data
    QByteArray rawData = m_clientConnection->peek(qMin(m_clientConnection->bytesAvailable(), (qint64)100));
    qDebug() << "Raw data received (first 100 bytes, hex):" << rawData.left(100).toHex();
    qDebug() << "Raw data as string:" << QString::fromUtf8(rawData);
    
    // Handle data based on current state
    switch (m_connectionState) {
        case HandshakeReceiving:
            // Expected READY response to our HELLO message
            {
                // Use QDataStream to read the response
                QDataStream in(m_clientConnection);
                in.setVersion(RPI_IMAGER_IPC_VERSION);
                
                QString response;
                in >> response;
                
                if (in.status() == QDataStream::Ok) {
                    qDebug() << "Received handshake response:" << response;
                    
                    if (response == "READY") {
                        qDebug() << "Received READY response, handshake complete";
                        changeState(Ready);
                    } else {
                        qCritical() << "Unexpected handshake response:" << response;
                        changeState(Error);
                    }
                } else {
                    qCritical() << "Failed to read handshake response, status:" << in.status();
                    changeState(Error);
                }
            }
            break;
            
        case Ready:
            // Ready for commands - use QDataStream to read commands
            {
                QDataStream in(m_clientConnection);
                in.setVersion(RPI_IMAGER_IPC_VERSION);
                
                QString command;
                in >> command;
                
                QDataStream::Status afterReadStatus = in.status();
                qDebug() << "Stream status after reading command:" << afterReadStatus;
                
                if (afterReadStatus == QDataStream::Ok) {
                    // Successfully read command with QDataStream
                    qDebug() << "Received command:" << command << "length:" << command.length();
                    
                    // Special case for READY response to handshake
                    if (command == "READY" && m_connectionState == HandshakeReceiving) {
                        qDebug() << "Received READY response, handshake complete";
                        changeState(Ready);
                        return;
                    }
                    
                    // Normal command processing - validate state first
                    if (!validateStateForCommand()) {
                        qCritical() << "Cannot process command in current state:" << stateToString(m_connectionState);
                        sendCompletionStatus("FAILURE");
                        return;
                    }
                    
                    // Store current command and go to processing state
                    m_currentCommand = command;
                    changeState(Processing);
                    
                    // Process the command
                    qDebug() << "Processing command...";
                    
                    try {
                        m_currentOperationSuccess = processCommand(command);
                        qDebug() << "Command processed with result:" << (m_currentOperationSuccess ? "SUCCESS" : "FAILURE");
                        
                        // Send completion status
                        sendCompletionStatus(m_currentOperationSuccess ? "SUCCESS" : "FAILURE");
                        
                        // Return to ready state
                        changeState(Ready);
                    } catch (const std::exception& e) {
                        qCritical() << "Exception during command processing:" << e.what();
                        
                        // Send failure status
                        sendCompletionStatus("FAILURE");
                        
                        // Go to error state
                        changeState(Error);
                    }
                } else {
                    qCritical() << "Failed to read command: " << afterReadStatus;
                    
                    // If we don't have enough data yet, just return and wait for more
                    if (afterReadStatus == QDataStream::ReadPastEnd) {
                        qDebug() << "Partial command received, waiting for more data";
                    }
                }
            }
            break;
            
        case Processing:
            // We received data while processing a command - this could be a cancellation
            // or additional parameters
            qDebug() << "Received data while processing command - reading and storing for later";
            m_clientConnection->readAll(); // Just store it for now
            break;
            
        case Error:
        case Idle:
        case Connected:
        case HandshakeSending:
            // Unexpected data in these states
            qDebug() << "Unexpected data received in state" << stateToString(m_connectionState) << ", ignoring";
            m_clientConnection->readAll(); // Consume data
            break;
    }
}

void DiskWriterHelper::sendCompletionStatus(const QString &status)
{
    qDebug() << "Sending completion status:" << status << "in state:" << stateToString(m_connectionState);
    
    // Only send completion in Processing state
    if (m_connectionState != Processing) {
        qWarning() << "Attempting to send completion status in invalid state:" << stateToString(m_connectionState);
        // Continue anyway since we want to let the client know
    }
    
    // Send completion status
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setVersion(RPI_IMAGER_IPC_VERSION);
    
    // Command completion status
    out << status;
    
    qDebug() << "Prepared command completion status:" << status << "size:" << block.size() << "bytes";
    
    // Ensure the socket is still open 
    if (!m_clientConnection || !m_clientConnection->isOpen()) {
        qCritical() << "Cannot send command status - client disconnected";
        changeState(Error);
        return;
    }
    
    qint64 bytesWritten = m_clientConnection->write(block);
    bool flushed = m_clientConnection->flush();
    qDebug() << "Command completion status sent: status=" << status 
            << "bytes written=" << bytesWritten 
            << "of" << block.size() 
            << "flush success=" << flushed;
    
    if (bytesWritten <= 0) {
        qCritical() << "Failed to send command status. Error:" << m_clientConnection->errorString();
        changeState(Error);
    } else if (bytesWritten != block.size()) {
        qCritical() << "Only partial command status sent:" << bytesWritten << "of" << block.size() << "bytes";
        changeState(Error);
    } else {
        // Bytes were written successfully, but flush may have failed
        if (!flushed) {
            qWarning() << "Flush failed when sending completion status, but bytes were written";
        }
        
        // Wait for the data to be actually written
        if (!m_clientConnection->waitForBytesWritten(1000)) {
            qWarning() << "Timeout waiting for completion status to be sent - continuing anyway";
        } else {
            qDebug() << "Command completion status successfully delivered";
        }
        // State transition will be handled by the caller
    }
}

bool DiskWriterHelper::processCommand(const QString &command)
{
    qDebug() << "DiskWriterHelper::processCommand - Processing command:" << command 
             << "in state:" << stateToString(m_connectionState)
             << "length:" << command.length()
             << "hex:" << command.toLatin1().toHex();
    
    // We should only be processing commands in the Processing state
    if (m_connectionState != Processing) {
        qWarning() << "Processing command in unexpected state:" << stateToString(m_connectionState);
        // Continue anyway since we're already here
    }
    
    if (command.startsWith("FORMAT ")) {
        QString drive = command.mid(7);
        // Remove quotes if present
        if (drive.startsWith("\"") && drive.endsWith("\"")) {
            drive = drive.mid(1, drive.length() - 2);
        }
        qDebug() << "Executing FORMAT command for drive:" << drive;
        return formatDrive(drive);
    }
    else if (command.startsWith("WRITE ")) {
        qDebug() << "Detected WRITE command, command data:" << command.mid(6);
        
        // More robust parsing for the WRITE command
        QString cmdWithoutPrefix = command.mid(6);
        qDebug() << "Parsing WRITE command arguments:" << cmdWithoutPrefix;
        
        // Try to extract device and source directly from quoted strings
        QRegularExpression regex("\"([^\"]*)\"\\s+\"([^\"]*)\"");
        QRegularExpressionMatch match = regex.match(cmdWithoutPrefix);
        
        if (match.hasMatch()) {
            QString drive = match.captured(1);
            QString source = match.captured(2);
            
            qDebug() << "Regex parsed WRITE command - drive:" << drive << "source:" << source;
            return writeImageToDevice(source, drive);
        }
        
        // If regex fails, try the manual parsing approach
        QStringList parts;
        QString current;
        bool inQuote = false;
        bool escapeNext = false;
        
        // Parse the command respecting quotes and escape characters
        for (int i = 0; i < cmdWithoutPrefix.length(); i++) {
            QChar c = cmdWithoutPrefix[i];
            
            if (escapeNext) {
                current.append(c);
                escapeNext = false;
            } else if (c == '\\') {
                escapeNext = true;
            } else if (c == '"') {
                // Toggle quote state
                inQuote = !inQuote;
                
                // If we're exiting a quote and have content, add it to parts
                if (!inQuote && !current.isEmpty()) {
                    parts.append(current);
                    current.clear();
                } else if (inQuote && parts.isEmpty()) {
                    // Starting the first quoted section
                    current.clear();
                }
            } else if (inQuote) {
                // Within quotes, append character to current token
                current.append(c);
            } else if (c == ' ' && !current.isEmpty()) {
                // Space outside quotes with content - finish current token
                parts.append(current);
                current.clear();
            } else if (c != ' ' || !current.isEmpty()) {
                // Regular character
                current.append(c);
            }
        }
        
        // Add any remaining content
        if (!current.isEmpty()) {
            parts.append(current);
        }
        
        qDebug() << "Parsed WRITE command into" << parts.size() << "parts:" << parts;
        
        if (parts.size() == 2) {
            QString drive = parts[0];
            QString source = parts[1];
            
            qDebug() << "Executing WRITE command for drive:" << drive << "from source:" << source;
            return writeImageToDevice(source, drive);
        }
        else {
            qCritical() << "Invalid WRITE command format, expected 2 parts but got" << parts.size();
            return false;
        }
    }
    else if (command.startsWith("CUSTOMIZE ")) {
        QString cmdWithoutPrefix = command.mid(10);
        QStringList parts;
        QString current;
        bool inQuote = false;
        
        // Parse the command respecting quotes
        for (int i = 0; i < cmdWithoutPrefix.length(); i++) {
            QChar c = cmdWithoutPrefix[i];
            if (c == '"') {
                inQuote = !inQuote;
                if (!inQuote) {
                    parts.append(current);
                    current.clear();
                }
            } else if (inQuote) {
                current.append(c);
            }
        }
        
        if (parts.size() == 7) {
            QString drive = parts[0];
            QByteArray config = QByteArray::fromBase64(parts[1].toLatin1());
            QByteArray cmdline = QByteArray::fromBase64(parts[2].toLatin1());
            QByteArray firstrun = QByteArray::fromBase64(parts[3].toLatin1());
            QByteArray cloudinit = QByteArray::fromBase64(parts[4].toLatin1());
            QByteArray cloudInitNetwork = QByteArray::fromBase64(parts[5].toLatin1());
            QByteArray initFormat = QByteArray::fromBase64(parts[6].toLatin1());
            
            qDebug() << "Executing CUSTOMIZE command for drive:" << drive;
            return customizeImage(drive, config, cmdline, firstrun, cloudinit, cloudInitNetwork, initFormat);
        }
        else {
            qCritical() << "Invalid CUSTOMIZE command format - expected 7 parameters, got" << parts.size();
            return false;
        }
    }
    else if (command.startsWith("VERIFY ")) {
        QString cmdWithoutPrefix = command.mid(7);
        QStringList parts;
        QString current;
        bool inQuote = false;
        
        // Parse the command respecting quotes
        for (int i = 0; i < cmdWithoutPrefix.length(); i++) {
            QChar c = cmdWithoutPrefix[i];
            if (c == '"') {
                inQuote = !inQuote;
                if (!inQuote) {
                    parts.append(current);
                    current.clear();
                }
            } else if (inQuote) {
                current.append(c);
            }
        }
        
        if (parts.size() == 3) {
            QString drive = parts[0];
            QString sourceFile = parts[1];
            QByteArray expectedHash = QByteArray::fromBase64(parts[2].toLatin1());
            
            qDebug() << "Executing VERIFY command for drive:" << drive << "against source:" << sourceFile;
            return verifyImage(drive, sourceFile, expectedHash);
        }
        else {
            qCritical() << "Invalid VERIFY command format - expected 3 parameters, got" << parts.size();
            return false;
        }
    }
    else if (command == "SHUTDOWN") {
        qDebug() << "Shutdown command received, terminating helper";
        QCoreApplication::quit();
        return true;
    }
    else {
        qDebug() << "Unknown command:" << command;
        return false;
    }
}

void DiskWriterHelper::sendProgressUpdate(int progressType, qint64 now, qint64 total)
{
    if (m_clientConnection && m_clientConnection->isOpen()) {
        static qint64 lastSentNow = -1;
        static int lastSentType = -1;
        
        // Don't send duplicate progress updates for the same value
        if (lastSentNow == now && lastSentType == progressType) {
            return;
        }
        
        lastSentNow = now;
        lastSentType = progressType;
        
        // Create a proper QDataStream for communication
        QByteArray block;
        QDataStream out(&block, QIODevice::WriteOnly);
        out.setVersion(RPI_IMAGER_IPC_VERSION);
        
        // Write progress type and values
        out << progressType << now << total;
        
        qDebug() << "Sending progress update: type=" << progressType 
                 << "now=" << now << "total=" << total
                 << "bytes=" << block.size();
        
        // Write data with proper flow control
        qint64 bytesWritten = m_clientConnection->write(block);
        bool flushSuccess = m_clientConnection->flush();
        
        if (bytesWritten != block.size()) {
            qWarning() << "Progress update not fully written:" << bytesWritten << "of" << block.size() << "bytes";
        } else {
            // Wait for bytes to be actually written with sufficient timeout
            if (!m_clientConnection->waitForBytesWritten(500)) {
                qWarning() << "Timeout waiting for progress update to be sent, but bytes were written";
            } else {
                qDebug() << "Progress update successfully written and flushed:" << flushSuccess;
            }
        }
    } else {
        qWarning() << "Cannot send progress update - client connection not available or closed";
    }
}

void DiskWriterHelper::onProgressChanged(qint64 now, qint64 total)
{
    sendProgressUpdate(WriteProgress, now, total);
}

void DiskWriterHelper::onDownloadProgress(qint64 now, qint64 total)
{
    sendProgressUpdate(DownloadProgress, now, total);
}

void DiskWriterHelper::onVerifyProgress(qint64 now, qint64 total)
{
    sendProgressUpdate(VerifyProgress, now, total);
}

int DiskWriterHelper::executeFromCommandLine(const QStringList &args)
{
    QCommandLineParser parser;
    parser.setApplicationDescription("Raspberry Pi Imager Disk Writer Helper");
    parser.addHelpOption();
    
    // Check if we're running with admin privileges
    if (!isRunningAsAdmin()) {
        qCritical() << "WARNING: Helper application is NOT running with administrator privileges!";
        qCritical() << "         This will likely cause disk write operations to fail.";
        qCritical() << "         Please ensure the manifest is properly embedded in the executable.";
    } else {
        qDebug() << "Helper application running with administrator privileges";
    }
    
    // Setup socket server if socket name provided
    constexpr auto formatOption = "format";
    constexpr auto writeOption = "write";
    constexpr auto sourceOption = "source";
    constexpr auto socketOption = "socket";
    constexpr auto daemonOption = "daemon";

    const QList<QCommandLineOption> options{
        {QStringList{"f", formatOption}, "Format the drive", "drive"},
        {QStringList{"w", writeOption}, "Write image to drive", "drive"},
        {QStringList{"s", sourceOption}, "Source image file", "file"},
        {QStringList{socketOption}, "Socket name for progress reporting", "name"},
        {QStringList{daemonOption}, "Run in daemon mode, listening for commands"}
    };

    parser.addOptions(options);
    
    parser.process(args);
    
    // Setup socket server if socket name provided
    // Set default socket name
    m_socketName = "rpihelperlocalsocket";
    
    // Override with user-provided socket name if specified
    if (parser.isSet(socketOption) && !parser.value(socketOption).isEmpty()) {
        m_socketName = parser.value(socketOption);
        qDebug() << "Using custom socket name:" << m_socketName;
    } else {
        qDebug() << "Using default socket name:" << m_socketName;
    }
    
    // Remove any existing server with this name
    QLocalServer::removeServer(m_socketName);
    
    // Make sure the server permits everyone to connect
    m_server->setSocketOptions(QLocalServer::WorldAccessOption);
    
    if (!m_server->listen(m_socketName)) {
        qCritical() << "Could not start local server:" << m_server->errorString();
        return 1;
    } else {
        qDebug() << "Started server with socket name:" << m_socketName;
    }
    
    if (parser.isSet(daemonOption)) {
        if (m_socketName.isEmpty()) {
            qCritical() << "Socket name must be provided in daemon mode";
            return 2;
        }
        
        return startDaemonMode(m_socketName);
    }
    else if (parser.isSet(formatOption)) {
        QString drive = parser.value(formatOption);
        return formatDrive(drive) ? 0 : 1;
    }
    else if (parser.isSet(writeOption)) {
        QString drive = parser.value(writeOption);
        if (!parser.isSet(sourceOption)) {
            qCritical() << "Source file must be specified for write operation";
            return 2;
        }
        QString sourceFile = parser.value(sourceOption);
        return writeToDrive(drive, sourceFile) ? 0 : 1;
    }
    
    // No valid operation specified
    parser.showHelp(3);
    return 3;
}

int DiskWriterHelper::startDaemonMode(const QString &socketName)
{
    // We've already configured the server in executeFromCommandLine
    qDebug() << "Starting in daemon mode with socket:" << m_socketName;
    
    // Create a signal file to indicate we're running
    QString signalFilePath = QDir::homePath() + "/Documents/rpi-imager-helper-running.txt";
    QFile signalFile(signalFilePath);
    if (signalFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        signalFile.write("Helper application is running\n");
        signalFile.write(QString("Socket name: %1\n").arg(m_socketName).toUtf8());
        signalFile.write(QString("Process ID: %1\n").arg(QCoreApplication::applicationPid()).toUtf8());
        signalFile.write(QString("Timestamp: %1\n").arg(QDateTime::currentDateTime().toString()).toUtf8());
        signalFile.flush();
        signalFile.close();
        qDebug() << "Created signal file at:" << signalFilePath;
    } else {
        qCritical() << "Failed to create signal file at:" << signalFilePath;
        qCritical() << "Error:" << signalFile.errorString();
    }
    
    // Print server information
    qDebug() << "Server created at: Name=" << m_server->serverName() 
           << ", FullServerName=" << m_server->fullServerName()
           << ", MaxPendingConnections=" << m_server->maxPendingConnections();
    
    // On Windows, named pipes don't create physical files in the filesystem
    // They exist in the NT namespace as \\.\pipe\<name>
    QString pipePath = QString("\\\\.\\pipe\\%1").arg(m_socketName);
    qDebug() << "Using named pipe:" << pipePath;
    
    m_daemonMode = true;
    
    qDebug() << "Daemon mode started successfully, application will remain running until shutdown command is received";
    
    // Return success but don't exit - event loop will keep running
    return 0;
}

bool DiskWriterHelper::formatDrive(const QString &drive)
{
    QByteArray devicePath = drive.toLatin1();
    
    // Check if this is a physical drive (\\.\PHYSICALDRIVEx)
    std::regex windriveregex("\\\\\\\\.\\\\PHYSICALDRIVE([0-9]+)", std::regex_constants::icase);
    std::cmatch m;

    if (std::regex_match(devicePath.constData(), m, windriveregex))
    {
        QByteArray nr = QByteArray::fromStdString(m[1]);
        
        qDebug() << "Helper formatting Windows drive #" << nr << "(" << devicePath << ")";
        
        // Use diskpart to clean and partition the drive
        QProcess proc;
        QByteArray diskpartCmds =
                "select disk "+nr+"\r\n"
                "clean\r\n";
        
        proc.start("diskpart", QStringList());
        proc.waitForStarted();
        proc.write(diskpartCmds);
        proc.closeWriteChannel();
        proc.waitForFinished();
        
        QByteArray output = proc.readAllStandardError();
        qDebug() << output;
        qDebug() << "Done running diskpart. Exit status code =" << proc.exitCode();
        
        if (proc.exitCode())
        {
            qCritical() << "Error partitioning: " << QString(output);
            return false;
        }
        
        // Now find the drive letter that was assigned
        auto l = Drivelist::ListStorageDevices();
        QByteArray devlower = devicePath.toLower();
        for (auto i : l)
        {
            if (QByteArray::fromStdString(i.device).toLower() == devlower && i.mountpoints.size() == 1)
            {
                QByteArray driveLetter = QByteArray::fromStdString(i.mountpoints.front());
                if (driveLetter.endsWith("\\"))
                    driveLetter.chop(1);
                
                qDebug() << "Found drive letter for device:" << driveLetter;
                return true;
            }
        }
        
        qWarning() << "Error: Could not determine drive letter for physical device:" << devicePath;
        return true;
    }
    else
    {
        // Direct volume letter format (like "E:")
        return runFat32Format(devicePath);
    }
}

bool DiskWriterHelper::runFat32Format(const QString &driveLetter)
{
    qDebug() << "Helper running fat32format on drive:" << driveLetter;
    
    // Verify we're running with admin rights
    if (!isRunningAsAdmin())
    {
        qCritical() << "ERROR: Helper not running with admin privileges, cannot format drive";
        return false;
    }
    
    // Try to find fat32format.exe in multiple possible locations
    QStringList searchPaths;
    searchPaths << QCoreApplication::applicationDirPath() + "/fat32format.exe"
               << QCoreApplication::applicationDirPath() + "/../fat32format.exe"
               << QCoreApplication::applicationDirPath() + "/../dependencies/fat32format/fat32format.exe"
               << QCoreApplication::applicationDirPath() + "/../../dependencies/fat32format/fat32format.exe"
               << QCoreApplication::applicationDirPath() + "/../../build/dependencies/fat32format/fat32format.exe"
               << QCoreApplication::applicationDirPath() + "/../../build/deploy/fat32format.exe";
    
    QString fat32formatPath;
    for (const QString &path : searchPaths) {
        if (QFile::exists(path)) {
            fat32formatPath = path;
            break;
        }
    }
    
    if (fat32formatPath.isEmpty()) {
        qCritical() << "Could not find fat32format.exe in any of the following locations:";
        for (const QString &path : searchPaths) {
            qCritical() << "  -" << path;
        }
        return false;
    }
    
    qDebug() << "Found fat32format at:" << fat32formatPath;
    
    // Execute fat32format directly using QProcess
    QProcess f32format;
    QStringList args;
    args << "-y" << driveLetter;
    
    qDebug() << "Running fat32format with args:" << args.join(" ");
    
    f32format.start(fat32formatPath, args);
    if (!f32format.waitForStarted())
    {
        qCritical() << "Error starting fat32format process";
        return false;
    }
    
    // Wait for the process to complete
    f32format.waitForFinished(120000); // Wait up to 2 minutes
    
    if (f32format.exitStatus() != QProcess::NormalExit || f32format.exitCode() != 0)
    {
        QByteArray output = f32format.readAllStandardOutput();
        QByteArray error = f32format.readAllStandardError();
        qCritical() << "Error running fat32format. Exit code:" << f32format.exitCode();
        qCritical() << "Output:" << output;
        qCritical() << "Error:" << error;
        return false;
    }
    
    qDebug() << "fat32format completed successfully";
    
    return true;
}

bool DiskWriterHelper::writeToDrive(const QString &drive, const QString &sourceFile)
{
    // Use our own custom implementation for writing directly to the device
    bool result = writeImageToDevice(sourceFile, drive);
    
    return result;
}

bool DiskWriterHelper::writeImageToDevice(const QString &sourceFile, const QString &devicePath)
{
    qDebug() << "Helper: Opening source file:" << sourceFile;
    
    QFile sourceFileObj(sourceFile);
    if (!sourceFileObj.open(QIODevice::ReadOnly)) {
        qCritical() << "Failed to open source file:" << sourceFileObj.errorString();
        return false;
    }
    
    qDebug() << "Helper: Opening device for writing:" << devicePath;
    
    // Properly format device path for Windows API
    QString fixedDevicePath = devicePath;
    // If we get a path that has extra backslashes, normalize it
    fixedDevicePath.replace("\\\\\\\\", "\\\\");
    fixedDevicePath.replace("\\\\\\.", "\\\\."); 
    
    qDebug() << "Normalized device path:" << fixedDevicePath;
    
    // Declare the device handle at the outer scope
    HANDLE deviceHandle = INVALID_HANDLE_VALUE;
    
    // If this is a physical drive, attempt to lock it and dismount volumes
    // Need variable scope for the whole function since we use it in the cleanup section
    bool isPhysicalDrive = fixedDevicePath.contains("PhysicalDrive", Qt::CaseInsensitive);
    if (isPhysicalDrive) {
        qDebug() << "Physical drive detected, using fat32format-style direct access";
        
        // Extract drive number from physical drive path
        m_lastDriveNumber = -1; // Initialize with invalid value
        QRegularExpression numRx("PhysicalDrive(\\d+)", QRegularExpression::CaseInsensitiveOption);
        auto match = numRx.match(fixedDevicePath);
        if (match.hasMatch()) {
            m_lastDriveNumber = match.captured(1).toInt();
            qDebug() << "Physical drive number:" << m_lastDriveNumber;
            
            // Check for and dismount all volumes on this disk before attempting write
            QProcess diskpart;
            
            // First list all volumes on this disk to identify what needs to be dismounted
            QByteArray listScript = QString("select disk %1\r\nlist volume\r\n").arg(m_lastDriveNumber).toLatin1();
            qDebug() << "Running diskpart to list volumes:" << listScript;
            
            diskpart.start("diskpart", QStringList());
            if (diskpart.waitForStarted(5000)) {
                diskpart.write(listScript);
                diskpart.closeWriteChannel();
                diskpart.waitForFinished(30000);
                QByteArray output = diskpart.readAllStandardOutput();
                QByteArray error = diskpart.readAllStandardError();
                qDebug() << "Diskpart list volumes exit code:" << diskpart.exitCode();
                qDebug() << "Diskpart output:" << output;
                qDebug() << "Diskpart error:" << error;
                
                // Now take the disk offline to prevent system from accessing it
                diskpart.close();
                QByteArray offlineScript = QString("select disk %1\r\noffline disk\r\nattributes disk clear readonly\r\n").arg(m_lastDriveNumber).toLatin1();
                qDebug() << "Running diskpart to offline then online disk:" << offlineScript;
                
                diskpart.start("diskpart", QStringList());
                if (diskpart.waitForStarted(5000)) {
                    diskpart.write(offlineScript);
                    diskpart.closeWriteChannel();
                    diskpart.waitForFinished(30000);
                    qDebug() << "Diskpart offline/online operation exit code:" << diskpart.exitCode();
                    qDebug() << "Diskpart output:" << diskpart.readAllStandardOutput();
                    qDebug() << "Diskpart error:" << diskpart.readAllStandardError();
                }
            }
            
            // Check if we need to clear existing partitions
            diskpart.close();
            QByteArray checkScript = QString("select disk %1\r\nlist partition\r\n").arg(m_lastDriveNumber).toLatin1();
            qDebug() << "Running diskpart to check existing partitions:" << checkScript;
            
            diskpart.start("diskpart", QStringList());
            if (diskpart.waitForStarted(5000)) {
                diskpart.write(checkScript);
                diskpart.closeWriteChannel();
                diskpart.waitForFinished(30000);
                QByteArray output = diskpart.readAllStandardOutput();
                QByteArray error = diskpart.readAllStandardError();
                qDebug() << "Diskpart check partitions exit code:" << diskpart.exitCode();
                qDebug() << "Diskpart output:" << output;
                qDebug() << "Diskpart error:" << error;
                
                if (output.contains("Partition")) {
                    // Clean the disk if it has existing partitions
                    diskpart.close();
                    QByteArray cleanScript = QString("select disk %1\r\nclean\r\n").arg(m_lastDriveNumber).toLatin1();
                    qDebug() << "Found existing partitions, running diskpart to clean disk:" << cleanScript;
                    
                    diskpart.start("diskpart", QStringList());
                    if (diskpart.waitForStarted(5000)) {
                        diskpart.write(cleanScript);
                        diskpart.closeWriteChannel();
                        diskpart.waitForFinished(30000);
                        qDebug() << "Diskpart clean exit code:" << diskpart.exitCode();
                        qDebug() << "Diskpart output:" << diskpart.readAllStandardOutput();
                        qDebug() << "Diskpart error:" << diskpart.readAllStandardError();
                    }
                    
                    // IMPORTANT: Do not create any partitions after cleaning!
                    // The image will provide its own partition table
                    qDebug() << "Disk cleaned successfully - NOT creating any partitions as image will provide its own";
                }
            }
            
            // DO NOT create any partitions - the image will provide its own
        }
        
        // Now open the device for raw access
        DWORD bytesReturned = 0;
        
        // Try with multiple access methods if necessary
        for (int attempt = 0; attempt < 3 && deviceHandle == INVALID_HANDLE_VALUE; attempt++) {
            DWORD shareMode = FILE_SHARE_READ | FILE_SHARE_WRITE; // Allow some sharing
            DWORD flags = FILE_FLAG_NO_BUFFERING;
            
            if (attempt == 1) {
                // Second attempt - try with different sharing mode
                shareMode = 0; // Exclusive
            } else if (attempt == 2) {
                // Third attempt - try with different flags
                flags = FILE_ATTRIBUTE_NORMAL;
            }
            
            qDebug() << "Opening device with attempt" << (attempt+1) 
                     << "shareMode:" << shareMode 
                     << "flags:" << flags;
            
            deviceHandle = CreateFileA(
                fixedDevicePath.toLatin1().constData(),
                GENERIC_READ | GENERIC_WRITE,
                shareMode,
                NULL,
                OPEN_EXISTING,
                flags,
                NULL
            );
            
            if (deviceHandle == INVALID_HANDLE_VALUE) {
                DWORD errorCode = GetLastError();
                qWarning() << "Failed to open device on attempt" << (attempt + 1) << "- Error code:" << errorCode;
                
                // Get error message
                LPVOID lpMsgBuf;
                FormatMessageA(
                    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (LPSTR)&lpMsgBuf, 0, NULL);
                qWarning() << "Error message:" << (char*)lpMsgBuf;
                LocalFree(lpMsgBuf);
                
                // Wait before retrying with different flags
                qDebug() << "Waiting before retry...";
                Sleep(2000);
            } else {
                qDebug() << "Successfully opened device on attempt" << (attempt + 1);
            }
        }
        
        if (deviceHandle == INVALID_HANDLE_VALUE) {
            DWORD errorCode = GetLastError();
            qCritical() << "Failed to open device for writing after multiple attempts. Error code:" << errorCode;
            
            // Get a human-readable error message
            LPVOID lpMsgBuf;
            DWORD msgLen = FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | 
                FORMAT_MESSAGE_FROM_SYSTEM |
                FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL,
                errorCode,
                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&lpMsgBuf,
                0, NULL );
                
            QString errorMessage;
            if (msgLen > 0) {
                errorMessage = QString("Error code %1: %2").arg(errorCode).arg(QString::fromUtf8((char*)lpMsgBuf));
                LocalFree(lpMsgBuf);
            } else {
                errorMessage = QString("Error code: %1").arg(errorCode);
            }
            
            qCritical() << "Detailed error:" << errorMessage;
            sourceFileObj.close();
            return false;
        }
        
        // Enable extended DASD I/O
        BOOL bRet = DeviceIoControl(deviceHandle, FSCTL_ALLOW_EXTENDED_DASD_IO, NULL, 0, NULL, 0, &bytesReturned, NULL);
        if (!bRet) {
            qWarning() << "Failed to allow extended DASD on device, error:" << GetLastError() << " (continuing anyway)";
        }
        
        // Lock the volume
        qDebug() << "Locking the volume...";
        bRet = DeviceIoControl(deviceHandle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL);
        if (!bRet) {
            DWORD errorCode = GetLastError();
            qWarning() << "Failed to lock the volume, error:" << errorCode << " (continuing anyway)";
            
            // Additional retry with delay if needed
            Sleep(2000);
            bRet = DeviceIoControl(deviceHandle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL);
            if (!bRet) {
                qWarning() << "Second attempt to lock volume failed, error:" << GetLastError();
            } else {
                qDebug() << "Successfully locked volume on second attempt";
            }
        } else {
            qDebug() << "Successfully locked volume";
        }
        
        // Dismount the volume
        qDebug() << "Dismounting the volume...";
        bRet = DeviceIoControl(deviceHandle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL);
        if (!bRet) {
            qWarning() << "Failed to dismount the volume, error:" << GetLastError() << " (continuing anyway)";
        } else {
            qDebug() << "Successfully dismounted volume";
        }
        
        // Store the handle for writing
        qDebug() << "Using handle for physical drive operations";
    } else {
        // For regular files/volumes, open with exclusive access
        qDebug() << "Regular file/volume detected, opening with exclusive access";
        
        // Try multiple times with different flags if needed
        for (int attempt = 0; attempt < 3; attempt++) {
            DWORD shareMode = (attempt == 0) ? 0 : FILE_SHARE_READ; // Try exclusive first, then shared
            DWORD flags = (attempt == 0) ? 
                (FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH) : 
                (FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
                
            deviceHandle = CreateFileA(
                fixedDevicePath.toLatin1().constData(),
                GENERIC_READ | GENERIC_WRITE,
                shareMode,
                NULL,
                OPEN_EXISTING,
                flags,
                NULL
            );
            
            if (deviceHandle != INVALID_HANDLE_VALUE) {
                qDebug() << "Successfully opened device on attempt" << (attempt + 1);
                break;
            }
            
            DWORD errorCode = GetLastError();
            qWarning() << "Failed to open device on attempt" << (attempt + 1) << "- Error code:" << errorCode;
            
            // Wait before retrying with different flags
            if (attempt < 2) {
                qDebug() << "Waiting before retry with different access flags...";
                Sleep(1000);
            }
        }
    }
    
    if (deviceHandle == INVALID_HANDLE_VALUE) {
        DWORD errorCode = GetLastError();
        qCritical() << "Failed to open device for writing after multiple attempts. Error code:" << errorCode;
        
        // Get a human-readable error message
        LPVOID lpMsgBuf;
        DWORD msgLen = FormatMessageA(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | 
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
            NULL,
            errorCode,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPSTR)&lpMsgBuf,
            0, NULL );
            
        QString errorMessage;
        if (msgLen > 0) {
            errorMessage = QString("Error code %1: %2").arg(errorCode).arg(QString::fromUtf8((char*)lpMsgBuf));
            LocalFree(lpMsgBuf);
        } else {
            errorMessage = QString("Error code: %1").arg(errorCode);
        }
        
        qCritical() << "Detailed error:" << errorMessage;
        sourceFileObj.close();
        return false;
    }
    
    m_bytesTotal = sourceFileObj.size();
    m_bytesWritten = 0;
    
    // Reset hash for this operation
    m_verifyHash.reset();
    
    // Send initial progress update immediately
    sendProgressUpdate(WriteProgress, 0, m_bytesTotal);
    
    // Start progress timer with higher frequency for more responsive UI updates
    disconnect(&m_progressTimer, nullptr, this, nullptr); // Ensure no duplicate connections
    connect(&m_progressTimer, &QTimer::timeout, this, [this]() {
        // Send direct update each time
        sendProgressUpdate(WriteProgress, m_bytesWritten, m_bytesTotal);
    });
    
    // Send an immediate progress update before starting
    sendProgressUpdate(WriteProgress, 0, m_bytesTotal);
    
    // Use a faster timer for better UI responsiveness
    m_progressTimer.start(200);
    
    qDebug() << "Starting write operation with total size:" << m_bytesTotal << "bytes";
    
    // Buffer for reading/writing
    const int BUFFER_SIZE = 10 * 1024 * 1024; // 10MB buffer
    QByteArray buffer(BUFFER_SIZE, 0);
    
    // Use page size aligned buffer for direct I/O
    DWORD bytesPerSector;
    if (!GetDiskFreeSpace(NULL, NULL, &bytesPerSector, NULL, NULL)) {
        bytesPerSector = 4096; // Default to 4K if we can't determine
    }
    
    // For MBR systems, the first sector (512 bytes) contains critical boot and partition info
    // We'll handle this separately to ensure it's written properly
    const int MBR_SIZE = 512;
    QByteArray mbrBlock(MBR_SIZE, 0);
    bool mbrSaved = false;
    
    const int alignedSize = ((BUFFER_SIZE + bytesPerSector - 1) / bytesPerSector) * bytesPerSector;
    qDebug() << "Using buffer size:" << alignedSize << "bytes with page size:" << bytesPerSector << "bytes";
    
    bool success = true;
    qint64 totalBytesWritten = 0;
    DWORD bytesWritten = 0;
    
    QElapsedTimer timer;
    timer.start();
    
    // First, read and save the MBR
    qint64 mbrBytesRead = sourceFileObj.read(mbrBlock.data(), MBR_SIZE);
    if (mbrBytesRead == MBR_SIZE) {
        // Compute hash from MBR data
        m_verifyHash.addData(QByteArrayView(mbrBlock.data(), mbrBytesRead));
        mbrSaved = true;
        qDebug() << "MBR block saved for later writing";
    } else {
        qWarning() << "Failed to read MBR block, only got" << mbrBytesRead << "bytes";
        // Continue anyway - we'll just write the image sequentially
    }
    
    // Skip the MBR sector when reading the rest of the data
    if (mbrSaved) {
        if (!sourceFileObj.seek(MBR_SIZE)) {
            qWarning() << "Failed to seek past MBR, will write image sequentially";
            sourceFileObj.seek(0); // Reset position
            mbrSaved = false;  // Force sequential writing
        }
    }
    
    // Read and write loop starting from sector 1 (after MBR)
    while (!sourceFileObj.atEnd() && success) {
        // Read chunk from source file
        qint64 bytesRead = sourceFileObj.read(buffer.data(), BUFFER_SIZE);
        if (bytesRead <= 0) {
            break;
        }
        
        // Compute hash from data
        m_verifyHash.addData(QByteArrayView(buffer.data(), bytesRead));
        
        // Round up to sector size if needed
        DWORD bytesToWrite = static_cast<DWORD>(bytesRead);
        if (bytesToWrite % bytesPerSector != 0) {
            bytesToWrite = ((bytesToWrite + bytesPerSector - 1) / bytesPerSector) * bytesPerSector;
            // Zero out any bytes beyond the actual data
            for (DWORD i = bytesRead; i < bytesToWrite; i++) {
                buffer.data()[i] = 0;
            }
        }
        
        // Calculate sector position for direct sector access if this is a physical drive
        if (isPhysicalDrive) {
            // Set file pointer to the correct sector
            LARGE_INTEGER sectorOffset;
            // If we saved the MBR, we need to offset all writes by MBR_SIZE
            sectorOffset.QuadPart = mbrSaved ? (totalBytesWritten + MBR_SIZE) : totalBytesWritten;
            if (!SetFilePointerEx(deviceHandle, sectorOffset, nullptr, FILE_BEGIN)) {
                DWORD errorCode = GetLastError();
                qCritical() << "Failed to seek to correct sector. Error code:" << errorCode;
                
                LPVOID lpMsgBuf;
                FormatMessageA(
                    FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                    NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                    (LPSTR)&lpMsgBuf, 0, NULL);
                qCritical() << "Error message:" << (char*)lpMsgBuf;
                LocalFree(lpMsgBuf);
                
                success = false;
                break;
            }
        }
        
        // Write data to device with simple approach like fat32format does
        if (!WriteFile(deviceHandle, buffer.data(), bytesToWrite, &bytesWritten, NULL)) {
            DWORD errorCode = GetLastError();
            qCritical() << "Error writing to device. Error code:" << errorCode;
            
            LPVOID lpMsgBuf;
            FormatMessageA(
                FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                (LPSTR)&lpMsgBuf, 0, NULL);
            qCritical() << "Error message:" << (char*)lpMsgBuf;
            LocalFree(lpMsgBuf);
            
            // Try once more with a delay
            qDebug() << "Waiting before retry write...";
            Sleep(1000);
            
            if (!WriteFile(deviceHandle, buffer.data(), bytesToWrite, &bytesWritten, NULL)) {
                qCritical() << "Failed to write to device on retry. Error code:" << GetLastError();
                success = false;
                break;
            }
        }
        
        // Update progress tracking
        totalBytesWritten += bytesRead;
        m_bytesWritten = totalBytesWritten;
        
        // Periodically log progress
        if (timer.elapsed() > 5000) {  // Log every 5 seconds
            double mbWritten = static_cast<double>(m_bytesWritten) / (1024 * 1024);
            double percent = static_cast<double>(m_bytesWritten) * 100.0 / m_bytesTotal;
            qDebug() << "Write progress:" << mbWritten << "MB (" << percent << "%)";
            timer.restart();
        }
    }
    
    // Now write the MBR block as the very last step
    if (success && mbrSaved) {
        qDebug() << "Writing MBR block...";
        
        // Seek to beginning of device
        SetFilePointer(deviceHandle, 0, NULL, FILE_BEGIN);
        
        // Write the MBR block aligned to sector size
        DWORD mbrBytesToWrite = MBR_SIZE;
        if (mbrBytesToWrite % bytesPerSector != 0) {
            mbrBytesToWrite = ((mbrBytesToWrite + bytesPerSector - 1) / bytesPerSector) * bytesPerSector;
            mbrBlock.resize(mbrBytesToWrite);
            // Zero out any bytes beyond the MBR data
            for (DWORD i = MBR_SIZE; i < mbrBytesToWrite; i++) {
                mbrBlock.data()[i] = 0;
            }
        }
        
        // Write the MBR block explicitly (try multiple times if needed)
        bool mbrWritten = false;
        for (int attempt = 0; attempt < 3 && !mbrWritten; attempt++) {
            if (WriteFile(deviceHandle, mbrBlock.data(), mbrBytesToWrite, &bytesWritten, NULL)) {
                mbrWritten = true;
                qDebug() << "MBR block successfully written on attempt" << (attempt + 1);
            } else {
                DWORD errorCode = GetLastError();
                qWarning() << "Error writing MBR block to device on attempt" << (attempt + 1) << ". Error code:" << errorCode;
                Sleep(500); // Wait before retry
            }
        }
        
        if (!mbrWritten) {
            qCritical() << "Failed to write MBR block after multiple attempts";
            success = false;
        }
        
        // Final progress update
        m_bytesWritten = m_bytesTotal;
        sendProgressUpdate(WriteProgress, m_bytesWritten, m_bytesTotal);
    }
    
    // Use the fat32format approach to ensure data is properly flushed
    qDebug() << "Flushing device buffers...";
    if (!FlushFileBuffers(deviceHandle)) {
        qWarning() << "Failed to flush file buffers, error:" << GetLastError();
    }
    
    // First close the direct handle to allow Windows to rescan the disk
    qDebug() << "Closing device handle...";
    CloseHandle(deviceHandle);
    deviceHandle = INVALID_HANDLE_VALUE;
    sourceFileObj.close();
    
    // For physical drives, run a post-write operation to ensure the disk is readable
    if (isPhysicalDrive && m_lastDriveNumber >= 0) {
        qDebug() << "Running post-write operations to make disk readable...";
        
        // Wait for Windows to detect the device changes
        Sleep(2000);
        
        // First, signal the kernel to rescan the disk for partitions
        QProcess diskpart;
        QByteArray onlineScript = QString("select disk %1\r\nonline disk\r\nrescan\r\n").arg(m_lastDriveNumber).toLatin1();
        qDebug() << "Running diskpart to rescan disk for partitions:" << onlineScript;
        
        diskpart.start("diskpart", QStringList());
        if (diskpart.waitForStarted(5000)) {
            diskpart.write(onlineScript);
            diskpart.closeWriteChannel();
            diskpart.waitForFinished(30000);
            qDebug() << "Diskpart rescan exit code:" << diskpart.exitCode();
            qDebug() << "Diskpart output:" << diskpart.readAllStandardOutput();
            qDebug() << "Diskpart error:" << diskpart.readAllStandardError();
        }
        
        // Allow Windows to detect the new partitions
        Sleep(3000);
        
        // List the partitions to verify they are detected
        diskpart.close();
        QByteArray listScript = QString("select disk %1\r\nlist partition\r\n").arg(m_lastDriveNumber).toLatin1();
        qDebug() << "Running diskpart to list partitions:" << listScript;
        
        diskpart.start("diskpart", QStringList());
        if (diskpart.waitForStarted(5000)) {
            diskpart.write(listScript);
            diskpart.closeWriteChannel();
            diskpart.waitForFinished(30000);
            QByteArray output = diskpart.readAllStandardOutput();
            QByteArray error = diskpart.readAllStandardError();
            qDebug() << "Diskpart list partitions exit code:" << diskpart.exitCode();
            qDebug() << "Diskpart output:" << output;
            qDebug() << "Diskpart error:" << error;
            
            // If partitions were found, try to assign letters to all of them
            if (output.contains("Partition")) {
                diskpart.close();
                
                // First try the boot partition which is usually the first one
                QByteArray assignScript = QString("select disk %1\r\nselect partition 1\r\nassign\r\n").arg(m_lastDriveNumber).toLatin1();
                qDebug() << "Running diskpart to assign drive letter to boot partition:" << assignScript;
                
                diskpart.start("diskpart", QStringList());
                if (diskpart.waitForStarted(5000)) {
                    diskpart.write(assignScript);
                    diskpart.closeWriteChannel();
                    diskpart.waitForFinished(30000);
                    qDebug() << "Diskpart drive letter assignment exit code:" << diskpart.exitCode();
                    qDebug() << "Diskpart output:" << diskpart.readAllStandardOutput();
                    qDebug() << "Diskpart error:" << diskpart.readAllStandardError();
                }
                
                // Also look for a second partition (usually the main system partition)
                if (output.contains("Partition 2")) {
                    diskpart.close();
                    QByteArray assignScript2 = QString("select disk %1\r\nselect partition 2\r\nassign\r\n").arg(m_lastDriveNumber).toLatin1();
                    qDebug() << "Running diskpart to assign drive letter to system partition:" << assignScript2;
                    
                    diskpart.start("diskpart", QStringList());
                    if (diskpart.waitForStarted(5000)) {
                        diskpart.write(assignScript2);
                        diskpart.closeWriteChannel();
                        diskpart.waitForFinished(30000);
                        qDebug() << "Diskpart drive letter assignment exit code:" << diskpart.exitCode();
                        qDebug() << "Diskpart output:" << diskpart.readAllStandardOutput();
                        qDebug() << "Diskpart error:" << diskpart.readAllStandardError();
                    }
                }
            } else {
                qWarning() << "No partitions found after writing image - the image may have a non-standard partition format";
            }
        }
    } else if (isPhysicalDrive) {
        // Just unlock the volume if we don't have a drive number
        qDebug() << "Unlocking volume...";
        HANDLE unlockHandle = CreateFileA(
            fixedDevicePath.toLatin1().constData(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );
        
        if (unlockHandle != INVALID_HANDLE_VALUE) {
            DWORD bytesReturned = 0;
            if (!DeviceIoControl(unlockHandle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &bytesReturned, NULL)) {
                qWarning() << "Failed to unlock volume, error:" << GetLastError() << " (continuing anyway)";
            }
            CloseHandle(unlockHandle);
        }
    }
    
    // Wait for all operations to complete
    Sleep(1000);
    
    // Stop progress timer
    m_progressTimer.stop();
    disconnect(&m_progressTimer, nullptr, this, nullptr);
    
    if (success) {
        // Store hash for verification
        m_sourceHash = m_verifyHash.result();
        qDebug() << "Write operation completed successfully. Hash:" << m_sourceHash.toHex();
        // Final progress update
        sendProgressUpdate(WriteProgress, m_bytesTotal, m_bytesTotal);
    }
    
    return success;
}

// New function to check for administrator privileges
bool DiskWriterHelper::isRunningAsAdmin() const
{
    BOOL isAdmin = FALSE;
    HANDLE hToken = NULL;
    
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION elevation;
        DWORD cbSize = sizeof(TOKEN_ELEVATION);
        
        if (GetTokenInformation(hToken, TokenElevation, &elevation, sizeof(elevation), &cbSize)) {
            isAdmin = elevation.TokenIsElevated;
        }
        
        CloseHandle(hToken);
    }
    
    return isAdmin;
}

bool DiskWriterHelper::customizeImage(const QString &drive, const QByteArray &config, 
                              const QByteArray &cmdline, const QByteArray &firstrun,
                              const QByteArray &cloudinit, const QByteArray &cloudInitNetwork,
                              const QByteArray &initFormat)
{
    qDebug() << "Helper: Customizing image on drive:" << drive;
    
    QByteArray _config = config;
    QByteArray _cmdline = cmdline;
    QByteArray _firstrun = firstrun;
    QByteArray _cloudinit = cloudinit;
    QByteArray _cloudinitNetwork = cloudInitNetwork;
    QByteArray _initFormat = initFormat;
    
    try {
        // Create a WinFile for device access
        WinFile winFile(this);
        winFile.setFileName(drive);
        
        if (!winFile.open(QIODevice::ReadWrite)) {
            qCritical() << "Failed to open device for customization:" << winFile.errorString();
            return false;
        }
        
        // Create a device wrapper with the WinFile
        DeviceWrapper dw(&winFile, this);
        DeviceWrapperFatPartition *fat = dw.fatPartition(1);
        
        if (!_config.isEmpty()) {
            auto configItems = _config.split('\n');
            configItems.removeAll("");
            QByteArray config = fat->readFile("config.txt");
            
            for (const QByteArray& item : configItems) {
                if (config.contains("#"+item)) {
                    // Uncomment existing line
                    config.replace("#"+item, item);
                } else if (config.contains("\n"+item)) {
                    // config.txt already contains the line
                } else {
                    // Append new line to config.txt
                    if (config.right(1) != "\n")
                        config += "\n"+item+"\n";
                    else
                        config += item+"\n";
                }
            }
            
            fat->writeFile("config.txt", config);
        }
        
        if (_initFormat == "auto") {
            // Auto-detect what customization format the image supports
            QByteArray issue = fat->readFile("issue.txt");
            
            if (fat->fileExists("user-data")) {
                // If we have user-data file on FAT partition, then it must be cloudinit
                _initFormat = "cloudinit";
                qDebug() << "user-data found on FAT partition. Assuming cloudinit support";
            }
            else if (issue.contains("pi-gen")) {
                // If issue.txt mentions pi-gen, and there is no user-data file assume
                // it is a RPI OS flavor, and use the old systemd unit firstrun script stuff
                _initFormat = "systemd";
                qDebug() << "using firstrun script invoked by systemd customization method";
            }
            else {
                // Fallback to writing cloudinit file, as it does not hurt having one
                // Will just have no customization if OS does not support it
                _initFormat = "cloudinit";
                qDebug() << "Unknown what customization method image supports. Falling back to cloudinit";
            }
        }
        
        if (!_firstrun.isEmpty() && _initFormat == "systemd") {
            fat->writeFile("firstrun.sh", _firstrun);
            _cmdline += " systemd.run=/boot/firstrun.sh systemd.run_success_action=reboot systemd.unit=kernel-command-line.target";
        }
        
        if (!_cloudinit.isEmpty() && _initFormat == "cloudinit") {
            _cloudinit = "#cloud-config\n"+_cloudinit;
            fat->writeFile("user-data", _cloudinit);
        }
        
        if (!_cloudinitNetwork.isEmpty() && _initFormat == "cloudinit") {
            fat->writeFile("network-config", _cloudinitNetwork);
        }
        
        if (!_cmdline.isEmpty()) {
            QByteArray cmdline = fat->readFile("cmdline.txt").trimmed();
            cmdline += _cmdline;
            fat->writeFile("cmdline.txt", cmdline);
        }
        
        dw.sync();
        
        // Close the file when done
        winFile.close();
    }
    catch (std::runtime_error &err) {
        qCritical() << "Error during customization:" << err.what();
        return false;
    }
    
    qDebug() << "Image customization completed successfully";
    return true;
}

bool DiskWriterHelper::verifyImage(const QString &drive, const QString &sourceFile, const QByteArray &expectedHash)
{
    qDebug() << "Helper: Verifying image on drive:" << drive;
    
    // If we don't have a source hash from a write operation, we can't verify
    if (m_sourceHash.isEmpty()) {
        qCritical() << "No source hash available for verification";
        return false;
    }
    
    constexpr qint64 VERIFY_BLOCK_SIZE = 10 * 1024 * 1024; // 10MB buffer for verification
    QFile deviceFile(drive);
    
    if (!deviceFile.open(QIODevice::ReadOnly)) {
        qCritical() << "Failed to open device for verification:" << deviceFile.errorString();
        return false;
    }
    
    // Get the total size to read
    qint64 totalBytes = m_bytesTotal;
    if (totalBytes <= 0) {
        // If we don't know the size, read the whole device
        qDebug() << "Unknown total bytes, using device size for verification";
        totalBytes = deviceFile.size();
    }
    
    // Reset hash for verification
    QCryptographicHash verifyHash(QCryptographicHash::Sha256);
    qint64 bytesVerified = 0;
    QElapsedTimer timer;
    timer.start();
    
    QByteArray buffer(VERIFY_BLOCK_SIZE, 0);
    
    // First block handling (skip first, read it last)
    qint64 firstBlockSize = 0;
    QByteArray firstBlock;
    
    // Read second block onwards first
    if (!deviceFile.seek(VERIFY_BLOCK_SIZE)) {
        qCritical() << "Failed to seek in device file:" << deviceFile.errorString();
        deviceFile.close();
        return false;
    }
    
    // Read the device and compute hash
    while (bytesVerified < totalBytes) {
        qint64 bytesToRead = qMin(VERIFY_BLOCK_SIZE, totalBytes - bytesVerified);
        qint64 bytesRead = deviceFile.read(buffer.data(), bytesToRead);
        
        if (bytesRead <= 0) {
            break;
        }
        
        verifyHash.addData(QByteArrayView(buffer.data(), bytesRead));
        bytesVerified += bytesRead;
        
        sendProgressUpdate(VerifyProgress, bytesVerified, totalBytes);
        QCoreApplication::processEvents();
    }
    
    // Now read the first block
    deviceFile.seek(0);
    firstBlockSize = qMin((qint64)VERIFY_BLOCK_SIZE, totalBytes);
    firstBlock.resize(firstBlockSize);
    
    qint64 firstBytesRead = deviceFile.read(firstBlock.data(), firstBlockSize);
    if (firstBytesRead > 0) {
        verifyHash.addData(QByteArrayView(firstBlock.data(), firstBytesRead));
    }
    
    // Compute the final hash
    QByteArray verifiedHash = verifyHash.result();
    
    qDebug() << "Computed device hash:" << verifiedHash.toHex();
    qDebug() << "Expected hash (from write):" << m_sourceHash.toHex();
    
    // Check if the hash matches
    bool hashesMatch = (verifiedHash == m_sourceHash);
    
    qint64 elapsedMs = timer.elapsed();
    
    if (hashesMatch) {
        qDebug() << "Verification successful - hashes match. Completed in" << (elapsedMs / 1000.0) << "seconds";
    } else {
        qCritical() << "Verification failed - hash mismatch";
        qCritical() << "Source hash:" << m_sourceHash.toHex();
        qCritical() << "Verified hash:" << verifiedHash.toHex();
    }
    
    deviceFile.close();
    return hashesMatch;
}
