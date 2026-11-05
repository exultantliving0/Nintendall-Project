#include "wiimotepair.hpp"

#include <set>

#if defined(_WIN32)

#include <windows.h>
#include <bluetoothapis.h>

#include <cwchar>
#include <type_traits>
#include <vector>

// The Bluetooth functions live in bthprops.cpl; it's loaded at run time so Nintendall still
// starts on PCs where it's missing.
namespace {

struct Bt {
    decltype(&BluetoothFindFirstRadio) findFirstRadio = nullptr;
    decltype(&BluetoothFindNextRadio) findNextRadio = nullptr;
    decltype(&BluetoothFindRadioClose) findRadioClose = nullptr;
    decltype(&BluetoothGetRadioInfo) getRadioInfo = nullptr;
    decltype(&BluetoothFindFirstDevice) findFirstDevice = nullptr;
    decltype(&BluetoothFindNextDevice) findNextDevice = nullptr;
    decltype(&BluetoothFindDeviceClose) findDeviceClose = nullptr;
    decltype(&BluetoothAuthenticateDevice) authenticate = nullptr;
    decltype(&BluetoothEnumerateInstalledServices) enumerateServices = nullptr;
    decltype(&BluetoothSetServiceState) setServiceState = nullptr;
    decltype(&BluetoothRemoveDevice) removeDevice = nullptr;

    bool load() {
        HMODULE lib = LoadLibraryW(L"bthprops.cpl");
        if (!lib) return false;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(reinterpret_cast<void*>(GetProcAddress(lib, name)));
            return fn != nullptr;
        };
        return get(findFirstRadio, "BluetoothFindFirstRadio") && get(findNextRadio, "BluetoothFindNextRadio") &&
               get(findRadioClose, "BluetoothFindRadioClose") && get(getRadioInfo, "BluetoothGetRadioInfo") &&
               get(findFirstDevice, "BluetoothFindFirstDevice") && get(findNextDevice, "BluetoothFindNextDevice") &&
               get(findDeviceClose, "BluetoothFindDeviceClose") && get(authenticate, "BluetoothAuthenticateDevice") &&
               get(enumerateServices, "BluetoothEnumerateInstalledServices") &&
               get(setServiceState, "BluetoothSetServiceState") && get(removeDevice, "BluetoothRemoveDevice");
    }
};

// Bluetooth HID service, 00001124-0000-1000-8000-00805F9B34FB.
const GUID kHidService = {0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

bool isWiiRemote(const BLUETOOTH_DEVICE_INFO& d) { return wcsncmp(d.szName, L"Nintendo RVL-CNT", 16) == 0; }

struct Radio {
    HANDLE handle;
    BLUETOOTH_RADIO_INFO info;
};

// Calls f(device) for every Bluetooth device a radio knows; `inquiry` also searches for new ones
// (multiplier x 1.28 seconds).
template <class F>
void forEachDevice(const Bt& bt, const Radio& radio, UCHAR inquiry, F f) {
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
    search.dwSize = sizeof search;
    search.fReturnAuthenticated = search.fReturnRemembered = search.fReturnUnknown = search.fReturnConnected = TRUE;
    search.fIssueInquiry = inquiry > 0;
    search.cTimeoutMultiplier = inquiry;
    search.hRadio = radio.handle;
    BLUETOOTH_DEVICE_INFO device{};
    device.dwSize = sizeof device;
    HBLUETOOTH_DEVICE_FIND find = bt.findFirstDevice(&search, &device);
    if (!find) return;
    do {
        f(device);
        device.dwSize = sizeof device;
    } while (bt.findNextDevice(find, &device));
    bt.findDeviceClose(find);
}

}  // namespace

