#include "diskwriter_helper.h"
#include <QCoreApplication>
#include <QDebug>
#include <windows.h>
#include <QDir>
#include <QFile>
#include <QDateTime>
#include <QCommandLineParser>
#include <QThread>

bool isRunningAsAdmin()
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

QString getLastErrorAsString()
{
    DWORD error = GetLastError();
    if (error == 0) {
        return QString("No error");
    }
    
    LPWSTR bufPtr = NULL;
    DWORD result = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | 
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&bufPtr,
        0, NULL);
    
    if (result == 0 || !bufPtr) {
        return QString("Unknown error code: %1").arg(error);
    }
    
    QString message = QString::fromWCharArray(bufPtr);
    LocalFree(bufPtr);
    
    // Remove trailing whitespace and newlines
    message = message.trimmed();
    
    return QString("Error %1: %2").arg(error).arg(message);
}

int main(int argc, char *argv[])
{
    // Try to write to multiple possible log locations
    QString docPath = QDir::homePath() + "/Documents/rpi-imager-early-log.txt";
    QString tempPath = QDir::tempPath() + "/rpi-imager-early-log.txt";
    QString curDirPath = "./rpi-imager-early-log.txt";
    
    FILE* docFile = fopen(docPath.toUtf8().constData(), "a");
    if (docFile) {
        fprintf(docFile, "Early logging started in Documents folder\n");
        fprintf(docFile, "Command line:\n");
        for (int i = 0; i < argc; i++) {
            fprintf(docFile, "  arg[%d]: %s\n", i, argv[i]);
        }
        fclose(docFile);
    }
    
    FILE* tempFile = fopen(tempPath.toUtf8().constData(), "a");
    if (tempFile) {
        fprintf(tempFile, "Early logging started in Temp folder\n");
        fprintf(tempFile, "Command line:\n");
        for (int i = 0; i < argc; i++) {
            fprintf(tempFile, "  arg[%d]: %s\n", i, argv[i]);
        }
        fclose(tempFile);
    }
    
    FILE* curDirFile = fopen(curDirPath.toUtf8().constData(), "a");
    if (curDirFile) {
        fprintf(curDirFile, "Early logging started in current directory\n");
        fprintf(curDirFile, "Command line:\n");
        for (int i = 0; i < argc; i++) {
            fprintf(curDirFile, "  arg[%d]: %s\n", i, argv[i]);
        }
        fclose(curDirFile);
    }
    
    // Initialize Qt application
    QCoreApplication app(argc, argv);
    
    // Try Qt-based file writing
    QFile qtDocFile(docPath);
    if (qtDocFile.open(QIODevice::Append)) {
        qtDocFile.write("QFile log test in Documents folder\n");
        qtDocFile.close();
    }
    
    QFile qtTempFile(tempPath);
    if (qtTempFile.open(QIODevice::Append)) {
        qtTempFile.write("QFile log test in Temp folder\n");
        qtTempFile.close();
    }
    
    QFile qtCurDirFile(curDirPath);
    if (qtCurDirFile.open(QIODevice::Append)) {
        qtCurDirFile.write("QFile log test in current directory\n");
        qtCurDirFile.close();
    }
    
    // Check admin privileges
    bool isAdmin = isRunningAsAdmin();
    
    // Create a signal file to indicate we're running
    QFile signalFile(QDir::homePath() + "/Documents/rpi-imager-helper-running.txt");
    if (signalFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        signalFile.write("Helper application diagnostic run\n");
        signalFile.write(QString("Admin privileges: %1\n").arg(isAdmin ? "YES" : "NO").toUtf8());
        signalFile.write(QString("Process ID: %1\n").arg(QCoreApplication::applicationPid()).toUtf8());
        signalFile.close();
    }
    
    // Create helper instance
    DiskWriterHelper helper;

    // Process command line arguments
    QCommandLineParser parser;
    parser.setApplicationDescription("Raspberry Pi Imager Disk Writer Helper");
    parser.addHelpOption();

    // Check for daemon mode
    QCommandLineOption daemonOption(QStringList() << "daemon", "Run in daemon mode, listening for commands");
    parser.addOption(daemonOption);

    // Process command line
    parser.process(app);

    if (parser.isSet(daemonOption)) {
        qDebug() << "Starting in daemon mode";
        
        // Log to the signal file that we're entering daemon mode
        QFile daemonLogFile(QDir::homePath() + "/Documents/rpi-imager-helper-daemon.txt");
        if (daemonLogFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            daemonLogFile.write("Entering daemon mode\n");
            daemonLogFile.write(QString("Timestamp: %1\n").arg(QDateTime::currentDateTime().toString()).toUtf8());
            daemonLogFile.close();
        }
        
        // Execute the helper in daemon mode and start event loop
        int result = helper.executeFromCommandLine(app.arguments());
        if (result != 0) {
            qCritical() << "Failed to start daemon mode, error code:" << result;
            return result;
        }
        
        qDebug() << "Daemon mode setup complete, entering event loop";
        
        // Log that we're entering the event loop
        if (daemonLogFile.open(QIODevice::Append)) {
            daemonLogFile.write("Entering event loop\n");
            daemonLogFile.write(QString("Timestamp: %1\n").arg(QDateTime::currentDateTime().toString()).toUtf8());
            daemonLogFile.close();
        }
        
        // If daemon mode is successful, enter the event loop to keep the application running
        int execResult = app.exec();
        
        // This code will only be reached when the event loop exits
        qDebug() << "Event loop exited with code:" << execResult;
        return execResult;
    } else {
        // Just execute the command and exit
        return helper.executeFromCommandLine(app.arguments());
    }
} 