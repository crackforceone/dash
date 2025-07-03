#pragma once

#include <QObject>
#include "plugins/brightness_plugin.hpp"

class Serial : public QObject, BrightnessPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID BrightnessPlugin_iid FILE "serial.json")
    Q_INTERFACES(BrightnessPlugin)

   public:
    Serial();
    bool supported() override;
    uint8_t priority() override;
    void set(int brightness) override;

   private:
    QString devicePath;
};