#include "BluetoothLink.h"

// Not implemented yet. wiimosc has implementations to port: macOS -[IOBluetoothDevice closeConnection], Linux
// org.bluez.Device1.Disconnect over D-Bus.
bool disconnectBluetoothDevice(const std::string &)
{
    return false;
}
