#include "elevationhelper.h"
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <QCoreApplication>
#include <QDataStream>
#include <QRandomGenerator>
#include <QUuid>
#include <QThread>
#include <QFile>
#include <QElapsedTimer>

// Initialize static instance pointer
ElevationHelper* ElevationHelper::s_instance = nullptr;

ElevationHelper::ElevationHelper(QObject *parent) : QObject(parent),
    m_socket(nullptr),
    m_helperProcess(NULL),
    m_operationComplete(false),
    m_connectionState(Disconnected),
    m_operationTimeoutMs(300000) // 5 minutes default timeout
{
    // Make sure socket is created in the same thread as this object
    m_socket = new QLocalSocket(this);
    m_socket->moveToThread(this->thread());
    
    connect(m_socket, &QLocalSocket::readyRead, this, &ElevationHelper::socketReadyRead);
    
    // Only pass socket errors to error handler, don't emit them as application errors
    connect(m_socket, static_cast<void(QLocalSocket::*)(QLocalSocket::LocalSocketError)>(&QLocalSocket::errorOccurred),
            this, &ElevationHelper::handleSocketError);
    
    // Set up timer to process Windows events - ensure it's in the right thread
    m_eventTimer.moveToThread(this->thread());
    connect(&m_eventTimer, &QTimer::timeout, this, &ElevationHelper::processPendingEvents);
    
    // Make sure the helper is terminated when the application exits
    connect(qApp, &QCoreApplication::aboutToQuit, this, &ElevationHelper::shutdownHelper);
    
    // Generate a persistent socket name for this session
    m_socketName = generateSocketName();
    
    qDebug() << "ElevationHelper constructed in state:" << stateToString(m_connectionState);
}

ElevationHelper::~ElevationHelper()
{
    shutdownHelper();
    
    if (m_socket) {
        if (m_socket->isOpen()) {
            m_socket->close();
        }
        delete m_socket;
        m_socket = nullptr;
    }
    
    if (s_instance == this) {
        s_instance = nullptr;
    }
    
    qDebug() << "ElevationHelper destroyed";
}

void ElevationHelper::changeState(ConnectionState newState)
{
    if (m_connectionState == newState) {
        return; // No change
    }
    
    qDebug() << "ElevationHelper state transition:" << stateToString(m_connectionState) 
             << "->" << stateToString(newState);
    
    ConnectionState oldState = m_connectionState;
    m_connectionState = newState;
    
    // Handle state-specific actions
    switch (newState) {
        case Disconnected:
            m_operationComplete = false;
            if (m_socket->state() != QLocalSocket::UnconnectedState) {
                m_socket->disconnectFromServer();
            }
            m_eventTimer.stop();
            break;
            
        case Connecting:
            // Actual connection is handled in ensureHelperRunning
            break;
            
        case HandshakeSending:
            // Handshake sending is handled in the connection code
            break;
            
        case HandshakeReceiving:
            // Just waiting for response
            break;
            
        case Connected:
            // Successfully connected and handshaked
            if (!m_eventTimer.isActive()) {
                m_eventTimer.start(100);
            }
            break;
            
        case Error:
            // Error state - try to clean up
            if (m_socket->state() != QLocalSocket::UnconnectedState) {
                m_socket->disconnectFromServer();
            }
            m_eventTimer.stop();
            break;
    }
    
    // Reset operation timer when entering a waiting state
    if (newState == Connecting || newState == HandshakeSending || newState == HandshakeReceiving) {
        m_operationTimer.start();
    }
    
    // Emit the state change signal
    emit stateChanged(newState);
}

QString ElevationHelper::stateToString(ConnectionState state) const
{
    switch (state) {
        case Disconnected: return "Disconnected";
        case Connecting: return "Connecting";
        case HandshakeSending: return "HandshakeSending";
        case HandshakeReceiving: return "HandshakeReceiving";
        case Connected: return "Connected";
        case Error: return "Error";
        default: return "Unknown";
    }
}

bool ElevationHelper::validateStateForOperation()
{
    if (m_connectionState != Connected) {
        qDebug() << "Operation attempted in invalid state:" << stateToString(m_connectionState);
        
        // If we're in an error state or disconnected, try to reconnect
        if (m_connectionState == Error || m_connectionState == Disconnected) {
            qDebug() << "Attempting to reconnect";
            return ensureHelperRunning();
        }
        
        // If we're in a transitional state, wait for a timeout period
        if (m_connectionState == Connecting || 
            m_connectionState == HandshakeSending || 
            m_connectionState == HandshakeReceiving) {
            
            // Check if we've been in this state too long
            if (m_operationTimer.isValid() && m_operationTimer.elapsed() > 10000) {
                qDebug() << "Timeout waiting for state transition, forcing reconnect";
                changeState(Disconnected);
                return ensureHelperRunning();
            }
            
            qDebug() << "Currently in transition state, operation not allowed";
            emit error(tr("Cannot perform operation while connection is being established"));
            return false;
        }
        
        emit error(tr("Cannot perform operation in current state: %1").arg(stateToString(m_connectionState)));
        return false;
    }
    
    return true;
}

ElevationHelper* ElevationHelper::instance()
{
    if (!s_instance) {
        s_instance = new ElevationHelper(qApp);
    }
    return s_instance;
}

