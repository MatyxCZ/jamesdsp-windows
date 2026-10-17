#include "../src/audio/wasapi/WasapiDeviceSelection.h"

#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* description)
{
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}
}

int main()
{
    using wasapi::DeviceInfo;
    using wasapi::SelectionStatus;
    using wasapi::resolveDeviceSelection;

    const std::vector<DeviceInfo> active = {
        {"CABLE Input (VB-Audio Virtual Cable)", "cable-id", true},
        {"Reproduktory (High Definition Audio Device)", "new-speaker-id", false},
        {"Headphones", "headphone-id", false}
    };
    auto result = resolveDeviceSelection(active, "", "");
    expect(result.status == SelectionStatus::Empty && !result.device,
           "an empty selection must not follow the default endpoint");
    result = resolveDeviceSelection(active, "", active[0].name);
    expect(result.status == SelectionStatus::Empty && !result.device,
           "a friendly name alone must not invent an explicit saved selection");
    result = resolveDeviceSelection({}, "missing-id", "Missing device");
    expect(result.status == SelectionStatus::Unavailable && !result.device,
           "no active endpoints must fail without fallback");
    result = resolveDeviceSelection(active, "missing-id", "Missing device");
    expect(result.status == SelectionStatus::Unavailable && !result.device,
           "a missing ID and name must not select an arbitrary endpoint");
    result = resolveDeviceSelection(active, "missing-id", "");
    expect(result.status == SelectionStatus::Unavailable && !result.device,
           "a stale ID without metadata must require explicit selection");
    result = resolveDeviceSelection(active, "cable-id", "");
    expect(result.status == SelectionStatus::Resolved && result.device == &active[0],
           "an active exact ID must work with legacy missing name metadata");
    result = resolveDeviceSelection(active, "headphone-id", active[0].name);
    expect(result.status == SelectionStatus::Resolved && result.device == &active[2],
           "an active exact ID must take precedence over a different saved name");
    result = resolveDeviceSelection(active, "old-speaker-id", active[1].name);
    expect(result.status == SelectionStatus::Resolved && result.device == &active[1],
           "a reboot-stale ID must recover the unique exact saved friendly name");
    result = resolveDeviceSelection(active, "missing-id", "headphones");
    expect(result.status == SelectionStatus::Unavailable && !result.device,
           "friendly-name recovery must be case sensitive");
    result = resolveDeviceSelection(active, "missing-id", "Headphones ");
    expect(result.status == SelectionStatus::Unavailable && !result.device,
           "friendly-name recovery must not trim or normalize metadata");

    auto duplicates = active;
    duplicates.push_back({active[1].name, "second-speaker-id", false});
    result = resolveDeviceSelection(duplicates, "stale-speaker-id", active[1].name);
    expect(result.status == SelectionStatus::Ambiguous && !result.device,
           "duplicate active friendly names must never pick the first match");
    result = resolveDeviceSelection(duplicates, "second-speaker-id", active[1].name);
    expect(result.status == SelectionStatus::Resolved && result.device == &duplicates[3],
           "an active exact ID must win even when its friendly name is ambiguous");
    result = resolveDeviceSelection(duplicates, "cable-id", active[1].name);
    expect(result.status == SelectionStatus::Resolved && result.device == &duplicates[0],
           "an active exact ID must win over ambiguous stale name metadata");

    const std::vector<DeviceInfo> unicode = {{"Reproduktory \xC4\x8D", "unicode-id", false}};
    result = resolveDeviceSelection(unicode, "stale-id", unicode[0].name);
    expect(result.status == SelectionStatus::Resolved && result.device == &unicode[0] &&
           result.device->name == unicode[0].name,
           "UTF-8 friendly names must remain byte-exact metadata");
    return failures == 0 ? 0 : 1;
}
