#include <QProcess>
#include <QRegularExpression>
#include <QDebug>

#include "ddcutil.hpp"

DDCUtil::DDCUtil()
{
    // Detect available DDC buses when constructing
    detectBuses();
}

void DDCUtil::detectBuses()
{
    detectedBuses.clear();
    
    // Run ddcutil detect to discover available buses
    QProcess process(this);
    process.start(QString("ddcutil detect"));
    process.waitForFinished();
    
    if (process.exitCode() == 0) {
        QString output = process.readAllStandardOutput();
        
        // Parse output to find buses
        // Example line: "I2C bus:  /dev/i2c-3"
        QRegularExpression busRegex("I2C bus:\\s+/dev/i2c-(\\d+)");
        QRegularExpressionMatchIterator matches = busRegex.globalMatch(output);
        
        while (matches.hasNext()) {
            QRegularExpressionMatch match = matches.next();
            int busNumber = match.captured(1).toInt();
            detectedBuses[busNumber] = QString("/dev/i2c-%1").arg(busNumber);
        }
    }
}

bool DDCUtil::supported()
{
    // Check that ddcutil is installed
    QProcess process(this);
    process.start(QString("ddcutil --version"));
    process.waitForFinished();
    
    // Only report as supported if ddcutil is installed and we found at least one bus
    return (process.exitCode() == 0) && !detectedBuses.isEmpty();
}

uint8_t DDCUtil::priority()
{
    // Higher priority than xrandr (2)
    return 3;
}

void DDCUtil::set(int brightness)
{
    // The slider's range appears to be 76-255 (based on Arbiter::decrease_brightness)
    // We want to map this to DDC range 0-100
    
    int ddcBrightness = 0;
    
    // Check if the brightness value is coming from the UI's constrained range (76-255)
    if (brightness <= 76) {
        // If brightness is at or below the hard-coded minimum, set it to 0
        ddcBrightness = 0;
    } else {
        // Linear mapping from input range (76-255) to output range (0-100)
        ddcBrightness = qRound((brightness - 76) * 100.0 / (255.0 - 76.0));
    }
    
    // Ensure we stay within bounds
    ddcBrightness = qBound(MIN_BRIGHTNESS, ddcBrightness, MAX_BRIGHTNESS);
    
    qDebug() << "Setting DDC brightness to" << ddcBrightness << "from input value" << brightness;
    
    // Apply to all detected buses
    for (auto busIt = detectedBuses.constBegin(); busIt != detectedBuses.constEnd(); ++busIt) {
        int busNumber = busIt.key();
        QProcess process(this);
        
        QString command = QString("ddcutil setvcp 10 %1 --bus %2").arg(ddcBrightness).arg(busNumber);
        qDebug() << "Executing:" << command;
        
        process.start(command);
        process.waitForFinished();
        
        if (process.exitCode() != 0) {
            qWarning() << "Failed to set brightness on bus" << busNumber << ":" << process.readAllStandardError();
        }
    }
}