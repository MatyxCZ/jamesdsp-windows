#ifndef WASAPIDEVICE_H
#define WASAPIDEVICE_H

#include "IOutputDevice.h"
#include <string>

class WasapiDevice : public IOutputDevice
{
public:
    WasapiDevice() = default;
    WasapiDevice(std::string name, std::string description, std::string id)
        : IOutputDevice(name, description), deviceId(id) {}

    std::string deviceId;
};

#endif // WASAPIDEVICE_H