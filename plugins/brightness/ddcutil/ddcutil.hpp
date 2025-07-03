#pragma once

#include <QObject>
#include <QMap>
#include "plugins/brightness_plugin.hpp"

class DDCUtil : public QObject, BrightnessPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID BrightnessPlugin_iid FILE "ddcutil.json")
    Q_INTERFACES(BrightnessPlugin)

   public:
    DDCUtil();
    bool supported() override;
    uint8_t priority() override;
    void set(int brightness) override;
    
    // Define min and max brightness values
    static constexpr int MIN_BRIGHTNESS = 0;  // Allow full darkness
    static constexpr int MAX_BRIGHTNESS = 100;

   private:
    // Store available DDC buses
    QMap<int, QString> detectedBuses;
    
    // Detect available DDC buses
    void detectBuses();
};