#pragma once

#include <string>

/*! \brief Drops the Bluetooth connection to the device with \a address ("XX:XX:XX:XX:XX:XX").

    A Move has no power button: like on a PlayStation, dropping its connection switches it off, and its pairing is
    kept, so pressing PS connects it again. Closing its HID handle isn't enough, the link stays up.

    Only implemented on Windows; returns false elsewhere.
 */
bool disconnectBluetoothDevice(const std::string &address);
