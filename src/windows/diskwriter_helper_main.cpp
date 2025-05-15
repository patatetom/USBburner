#include "diskwriter_helper.h"
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("Raspberry Pi Imager Disk Writer Helper");
    QCoreApplication::setApplicationVersion("1.0");
    
    DiskWriterHelper helper;
    int result = helper.executeFromCommandLine(QCoreApplication::arguments());
    
    // Ensure all events are processed before exiting
    QCoreApplication::processEvents();
    
    return result;
} 