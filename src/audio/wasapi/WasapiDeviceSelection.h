#ifndef WASAPIDEVICESELECTION_H
#define WASAPIDEVICESELECTION_H

#include <string>
#include <vector>

namespace wasapi {

struct DeviceInfo {
    std::string name;
    std::string id;
    bool isDefault = false;
};

enum class SelectionStatus { Resolved, Empty, Unavailable, Ambiguous };

struct DeviceSelection {
    SelectionStatus status;
    const DeviceInfo* device = nullptr;
};

// Only active render endpoints belong in devices. Names are exact metadata,
// not labels: never normalize them or use a name to override a surviving ID.
inline DeviceSelection resolveDeviceSelection(const std::vector<DeviceInfo>& devices,
                                             const std::string& savedId,
                                             const std::string& savedName)
{
    if (savedId.empty())
        return {SelectionStatus::Empty, nullptr};

    for (const auto& device : devices)
        if (device.id == savedId)
            return {SelectionStatus::Resolved, &device};

    const DeviceInfo* match = nullptr;
    if (!savedName.empty()) {
        for (const auto& device : devices) {
            if (device.name != savedName)
                continue;
            if (match)
                return {SelectionStatus::Ambiguous, nullptr};
            match = &device;
        }
    }
    return match ? DeviceSelection{SelectionStatus::Resolved, match}
                 : DeviceSelection{SelectionStatus::Unavailable, nullptr};
}

} // namespace wasapi

#endif // WASAPIDEVICESELECTION_H