void ElevationHelper::socketReadyRead()
{
    qDebug() << "Socket readyRead signal received, bytes available:" << m_socket->bytesAvailable()
             << "in state:" << stateToString(m_connectionState);
    
    // Even if bytesAvailable returns 0, the readyRead signal indicates data is waiting
    // Try to read anyway
    qint64 bytesAvailable = m_socket->bytesAvailable();
    
    if (bytesAvailable == 0) {
        // Force read a small amount to see if any data is actually available
        QByteArray smallBuffer(128, 0);
        qint64 actuallyRead = m_socket->read(smallBuffer.data(), smallBuffer.size());
        
        if (actuallyRead > 0) {
            qDebug() << "Successfully read" << actuallyRead << "bytes despite bytesAvailable=0";
            smallBuffer.resize(actuallyRead);
            m_messageQueue.append(smallBuffer);
            processNextMessage();
        } else {
            qDebug() << "No bytes could be read despite readyRead signal, actual read result:" << actuallyRead;
        }
        return;
    }
    
    // Read all available data from the socket
    if (bytesAvailable > 0) {
        // For debugging, peek at what's available
        QByteArray peek = m_socket->peek(qMin(bytesAvailable, (qint64)100));
        qDebug() << "Peeking at socket data:" << peek.toHex() << (peek.size() > 50 ? "... (truncated)" : "");
        qDebug() << "Peeking at socket data as string:" << QString::fromUtf8(peek);
        
        // If we're already connected, don't go through handshake states again
        if (m_connectionState == Connected) {
            // Check if this is a command completion message (SUCCESS/FAILURE)
            QDataStream completionCheck(m_socket);
            completionCheck.setVersion(RPI_IMAGER_IPC_VERSION);
            
            // Try to read a string, but only peek - don't remove from stream
            QString statusMessage;
            m_socket->startTransaction();
            completionCheck >> statusMessage;
            
            if (completionCheck.status() == QDataStream::Ok) {
                qDebug() << "Received command completion status:" << statusMessage;
                
                // It's a command completion message - actually consume it from the socket
                m_socket->commitTransaction();
                
                if (statusMessage == "SUCCESS") {
                    qDebug() << "Command completed successfully";
                    m_operationComplete = true;
                } else if (statusMessage == "FAILURE") {
                    qDebug() << "Command failed";
                    emit error(tr("Helper operation failed"));
                    m_operationComplete = true;
                }
                
                return;
            } else {
                // Not a command completion message - rollback transaction and process as normal data
                m_socket->rollbackTransaction();
                completionCheck.resetStatus();
                
                // Just read all data for normal processing
                QByteArray data = m_socket->readAll();
                // Store it for processing
                m_messageQueue.append(data);
                
                // Process it now
                processNextMessage();
            }
            return;
        }
        
        // Handle data based on our current state
        switch (m_connectionState) {
            case HandshakeReceiving:
                // We're waiting for the HELLO message from the helper
                {
                    QByteArray messageData = m_socket->readAll();
                    qDebug() << "Received handshake data:" << messageData.toHex();
                    
                    // Store it for processing
                    m_messageQueue.append(messageData);
                    
                    // Process the message synchronously (we're waiting for handshake)
                    if (processNextMessage()) {
                        // Successfully processed HELLO - switch to responding state
                        changeState(HandshakeSending);
                        
                        // Send READY response
                        QByteArray block;
                        QDataStream out(&block, QIODevice::WriteOnly);
                        out.setVersion(RPI_IMAGER_IPC_VERSION);
                        out << QString("READY");
                        
                        qDebug() << "Sending READY response to helper, size:" << block.size() << "bytes";
                        
                        qint64 bytesWritten = m_socket->write(block);
                        bool flushSuccess = m_socket->flush();
                        
                        if (bytesWritten == block.size()) {
                            // If bytes were written successfully, consider it a success even if flush fails
                            // The bytes might still get through even without an explicit flush
                            if (!flushSuccess) {
                                qWarning() << "Flush failed during handshake, but bytes were written - continuing anyway";
                            }
                            
                            // Wait for bytes to be written, but don't fail if timeout occurs
                            if (!m_socket->waitForBytesWritten(5000)) {
                                qWarning() << "waitForBytesWritten failed:" << m_socket->errorString();
                                // Continue anyway - the bytes might still be written
                            }
                            
                            qDebug() << "READY response sent, handshake complete";
                            changeState(Connected);
                            
                            // Return immediately once we reach the Connected state
                            // Skip any further handshake attempts since we're already connected
                            return;
                        } else {
                            qCritical() << "Failed to send READY response. Bytes written:" << bytesWritten
                                       << "of" << block.size() << ", flush success:" << flushSuccess;
                            changeState(Error);
                        }
                    } else {
                        qCritical() << "Failed to process handshake message";
                        changeState(Error);
                    }
                }
                break;
            
            case Connected:
                // Normal operation - check for command completion or progress updates
                {
                    // Check if this is a command completion message (SUCCESS/FAILURE)
                    QDataStream completionCheck(m_socket);
                    completionCheck.setVersion(RPI_IMAGER_IPC_VERSION);
                    
                    // Try to read a string, but only peek - don't remove from stream
                    QString statusMessage;
                    m_socket->startTransaction();
                    completionCheck >> statusMessage;
                    
                    if (completionCheck.status() == QDataStream::Ok) {
                        qDebug() << "Received command completion status:" << statusMessage;
                        
                        // It's a command completion message - actually consume it from the socket
                        m_socket->commitTransaction();
                        
                        if (statusMessage == "SUCCESS") {
                            qDebug() << "Command completed successfully";
                            m_operationComplete = true;
                        } else if (statusMessage == "FAILURE") {
                            qDebug() << "Command failed";
                            emit error(tr("Helper operation failed"));
                            m_operationComplete = true;
                        }
                        
                        return;
                    } else {
                        // Not a command completion message - rollback transaction and process as progress update
                        m_socket->rollbackTransaction();
                        completionCheck.resetStatus();
                    }
                    
                    // Progress data handling for when helper is running
                    QDataStream in(m_socket);
                    in.setVersion(RPI_IMAGER_IPC_VERSION);
                    
                    // Progress messages are int (progressType) followed by two qint64 (now, total)
                    // Each int is 4 bytes and each qint64 is 8 bytes
                    const int expectedProgressSize = 4 + 8 + 8;
                    
                    // Try to process as many messages as we can, forcing reads if necessary
                    while (m_socket->bytesAvailable() >= expectedProgressSize || m_socket->bytesAvailable() > 0) {
                        qDebug() << "Attempting to read progress data, available:" << m_socket->bytesAvailable();
                        
                        // Try to read the progress data directly
                        int progressType = 0;
                        qint64 now = 0, total = 0;
                        
                        // Start a transaction so we can roll back if it's not a valid message
                        m_socket->startTransaction();
                        in >> progressType >> now >> total;
                        
                        if (in.status() == QDataStream::Ok && progressType >= 1 && progressType <= 3) {
                            // Valid progress message - commit the transaction
                            m_socket->commitTransaction();
                            
                            qDebug() << "Received valid progress update type:" << progressType 
                                    << "now:" << now << "total:" << total;
                            
                            switch (progressType) {
                                case 1: // DownloadProgress
                                    emit downloadProgress(now, total);
                                    break;
                                case 2: // VerifyProgress
                                    emit verifyProgress(now, total);
                                    break;
                                case 3: // WriteProgress
                                    emit writeProgress(now, total);
                                    break;
                                default:
                                    qDebug() << "Unknown progress type:" << progressType;
                                    break;
                            }
                        } else {
                            // Not a valid progress message - roll back and break out
                            m_socket->rollbackTransaction();
                            qDebug() << "Not a valid progress message. Stream status:" << in.status() 
                                    << "progressType:" << progressType;
                            break;
                        }
                        
                        // If we processed all available data, break out
                        if (m_socket->bytesAvailable() == 0) {
                            break;
                        }
                    }
                }
                break;
                
            case HandshakeSending:
                // We shouldn't get data during this phase, but handle it just in case
                qDebug() << "Unexpected data received during handshake sending phase";
                // Store the data in the queue for later processing
                m_messageQueue.append(m_socket->readAll());
                break;
                
            case Connecting:
                // We shouldn't get data during this phase, but handle it just in case
                qDebug() << "Unexpected data received during connecting phase";
                // Store the data in the queue for later processing
                m_messageQueue.append(m_socket->readAll());
                break;
                
            case Disconnected:
            case Error:
                // Just consume the data and log it for debugging
                qDebug() << "Received data in" << stateToString(m_connectionState) << "state, ignoring";
                m_socket->readAll();
                break;
        }
    }
}

