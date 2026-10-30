#include "BluetoothLink.h"

#include <windows.h>
#include <bluetoothapis.h>
#include <winioctl.h> // CTL_CODE, FILE_DEVICE_BLUETOOTH (left out by WIN32_LEAN_AND_MEAN)
#include <bthioctl.h>

#include <cctype>

bool disconnectBluetoothDevice(const std::string &address)
{
    // BTH_ADDR holds the address with its first displayed byte as the most significant one.
    BTH_ADDR bthAddress = 0;
    int digits = 0;
    for (char c : address) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            continue;
        const int value = std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::tolower(c) - 'a' + 10;
        bthAddress = (bthAddress << 4) | static_cast<BTH_ADDR>(value);
        ++digits;
    }
    if (digits != 12)
        return false;

    BLUETOOTH_FIND_RADIO_PARAMS params = {sizeof(params)};
    HANDLE radio = nullptr;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find)
        return false;
    BluetoothFindRadioClose(find);

    // Undocumented in the Win32 API docs, but declared in the SDK's bthioctl.h (and used by e.g. DS4Windows and
    // wiimosc). Needs no Administrator rights.
    DWORD bytesReturned = 0;
    const bool isDisconnected = DeviceIoControl(radio, IOCTL_BTH_DISCONNECT_DEVICE, &bthAddress, sizeof(bthAddress),
                                                nullptr, 0, &bytesReturned, nullptr) != 0;
    CloseHandle(radio);
    return isDisconnected;
}
