#include "elevationhelper.h"
#include <QDir>
#include <QProcess>
#include <QDebug>
#include <QCoreApplication>
#include <QDataStream>
#include <QRandomGenerator>
#include <QUuid>

ElevationHelper::ElevationHelper(QObject *parent) : QObject(parent),
    m_socket(nullptr),
    m_helperProcess(NULL),
    m_operationComplete(false)
{
    m_socket = new QLocalSocket(this);
    connect(m_socket, &QLocalSocket::readyRead, this, &ElevationHelper::socketReadyRead);
    
    // Set up timer to process Windows events
    connect(&m_eventTimer, &QTimer::timeout, this, &ElevationHelper::processPendingEvents);
}

ElevationHelper::~ElevationHelper()
{
    if (m_socket) {
        if (m_socket->isOpen()) {
            m_socket->close();
        }
    }
    
    if (m_helperProcess != NULL) {
        CloseHandle(m_helperProcess);
    }
}

void ElevationHelper::socketReadyRead()
{
    QDataStream in(m_socket);
    in.setVersion(QDataStream::Qt_6_9);
    
    while (m_socket->bytesAvailable() >= (int)sizeof(int) * 3) {
        int progressType;
        qint64 now, total;
        
        in >> progressType >> now >> total;
        
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
    // Create a unique socket name based on process ID and timestamp
    return QString("rpi-imager-helper-%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

bool ElevationHelper::runFormatDrive(const QString &drive)
{
    m_socketName = generateSocketName();
    QString args = QString("--format \"%1\" --socket \"%2\"").arg(drive, m_socketName);
    return runHelperWithArgs(args);
}

bool ElevationHelper::runWriteToDrive(const QString &drive, const QString &sourceFile)
{
    qDebug() << "Preparing to write" << sourceFile << "to" << drive;
    m_socketName = generateSocketName();
    // Make sure to properly escape quotes in file paths
    QString safeSourceFile = sourceFile;
    safeSourceFile.replace("\"", "\\\"");
    QString args = QString("--write \"%1\" --source \"%2\" --socket \"%3\"").arg(drive, safeSourceFile, m_socketName);
    qDebug() << "Helper command arguments:" << args;
    return runHelperWithArgs(args);
}

bool ElevationHelper::runHelperWithArgs(const QString &args)
{
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
            m_socket->connectToServer(m_socketName);
            
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

void ElevationHelper::handleSocketError(QLocalSocket::LocalSocketError socketError)
{
    if (socketError != QLocalSocket::PeerClosedError) {
        qDebug() << "Socket error:" << m_socket->errorString();
    }
} 