void ElevationHelper::processPendingEvents()
{
    if (m_helperProcess != NULL) {
        DWORD exitCode = 0;
        
        // Check if process is still running
        if (GetExitCodeProcess(m_helperProcess, &exitCode)) {
            if (exitCode != STILL_ACTIVE) {
                // Process exited
                m_eventTimer.stop();
                m_operationComplete = true;
                m_connectionState = Disconnected;
                
                if (exitCode != 0) {
                    emit error(tr("Helper application exited with code: %1").arg(exitCode));
                }
                
                CloseHandle(m_helperProcess);
                m_helperProcess = NULL;
            }
        }
    }
}

QString ElevationHelper::generateSocketName()
{
    // Default socket name
    QString socketName = "rpihelperlocalsocket";
    
    // In a real implementation, you could check environment variables 
    // or configuration files here to allow custom socket names
    
    return socketName;
}

bool ElevationHelper::ensureHelperRunning()
{
    // If already connected, we're good
    if (m_connectionState == Connected) {
        qDebug() << "Helper already running, reusing connection";
        return true;
    }
    
    // If we're in a transitional state, wait a bit
    if (m_connectionState == Connecting || 
        m_connectionState == HandshakeSending || 
        m_connectionState == HandshakeReceiving) {
        
        // Check if we've been in this state too long
        if (m_operationTimer.isValid() && m_operationTimer.elapsed() > 10000) {
            qDebug() << "Timeout waiting for connection state, resetting";
            changeState(Disconnected);
        } else {
            qDebug() << "Already attempting to connect, please wait";
            return false;
        }
    }
    
    // Start connection process
    changeState(Connecting);
    
    // Close any previous socket connection
    if (m_socket->state() != QLocalSocket::UnconnectedState) {
        m_socket->disconnectFromServer();
        m_socket->close();
    }
    
    // Close any existing process handle
    if (m_helperProcess != NULL) {
        CloseHandle(m_helperProcess);
        m_helperProcess = NULL;
    }
    
    // Clear any pending messages
    m_messageQueue.clear();
    
    // Get absolute path to helper
    QString helperPath = QDir::toNativeSeparators(
                QCoreApplication::applicationDirPath() + "/rpi-imager-helper.exe");
    
    // Check if helper exists
    QFileInfo helperInfo(helperPath);
    if (!helperInfo.exists()) {
        QString errorMsg = tr("Helper executable not found at: %1").arg(helperPath);
        qCritical() << errorMsg;
        emit error(errorMsg);
        changeState(Error);
        return false;
    }
    
    qDebug() << "Helper path verified:" << helperPath << "Size:" << helperInfo.size() << "Last modified:" << helperInfo.lastModified().toString();
    
    // Start the helper in daemon mode using fixed socket name
    QString args = QString("--daemon");
    
    qDebug() << "Starting helper in daemon mode:" << helperPath << "with args:" << args;
    
    // Now try the actual elevated launch
    SHELLEXECUTEINFO shExInfo = {0};
    shExInfo.cbSize = sizeof(SHELLEXECUTEINFO);
    shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI; // Don't show error UI
    shExInfo.hwnd = NULL;
    shExInfo.lpVerb = L"runas";  // Request elevation
    shExInfo.lpFile = reinterpret_cast<LPCWSTR>(helperPath.utf16());
    shExInfo.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
    shExInfo.lpDirectory = NULL;
    shExInfo.nShow = SW_HIDE;
    shExInfo.hInstApp = NULL;
    
    // Get detailed error information
    DWORD lastError = 0;
    QString errorMessage;
    
    SetLastError(0); // Reset error state
    bool result = ShellExecuteEx(&shExInfo);
    lastError = GetLastError();
    
    if (!result || lastError != 0) {
        errorMessage = getLastErrorAsString(lastError);
        qCritical() << "ShellExecuteEx failed with error:" << errorMessage;
        
        // Additional checks
        if (lastError == ERROR_CANCELLED) {
            emit error(tr("Operation cancelled by user"));
            changeState(Error);
            return false;
        }
        else if (lastError == ERROR_FILE_NOT_FOUND) {
            emit error(tr("Helper application not found or access denied"));
            changeState(Error);
            return false;
        }
        else if (lastError == ERROR_PATH_NOT_FOUND) {
            emit error(tr("Helper application path not found"));
            changeState(Error);
            return false;
        }
        else if (lastError == ERROR_ACCESS_DENIED) {
            emit error(tr("Access denied when trying to run helper"));
            changeState(Error);
            return false;
        }
        else {
            emit error(tr("Failed to execute helper application: %1").arg(errorMessage));
            changeState(Error);
            return false;
        }
    }
    
    // Success - we got a process handle
    if (shExInfo.hProcess == NULL) {
        qCritical() << "ShellExecuteEx returned success but process handle is NULL";
        emit error(tr("Failed to get process handle for helper"));
        changeState(Error);
        return false;
    }
    
    // Save process handle
    m_helperProcess = shExInfo.hProcess;
    qDebug() << "Got process handle:" << m_helperProcess;
    
    // Check if process is still running
    DWORD exitCode = 0;
    if (GetExitCodeProcess(m_helperProcess, &exitCode) && exitCode != STILL_ACTIVE) {
        qCritical() << "Helper process exited immediately with code:" << exitCode;
        emit error(tr("Helper process exited immediately with code: %1").arg(exitCode));
        CloseHandle(m_helperProcess);
        m_helperProcess = NULL;
        changeState(Error);
        return false;
    }
    
    // Process is still active, try to connect
    bool connected = false;
    qDebug() << "Helper process started, waiting for socket...";
    
    // Connection timeout loop - try for up to 5 seconds
    for (unsigned int attempts = 50; attempts != 0; attempts--) {
        qDebug() << "Attempting to connect, attempt " << (attempts + 1);
        
        // Check if process is still running
        if (GetExitCodeProcess(m_helperProcess, &exitCode) && exitCode != STILL_ACTIVE) {
            qCritical() << "Helper process exited with code:" << exitCode << "during connection attempts";
            emit error(tr("Helper process exited with code: %1 during connection attempts").arg(exitCode));
            CloseHandle(m_helperProcess);
            m_helperProcess = NULL;
            changeState(Error);
            return false;
        }
        
        // Try to connect to the socket
        QString socketName = generateSocketName();
        qDebug() << "Connecting to local socket:" << socketName;
        m_socket->connectToServer(socketName);
        if (m_socket->waitForConnected(100)) {
            connected = true;
            qDebug() << "Connected to helper socket";
            break;
        }
        
        // Don't treat connection errors as fatal during retry attempts
        // Just log them and continue to the next attempt
        qDebug() << "Socket error:" << m_socket->errorString() << "(" << m_socket->error() << ")";
        m_socket->abort(); // Reset socket state for next attempt
        QThread::msleep(100);
    }
    
    if (!connected) {
        emit error(tr("Failed to connect to helper application after 50 attempts"));
        if (m_helperProcess) {
            TerminateProcess(m_helperProcess, 1);
            CloseHandle(m_helperProcess);
            m_helperProcess = NULL;
        }
        changeState(Error);
        return false;
    }
    
    // Connected to socket, complete handshake
    qDebug() << "Successfully connected to helper socket, performing handshake";
    changeState(HandshakeReceiving);
    
    // Wait for the handshake message
    bool handshakeReceived = false;
    
    // Check if we already have messages in the queue
    if (!m_messageQueue.isEmpty()) {
        qDebug() << "Already have messages in the queue, processing";
        if (processNextMessage()) {
            // Successfully processed the HELLO message
            handshakeReceived = true;
            
            // Immediately send READY response
            QByteArray block;
            QDataStream out(&block, QIODevice::WriteOnly);
            out.setVersion(RPI_IMAGER_IPC_VERSION);
            out << QString("READY");
            
            qDebug() << "Sending READY response to helper, size:" << block.size() << "bytes";
            changeState(HandshakeSending);
            
            // Check socket state
            if (m_socket->state() != QLocalSocket::ConnectedState) {
                qDebug() << "Socket not connected when trying to send READY response";
                emit error(tr("Socket disconnected before sending handshake response"));
                disconnectAndCleanup();
                changeState(Error);
                return false;
            }
            
            // Write the data
            qint64 bytesWritten = m_socket->write(block);
            if (bytesWritten != block.size()) {
                qDebug() << "Failed to write all bytes:" << bytesWritten << "of" << block.size();
                emit error(tr("Failed to write complete handshake response"));
                disconnectAndCleanup();
                changeState(Error);
                return false;
            }
            
            // Flush the socket
            bool flushSuccess = m_socket->flush();
            qDebug() << "Flush result:" << flushSuccess;
            
            // Allow handshake to proceed even if flush fails
            if (!flushSuccess) {
                qWarning() << "Flush failed during handshake, but bytes were written - continuing anyway";
            }
            
            // Wait for bytes written with a longer timeout
            if (!m_socket->waitForBytesWritten(5000)) {
                qDebug() << "waitForBytesWritten failed:" << m_socket->errorString();
                // Don't treat this as fatal, as messages often get through anyway
                // Just log it and continue
            }
            
            qDebug() << "READY response sent successfully";
            
            // Consider the handshake complete at this point
            qDebug() << "Handshake complete, helper is now running";
            changeState(Connected);
            
            // Start event timer in the main thread
            QMetaObject::invokeMethod(this, [this]() {
                m_eventTimer.start(100);
            }, Qt::QueuedConnection);
            
            // Return true immediately since we're now connected
            return true;
        } else {
            emit error(tr("Failed to process handshake message"));
            m_socket->disconnectFromServer();
            if (m_helperProcess) {
                TerminateProcess(m_helperProcess, 1);
                CloseHandle(m_helperProcess);
                m_helperProcess = NULL;
            }
            changeState(Error);
            return false;
        }
    }
    
    // If we haven't received a valid handshake yet, wait for messages to arrive
    if (!handshakeReceived) {
        int attempts = 0;
        const int maxAttempts = 10;
        
        while (!handshakeReceived && m_connectionState != Connected) {
            // If we're already in Connected state, no need to wait for handshake
            if (m_connectionState == Connected) {
                qDebug() << "Already in Connected state, skipping handshake waiting";
                return true;
            }
            
            if (!m_socket->waitForReadyRead(500)) {
                attempts++;
                qDebug() << "Waiting for handshake message... attempt" << attempts;
                
                // Check if we're in Connected state (might have happened via other thread)
                if (m_connectionState == Connected) {
                    qDebug() << "Connection state changed to Connected during wait, handshake successful";
                    return true;
                }
                
                if (attempts >= maxAttempts) {
                    qDebug() << "Timed out waiting for handshake message after" << attempts << "attempts";
                    emit error(tr("Helper did not send handshake message after %1 attempts").arg(attempts));
                    m_socket->disconnectFromServer();
                    if (m_helperProcess) {
                        TerminateProcess(m_helperProcess, 1);
                        CloseHandle(m_helperProcess);
                        m_helperProcess = NULL;
                    }
                    changeState(Error);
                    return false;
                }
            } else {
                // The readyRead signal should have populated the message queue
                qDebug() << "Data received, messages in queue: " << m_messageQueue.size();
                
                if (!m_messageQueue.isEmpty()) {
                    if (processNextMessage()) {
                        // Successfully processed the HELLO message
                        handshakeReceived = true;
                        
                        // Immediately send READY response
                        QByteArray block;
                        QDataStream out(&block, QIODevice::WriteOnly);
                        out.setVersion(RPI_IMAGER_IPC_VERSION);
                        out << QString("READY");
                        
                        qDebug() << "Sending READY response to helper, size:" << block.size() << "bytes";
                        changeState(HandshakeSending);
                        
                        // Check socket state
                        if (m_socket->state() != QLocalSocket::ConnectedState) {
                            qDebug() << "Socket not connected when trying to send READY response";
                            emit error(tr("Socket disconnected before sending handshake response"));
                            disconnectAndCleanup();
                            changeState(Error);
                            return false;
                        }
                        
                        // Write the data
                        qint64 bytesWritten = m_socket->write(block);
                        if (bytesWritten != block.size()) {
                            qDebug() << "Failed to write all bytes:" << bytesWritten << "of" << block.size();
                            emit error(tr("Failed to write complete handshake response"));
                            disconnectAndCleanup();
                            changeState(Error);
                            return false;
                        }
                        
                        // Flush the socket
                        bool flushSuccess = m_socket->flush();
                        qDebug() << "Flush result:" << flushSuccess;
                        
                        // Allow handshake to proceed even if flush fails
                        if (!flushSuccess) {
                            qWarning() << "Flush failed during handshake, but bytes were written - continuing anyway";
                        }
                        
                        // Wait for bytes written with a longer timeout
                        if (!m_socket->waitForBytesWritten(5000)) {
                            qDebug() << "waitForBytesWritten failed:" << m_socket->errorString();
                            // Don't treat this as fatal, as messages often get through anyway
                            // Just log it and continue
                        }
                        
                        qDebug() << "READY response sent successfully";
                        
                        // Consider the handshake complete at this point
                        qDebug() << "Handshake complete, helper is now running";
                        changeState(Connected);
                        
                        // Start event timer in the main thread
                        QMetaObject::invokeMethod(this, [this]() {
                            m_eventTimer.start(100);
                        }, Qt::QueuedConnection);
                        break;
                    } else {
                        emit error(tr("Failed to process handshake message"));
                        m_socket->disconnectFromServer();
                        if (m_helperProcess) {
                            TerminateProcess(m_helperProcess, 1);
                            CloseHandle(m_helperProcess);
                            m_helperProcess = NULL;
                        }
                        changeState(Error);
                        return false;
                    }
                }
                
                // If readyRead didn't add to the queue, there might be an issue with the framing
                qDebug() << "ReadyRead occurred but no messages were added to the queue";
            }
        }
    }
    
    // If we get here, the handshake was successful
    qDebug() << "Helper handshake successful, helper is ready";
    
    return m_connectionState == Connected;
}