WiimotePairResult pairWiiRemotes() {
    WiimotePairResult result;
    Bt bt;
    if (!bt.load()) {
        result.error = "Windows' Bluetooth support couldn't be loaded";
        return result;
    }

    std::vector<Radio> radios;
    BLUETOOTH_FIND_RADIO_PARAMS radioParams{sizeof radioParams};
    HANDLE handle = nullptr;
    if (HBLUETOOTH_RADIO_FIND find = bt.findFirstRadio(&radioParams, &handle)) {
        do {
            Radio r{handle, {}};
            r.info.dwSize = sizeof r.info;
            if (bt.getRadioInfo(handle, &r.info) == ERROR_SUCCESS) radios.push_back(r);
            else CloseHandle(handle);
        } while (bt.findNextRadio(find, &handle));
        bt.findRadioClose(find);
    }
    if (radios.empty()) {
        result.error = "No Bluetooth found on this PC (or it's turned off)";
        return result;
    }

    // Windows keeps remotes that were connected but never paired, and then can't reconnect them:
    // forget those so they can be paired properly.
    for (const Radio& r : radios)
        forEachDevice(bt, r, 0, [&](const BLUETOOTH_DEVICE_INFO& d) {
            if (isWiiRemote(d) && d.fRemembered && !d.fConnected && !d.fAuthenticated) bt.removeDevice(&d.Address);
        });

    // Search a few times (Windows often misses a remote on the first try) and pair what's found.
    std::set<ULONGLONG> paired;
    for (int round = 0; round < 3; ++round)
        for (const Radio& r : radios)
            forEachDevice(bt, r, 3, [&](BLUETOOTH_DEVICE_INFO& d) {
                if (!isWiiRemote(d) || d.fConnected) return;
                if (!d.fAuthenticated) {
                    // After the red sync button, a remote expects the PC's Bluetooth address as the
                    // PIN (its 6 bytes as they're stored, one per character). It then remembers the PC.
                    WCHAR pin[6];
                    for (int i = 0; i < 6; ++i) pin[i] = WCHAR(r.info.address.rgBytes[i]);
                    if (bt.authenticate(nullptr, r.handle, &d, pin, 6) != ERROR_SUCCESS) return;
                    DWORD services = 0;  // makes Windows keep the pairing
                    bt.enumerateServices(r.handle, &d, &services, nullptr);
                }
                if (bt.setServiceState(r.handle, &d, &kHidService, BLUETOOTH_SERVICE_ENABLE) == ERROR_SUCCESS)
                    paired.insert(d.Address.ullLong);
            });

    for (const Radio& r : radios) CloseHandle(r.handle);
    result.paired = int(paired.size());
    return result;
}

#elif defined(__APPLE__)

WiimotePairResult pairWiiRemotes() {
    WiimotePairResult result;
    result.error = "On a Mac, pair the remote in System Settings > Bluetooth";
    return result;
}

#else  // Linux: BlueZ's bluetoothctl

#include <cctype>
#include <cstdio>
#include <sstream>

namespace {

std::string run(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen((cmd + " 2>&1").c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
    }
    return out;
}

bool isAddress(const std::string& s) {
    if (s.size() != 17) return false;
    for (size_t i = 0; i < s.size(); ++i)
        if (i % 3 == 2 ? s[i] != ':' : !std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    return true;
}

// Addresses in bluetoothctl output lines like "[NEW] Device 00:1F:32:AA:BB:CC Nintendo RVL-CNT-01";
// with wiiOnly, just the ones named like a Wii Remote.
std::set<std::string> addresses(const std::string& text, bool wiiOnly) {
    std::set<std::string> out;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const size_t at = line.find("Device ");
        if (at == std::string::npos) continue;
        const std::string addr = line.substr(at + 7, 17);
        if (isAddress(addr) && (!wiiOnly || line.find("Nintendo RVL-CNT", at) != std::string::npos)) out.insert(addr);
    }
    return out;
}

}  // namespace

WiimotePairResult pairWiiRemotes() {
    WiimotePairResult result;
    if (run("command -v bluetoothctl").empty()) {
        result.error = "Pairing needs BlueZ (bluetoothctl) - install it, or pair in your Bluetooth settings";
        return result;
    }
    const std::string show = run("bluetoothctl show");
    if (show.find("Controller") == std::string::npos) {
        result.error = "No Bluetooth found on this PC";
        return result;
    }
    if (show.find("Powered: yes") == std::string::npos) run("bluetoothctl power on");

    // Search for remotes that are waiting to pair (red sync button), then pair, trust and connect them.
    // BlueZ works out a Wii Remote's PIN by itself. Trusted remotes reconnect with any button later.
    // Only remotes heard during this search (not every remote BlueZ ever saw, which may be off).
    const std::set<std::string> seen = addresses(run("bluetoothctl --timeout 8 scan on"), false);
    for (const std::string& addr : addresses(run("bluetoothctl devices"), true)) {
        if (!seen.count(addr)) continue;  // addresses are checked to be XX:XX:..., safe in a command
        if (run("bluetoothctl info " + addr).find("Connected: yes") != std::string::npos) continue;
        run("timeout 20 bluetoothctl pair " + addr);
        run("bluetoothctl trust " + addr);
        run("timeout 15 bluetoothctl connect " + addr);
        if (run("bluetoothctl info " + addr).find("Connected: yes") != std::string::npos) ++result.paired;
    }
    return result;
}

#endif
