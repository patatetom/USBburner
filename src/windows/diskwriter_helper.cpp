#include "diskwriter_helper.h"
#include "../downloadthread.h"
#include "../driveformatthread.h"
#include <QDebug>
#include <QDataStream>
#include <regex>
#include <windows.h>
#include <QTimer>
#include <QFile>

DiskWriterHelper::DiskWriterHelper(QObject *parent) : QObject(parent),
    m_server(nullptr), m_clientConnection(nullptr), m_bytesTotal(0), m_bytesWritten(0)
{
    m_server = new QLocalServer(this);
    connect(m_server, &QLocalServer::newConnection, this, &DiskWriterHelper::onNewConnection);
    connect(&m_progressTimer, &QTimer::timeout, this, &DiskWriterHelper::progressTimeout);
}

DiskWriterHelper::~DiskWriterHelper()
{
    if (m_clientConnection) {
        m_clientConnection->close();
    }
    
    if (m_server) {
        m_server->close();
    }
}

void DiskWriterHelper::progressTimeout()
{
    // Send progress updates periodically
    onProgressChanged(m_bytesWritten, m_bytesTotal);
}

void DiskWriterHelper::onNewConnection()
{
    if (m_clientConnection) {
        m_clientConnection->close();
    }
    
    m_clientConnection = m_server->nextPendingConnection();
    qDebug() << "Client connected to progress server";
}

void DiskWriterHelper::sendProgressUpdate(int progressType, qint64 now, qint64 total)
{
    if (m_clientConnection && m_clientConnection->isOpen()) {
        QByteArray block;
        QDataStream out(&block, QIODevice::WriteOnly);
        out.setVersion(QDataStream::Qt_6_9);
        
        // Write progress type and values
        out << progressType << now << total;
        
        m_clientConnection->write(block);
        m_clientConnection->flush();
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
    
    QCommandLineOption formatOption(QStringList() << "f" << "format", "Format the drive", "drive");
    parser.addOption(formatOption);
    
    QCommandLineOption writeOption(QStringList() << "w" << "write", "Write image to drive", "drive");
    parser.addOption(writeOption);
    
    QCommandLineOption sourceOption(QStringList() << "s" << "source", "Source image file", "file");
    parser.addOption(sourceOption);
    
    QCommandLineOption socketOption(QStringList() << "socket", "Socket name for progress reporting", "name");
    parser.addOption(socketOption);
    
    parser.process(args);
    
    // Setup socket server if socket name provided
    if (parser.isSet(socketOption)) {
        m_socketName = parser.value(socketOption);
        
        if (!m_socketName.isEmpty()) {
            // Remove any existing server with this name
            QLocalServer::removeServer(m_socketName);
            
            if (!m_server->listen(m_socketName)) {
                qCritical() << "Could not start local server:" << m_server->errorString();
            } else {
                qDebug() << "Started progress server on:" << m_socketName;
            }
        }
    }
    
    if (parser.isSet(formatOption)) {
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

bool DiskWriterHelper::formatDrive(const QString &drive)
{
    QByteArray devicePath = drive.toLatin1();
    DriveFormatThread formatThread(devicePath, this);
    
    bool success = false;
    QObject::connect(&formatThread, &DriveFormatThread::success, [&success]() {
        success = true;
    });
    
    QObject::connect(&formatThread, &DriveFormatThread::error, [](const QString &msg) {
        qCritical() << "Format error:" << msg;
    });
    
    formatThread.start();
    formatThread.wait();
    
    return success;
}

bool DiskWriterHelper::writeToDrive(const QString &drive, const QString &sourceFile)
{
    // Use our own custom implementation for writing directly to the device
    return writeImageToDevice(sourceFile, drive);
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
    
    // Open device for writing
    HANDLE deviceHandle = CreateFileA(
        devicePath.toLatin1().constData(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        NULL
    );
    
    if (deviceHandle == INVALID_HANDLE_VALUE) {
        DWORD errorCode = GetLastError();
        qCritical() << "Failed to open device for writing. Error code:" << errorCode;
        
        if (errorCode == ERROR_ACCESS_DENIED) {
            qCritical() << "Access denied - ensure the helper has admin rights";
        } else if (errorCode == ERROR_FILE_NOT_FOUND) {
            qCritical() << "Device not found - invalid path:" << devicePath;
        } else if (errorCode == ERROR_SHARING_VIOLATION) {
            qCritical() << "Device is in use by another process";
        }
        
        sourceFileObj.close();
        return false;
    }
    
    m_bytesTotal = sourceFileObj.size();
    m_bytesWritten = 0;
    
    // Start progress timer
    m_progressTimer.start(100);
    
    // Buffer for reading/writing
    const int BUFFER_SIZE = 10 * 1024 * 1024; // 10MB buffer
    QByteArray buffer(BUFFER_SIZE, 0);
    DWORD bytesWritten;
    bool success = true;
    
    while (!sourceFileObj.atEnd() && success) {
        // Read chunk from source file
        qint64 bytesRead = sourceFileObj.read(buffer.data(), BUFFER_SIZE);
        if (bytesRead <= 0) {
            break;
        }
        
        // Write to device
        if (!WriteFile(deviceHandle, buffer.data(), bytesRead, &bytesWritten, NULL) || bytesRead != bytesWritten) {
            qCritical() << "Failed to write to device. Error code:" << GetLastError();
            success = false;
            break;
        }
        
        m_bytesWritten += bytesWritten;
        
        // Process events to allow progress reporting
        QCoreApplication::processEvents();
    }
    
    // Clean up
    m_progressTimer.stop();
    CloseHandle(deviceHandle);
    sourceFileObj.close();
    
    return success;
} 