bool ElevationHelper::runFormatDrive(const QString &drive)
{
    if (!validateStateForOperation()) {
        return false;
    }
    
    QString cmd = QString("FORMAT \"%1\"").arg(drive);
    m_operationComplete = false;
    m_operationTimer.start(); // Start timeout timer
    
    if (!sendCommand(cmd)) {
        return false;
    }
    
    // Wait for operation to complete
    while (!m_operationComplete) {
        // Check for timeout
        if (m_operationTimer.elapsed() > m_operationTimeoutMs) {
            qCritical() << "Format operation timed out after" << (m_operationTimeoutMs / 1000) << "seconds";
            emit error(tr("Format operation timed out"));
            return false;
        }
        
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
    
    return true;
}

bool ElevationHelper::runWriteToDrive(const QString &drive, const QString &sourceFile)
{
    if (!validateStateForOperation()) {
        return false;
    }
    
    qDebug() << "Preparing to write" << sourceFile << "to" << drive;
    
    // Use forward slashes for the source file path (works fine on Windows)
    QString safeSourceFile = sourceFile;
    safeSourceFile.replace("\\", "/");
    
    // For the physical drive, use the path as-is or with minimal escaping
    QString safeDrive = drive;
    if (drive.startsWith("\\\\.\\")) {
        // For physical drives, don't escape the backslashes in the path prefix
        safeDrive = drive;
    }
    
    // Escape any quotes in the paths
    safeDrive.replace("\"", "\\\"");
    safeSourceFile.replace("\"", "\\\"");
    
    QString cmd = QString("WRITE \"%1\" \"%2\"").arg(safeDrive, safeSourceFile);
    qDebug() << "Command constructed:" << cmd;
    m_operationComplete = false;
    m_operationTimer.start(); // Start timeout timer
    
    // Track operation success/failure using a local variable
    bool operationFailed = false;
    
    // Connect error signal to track failures
    QMetaObject::Connection errorConnection = connect(this, &ElevationHelper::error,
        [&operationFailed, this](const QString &errorMsg) {
            qDebug() << "Error detected during write operation:" << errorMsg;
            operationFailed = true;
            m_operationComplete = true;
        });
    
    if (!sendCommand(cmd)) {
        qCritical() << "Failed to send WRITE command to helper";
        disconnect(errorConnection);
        return false;
    }
    
    qDebug() << "WRITE command sent successfully, waiting for completion...";
    
    // Temporarily flush events to ensure command is processed
    QCoreApplication::processEvents();
    
    // Wait for operation to complete with a timeout
    while (!m_operationComplete) {
        // Check for timeout
        if (m_operationTimer.elapsed() > m_operationTimeoutMs) {
            qCritical() << "Write operation timed out after" << (m_operationTimeoutMs / 1000) << "seconds";
            emit error(tr("Write operation timed out"));
            disconnect(errorConnection);
            return false;
        }
        
        // Check if we have any data to read from the socket - or force a read anyway
        if (m_socket->bytesAvailable() > 0) {
            qDebug() << "Data available in socket, processing...";
            
            // First try to peek at the data to see if it contains a FAILURE string
            QByteArray peek = m_socket->peek(m_socket->bytesAvailable());
            qDebug() << "Socket data:" << peek.toHex() << "(as string):" << QString::fromUtf8(peek);
                
            // Check for direct "FAILURE" string in the raw data
            if (QString::fromUtf8(peek).contains("FAILURE")) {
                qDebug() << "FAILURE detected in raw socket data";
                emit error(tr("Helper operation failed"));
                operationFailed = true;
                m_operationComplete = true;
                
                // Consume the data
                m_socket->readAll();
                break;
            }
            
            // Handle the data using the standard method
            socketReadyRead();
        }
        // Instead of polling regularly, we'll only do a forced read 
        // when a specific amount of time has passed without any activity
        else if (m_socket->state() == QLocalSocket::ConnectedState && 
                 m_operationTimer.elapsed() > 30000) {  // Only check after 30 seconds of inactivity
            qDebug() << "Checking connection after inactivity period...";
            
            // Restart the timer to avoid too frequent checks
            m_operationTimer.restart();
            
            // Try to read a small amount even if bytesAvailable reports 0
            // This helps detect disconnections that weren't properly signaled
            QByteArray smallBuffer(128, 0);
            qint64 actuallyRead = m_socket->read(smallBuffer.data(), smallBuffer.size());
            
            if (actuallyRead > 0) {
                qDebug() << "Successfully read" << actuallyRead << "bytes from forced read";
                smallBuffer.resize(actuallyRead);
                
                // Process this data - first add it to the queue
                m_messageQueue.append(smallBuffer);
                processNextMessage();
            }
        }
        
        // Check if operation is complete after processing socket data
        if (m_operationComplete || operationFailed) {
            qDebug() << "Operation completed or failed during socket read";
            break;
        }
    }
    
    // Disconnect the temporary error handler
    disconnect(errorConnection);
    
    // Log completion and return operation result
    qDebug() << "Write operation completed in" << (m_operationTimer.elapsed() / 1000) << "seconds"
             << "with " << (operationFailed ? "FAILURE" : "SUCCESS");
    
    return !operationFailed;
}

bool ElevationHelper::sendCommand(const QString &command)
{
    if (!m_socket->isOpen()) {
        emit error(tr("Helper connection is not open"));
        return false;
    }
    
    // Perform the socket operations directly rather than using invokeMethod
    // to avoid potential deadlocks
    QByteArray block;
    QDataStream out(&block, QIODevice::WriteOnly);
    out.setVersion(RPI_IMAGER_IPC_VERSION);
    
    out << command;
    
    qDebug() << "Sending command to helper:" << command << "size:" << block.size() << "bytes, raw data:" << block.toHex();
    qint64 bytesWritten = m_socket->write(block);
    bool flushed = m_socket->flush();
    
    qDebug() << "Command sent: wrote" << bytesWritten << "of" << block.size() << "bytes, flush:" << flushed;
    
    bool success = false;
    
    if (bytesWritten == -1) {
        qCritical() << "Failed to send command to helper - write error:" << m_socket->errorString();
        success = false;
    } else if (bytesWritten != block.size()) {
        qCritical() << "Only partial command sent:" << bytesWritten << "of" << block.size() << "bytes";
        success = false;
    } else {
        // All bytes were written, but flush may have failed
        if (!flushed) {
            qWarning() << "Flush failed when sending command, but bytes were written - continuing anyway";
        }
        
        // Wait for the bytes to be actually written
        if (!m_socket->waitForBytesWritten(5000)) {
            qWarning() << "Timeout waiting for command to be sent, error:" << m_socket->errorString();
            qWarning() << "Command may or may not arrive at helper - continuing with operation";
        }
        
        qDebug() << "Command successfully sent to helper";
        success = true;
    }
    
    // If we're still failing, check socket state
    if (!success) {
        qDebug() << "Socket state:" << m_socket->state() 
                << "Error:" << m_socket->error()
                << "Error string:" << m_socket->errorString();
        emit error(tr("Failed to send command to helper: %1").arg(m_socket->errorString()));
    }
    
    return success;
}

bool ElevationHelper::runHelperWithArgs(const QString &args)
{
    // This method is now deprecated but kept for backward compatibility
    // It uses the one-shot approach with a separate helper process
    QString helperPath = QDir::toNativeSeparators(
                QCoreApplication::applicationDirPath() + "/rpi-imager-helper.exe");
    
    qDebug() << "Executing helper:" << helperPath << "with args:" << args;
    
    // Close the previous socket if it's open
    if (m_socket->isOpen()) {
        m_socket->close();
    }
    
    // Close any previous process handle
    if (m_helperProcess != NULL) {
        CloseHandle(m_helperProcess);
        m_helperProcess = NULL;
    }
    
    m_operationComplete = false;
    
    SHELLEXECUTEINFO shExInfo = {0};
    shExInfo.cbSize = sizeof(SHELLEXECUTEINFO);
    shExInfo.fMask = SEE_MASK_NOCLOSEPROCESS;
    shExInfo.hwnd = NULL;
    shExInfo.lpVerb = L"runas";  // Request elevation
    shExInfo.lpFile = reinterpret_cast<LPCWSTR>(helperPath.utf16());
    shExInfo.lpParameters = reinterpret_cast<LPCWSTR>(args.utf16());
    shExInfo.lpDirectory = NULL;
    shExInfo.nShow = SW_HIDE;
    shExInfo.hInstApp = NULL;
    
    if (!ShellExecuteEx(&shExInfo))
    {
        DWORD dwError = GetLastError();
        if (dwError == ERROR_CANCELLED)
        {
            // User cancelled the UAC prompt
            emit error(tr("Operation cancelled by user"));
        }
        else
        {
            emit error(tr("Failed to execute helper application with error: %1").arg(dwError));
        }
        return false;
    }
    
    // Save process handle
    m_helperProcess = shExInfo.hProcess;
    
    // Connect to the socket
    // Allow time for the helper process to start and create the server
    QTimer::singleShot(500, [this]() {
        if (!m_operationComplete) {
            // Ensure we use the named pipe format for the socket name
            QString socketToUse = m_socketName;
            if (!socketToUse.startsWith("//./pipe/")) {
                socketToUse = QString("//./pipe/%1").arg(m_socketName);
            }
            qDebug() << "Connecting to socket:" << socketToUse;
            m_socket->connectToServer(socketToUse);
            
            // Start process monitoring timer
            m_eventTimer.start(100);
        }
    });
    
    // Wait for the process to finish
    WaitForSingleObject(shExInfo.hProcess, INFINITE);
    
    // Get the exit code
    DWORD exitCode = 0;
    GetExitCodeProcess(shExInfo.hProcess, &exitCode);
    
    if (exitCode != 0)
    {
        emit error(tr("Helper application exited with code: %1").arg(exitCode));
        return false;
    }
    
    return true;
}

void ElevationHelper::shutdownHelper()
{
    if (m_connectionState == Connected && m_socket->isOpen()) {
        // Send shutdown command
        sendCommand("SHUTDOWN");
        m_socket->waitForBytesWritten();
        
        // Wait briefly for clean shutdown
        QThread::msleep(500);
    }
    
    if (m_helperProcess != NULL) {
        // Ensure process is terminated
        TerminateProcess(m_helperProcess, 0);
        CloseHandle(m_helperProcess);
        m_helperProcess = NULL;
    }
    
    if (m_socket->isOpen()) {
        m_socket->close();
    }
    
    m_connectionState = Disconnected;
}

void ElevationHelper::handleSocketError(QLocalSocket::LocalSocketError socketError)
{
    // Ignore PeerClosedError (normal during shutdown)
    // Also ignore ServerNotFoundError during connection attempts
    if (socketError != QLocalSocket::PeerClosedError && 
        !(socketError == QLocalSocket::ServerNotFoundError && m_connectionState != Connected)) {
        qDebug() << "Socket error:" << m_socket->errorString() << "in state:" << stateToString(m_connectionState);
        
        // Update state based on error
        if (m_connectionState == Connecting || 
            m_connectionState == HandshakeSending || 
            m_connectionState == HandshakeReceiving) {
            qDebug() << "Connection error during connection setup, transitioning to Error state";
            changeState(Error);
        }
    }
}

QString ElevationHelper::getLastErrorAsString(DWORD errorCode)
{
    if (errorCode == 0) {
        errorCode = GetLastError();
    }
    
    if (errorCode == 0) {
        return tr("No error occurred");
    }
    
    LPWSTR messageBuffer = nullptr;
    size_t size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM | 
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, 
        errorCode, 
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&messageBuffer, 
        0, 
        NULL);
    
    QString message = (size > 0) ? 
        QString::fromWCharArray(messageBuffer, size).trimmed() :
        QString("Unknown error (%1)").arg(errorCode);
    
    LocalFree(messageBuffer);
    
    return QString("Error %1: %2").arg(errorCode).arg(message);
}

