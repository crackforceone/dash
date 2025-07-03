#include <QFileInfo>
#include <QFile>
#include <QTextStream>
#include <QDebug>

#include "serial.hpp"

Serial::Serial()
{
    this->devicePath = "/dev/ttyACM0";
}

bool Serial::supported()
{
    QFileInfo deviceInfo(this->devicePath);
    
    // Check if device exists and is writable
    if (deviceInfo.exists() && deviceInfo.isWritable()) {
        return true;
    }
    
    return false;
}

uint8_t Serial::priority()
{
    // Higher priority than X plugin since this is direct hardware control
    return 3;
}

void Serial::set(int brightness)
{
    // Clamp brightness to valid range (5-255)
    // If input is 0-255 range, map it to 5-255
    int clampedBrightness;
    if (brightness < 5) {
        clampedBrightness = 5;
    } else if (brightness > 255) {
        clampedBrightness = 255;
    } else {
        clampedBrightness = brightness;
    }
    
    QFile device(this->devicePath);
    if (device.open(QIODevice::WriteOnly)) {
        QTextStream stream(&device);
        QString command = QString("@B%1#").arg(clampedBrightness, 3, 10, QChar('0'));
        stream << command;
        stream.flush();
        device.close();
    } else {
        qWarning() << "Failed to open" << this->devicePath << "for writing";
    }
}