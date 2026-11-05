#pragma once
#include <string>

// Options > Pair Wii Remotes: pairs real Wii Remotes with the PC over Bluetooth, like
// Dolphin's "Pair" button. Press the red sync button on each remote first. Paired
// remotes stay paired: later, any button on the remote reconnects it.
//   Windows: pairs through Windows' Bluetooth API.
//   Linux:   pairs through BlueZ's bluetoothctl.
struct WiimotePairResult {
    int paired = 0;     // remotes paired (or reconnected) this time
    std::string error;  // why pairing couldn't run at all (empty if it ran)
};

// Takes about 10-15 seconds (it searches for remotes a few times): run it on a thread.
WiimotePairResult pairWiiRemotes();