bool ElevationHelper::processNextMessage()
{
    if (m_messageQueue.isEmpty()) {
        qDebug() << "No messages to process";
        return false;
    }
    
    // Get the next message
    QByteArray messageData = m_messageQueue.takeFirst();
    qDebug() << "Processing message of" << messageData.size() << "bytes, hex:" << messageData.toHex();
    
    // Process message based on current state
    switch (m_connectionState) {
        case HandshakeReceiving:
            // Expecting HELLO message during handshake
            {
                // Use QDataStream to properly decode the QString
                QDataStream stream(messageData);
                stream.setVersion(RPI_IMAGER_IPC_VERSION);
                
                QString greeting;
                stream >> greeting;
                
                if (stream.status() != QDataStream::Ok) {
                    qCritical() << "Failed to parse handshake greeting, status:" << stream.status();
                    return false;
                }
                
                qDebug() << "Parsed handshake greeting:" << greeting;
                
                if (greeting == "HELLO") {
                    qDebug() << "Valid HELLO greeting received";
                    return true;
                } else {
                    qCritical() << "Invalid handshake greeting:" << greeting;
                    return false;
                }
            }
            break;
            
        case Connected:
            // Process normal messages during connected state
            {
                // Try to interpret as a command completion message
                QDataStream stream(messageData);
                stream.setVersion(RPI_IMAGER_IPC_VERSION);
                
                QString command;
                stream >> command;
                
                if (stream.status() == QDataStream::Ok) {
                    if (command == "SUCCESS" || command == "FAILURE") {
                        qDebug() << "Received command status:" << command;
                        m_operationComplete = true;
                        
                        if (command == "FAILURE") {
                            emit error(tr("Helper operation failed"));
                        }
                        
                        return true;
                    }
                }
                
                // If not a command status, try to interpret as a progress update
                stream.device()->reset();
                stream.resetStatus();
                
                int progressType;
                qint64 now, total;
                
                stream >> progressType >> now >> total;
                
                if (stream.status() == QDataStream::Ok) {
                    switch (progressType) {
                        case 1: // DownloadProgress
                            emit downloadProgress(now, total);
                            break;
                        case 2: // VerifyProgress
                            emit verifyProgress(now, total);
                            break;
                        case 3: // WriteProgress
                            emit writeProgress(now, total);
                            break;
                        default:
                            qDebug() << "Unknown progress type:" << progressType;
                            break;
                    }
                    return true;
                }
                
                qDebug() << "Failed to parse message in Connected state";
                return false;
            }
            break;
            
        case Disconnected:
            qDebug() << "Skipping message processing in Disconnected state";
            return false;
            
        case Connecting:
            // If we get a message in Connecting state, it might be an early handshake
            // Try to process it as if we were in HandshakeReceiving
            qDebug() << "Got message in Connecting state, treating as early handshake message";
            if (m_connectionState == Connecting) {
                changeState(HandshakeReceiving);
                // Re-process the message in HandshakeReceiving state
                m_messageQueue.prepend(messageData);
                return processNextMessage();
            }
            return false;
            
        default:
            // In other states, we don't expect messages to be processed
            qDebug() << "Unexpected message in state" << stateToString(m_connectionState);
            return false;
    }
    
    return false;
}

void ElevationHelper::disconnectAndCleanup()
{
    if (m_socket->isOpen()) {
        m_socket->disconnectFromServer();
        m_socket->close();
    }
    
    if (m_helperProcess != NULL) {
        TerminateProcess(m_helperProcess, 1);
        CloseHandle(m_helperProcess);
        m_helperProcess = NULL;
    }
    
    m_connectionState = Disconnected;
}

bool ElevationHelper::runCustomizeImage(const QString &drive, const QByteArray &config, 
                                  const QByteArray &cmdline, const QByteArray &firstrun,
                                  const QByteArray &cloudinit, const QByteArray &cloudInitNetwork,
                                  const QByteArray &initFormat)
{
    if (!validateStateForOperation()) {
        return false;
    }
    
    qDebug() << "Preparing to customize image on drive:" << drive;
    
    // Base64 encode all binary data to make it safe for command transmission
    QString base64Config = QString::fromLatin1(config.toBase64());
    QString base64Cmdline = QString::fromLatin1(cmdline.toBase64());
    QString base64Firstrun = QString::fromLatin1(firstrun.toBase64());
    QString base64Cloudinit = QString::fromLatin1(cloudinit.toBase64());
    QString base64CloudInitNetwork = QString::fromLatin1(cloudInitNetwork.toBase64());
    QString base64InitFormat = QString::fromLatin1(initFormat.toBase64());
    
    // Create the command with all parameters
    QString cmd = QString("CUSTOMIZE \"%1\" \"%2\" \"%3\" \"%4\" \"%5\" \"%6\" \"%7\"")
                    .arg(drive)
                    .arg(base64Config)
                    .arg(base64Cmdline)
                    .arg(base64Firstrun)
                    .arg(base64Cloudinit)
                    .arg(base64CloudInitNetwork)
                    .arg(base64InitFormat);
    
    m_operationComplete = false;
    m_operationTimer.start(); // Start timeout timer
    
    if (!sendCommand(cmd)) {
        return false;
    }
    
    // Wait for operation to complete
    while (!m_operationComplete) {
        // Check for timeout
        if (m_operationTimer.elapsed() > m_operationTimeoutMs) {
            qCritical() << "Customize operation timed out after" << (m_operationTimeoutMs / 1000) << "seconds";
            emit error(tr("Customize operation timed out"));
            return false;
        }
        
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
    
    return true;
}

bool ElevationHelper::runVerifyImage(const QString &drive, const QString &sourceFile, const QByteArray &expectedHash)
{
    if (!validateStateForOperation()) {
        return false;
    }
    
    qDebug() << "Preparing to verify image on drive:" << drive << "against source:" << sourceFile;
    
    // Make sure to properly escape quotes in file paths
    QString safeSourceFile = sourceFile;
    safeSourceFile.replace("\"", "\\\"");
    
    // Base64 encode the hash to make it safe for command transmission
    QString base64Hash = QString::fromLatin1(expectedHash.toBase64());
    
    QString cmd = QString("VERIFY \"%1\" \"%2\" \"%3\"").arg(drive, safeSourceFile, base64Hash);
    m_operationComplete = false;
    m_operationTimer.start(); // Start timeout timer
    
    if (!sendCommand(cmd)) {
        return false;
    }
    
    // Wait for operation to complete
    while (!m_operationComplete) {
        // Check for timeout
        if (m_operationTimer.elapsed() > m_operationTimeoutMs) {
            qCritical() << "Verify operation timed out after" << (m_operationTimeoutMs / 1000) << "seconds";
            emit error(tr("Verify operation timed out"));
            return false;
        }
        
        QCoreApplication::processEvents();
        QThread::msleep(50);
    }
    
    return true;
}

