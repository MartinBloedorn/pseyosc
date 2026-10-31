#include "MoveHub.h"
#include "BluetoothLink.h"

#include <psmove.h>
#include <psmove_port.h>
#include <psmove_private.h>
#include <hidapi.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

const auto sFinderInterval = std::chrono::milliseconds(500);
//! A connected controller sends input reports continuously; silence this long means it's gone.
const auto sReportTimeout = std::chrono::seconds(3);
const auto sRumbleDuration = std::chrono::milliseconds(300);

struct Color {
    unsigned char r, g, b;
};

//! Orb colors by id; dimmed, since the orb is bright and drains the battery.
const Color sIdColors[MoveHub::sMaxDevices] = {
    {64, 0, 0}, {0, 64, 0}, {0, 0, 64}, {64, 48, 0}, {64, 0, 64}, {0, 64, 64}, {48, 48, 48}, {64, 16, 0}};

struct HidController {
    std::string path;
    std::wstring serial;
    unsigned short productId;
};

/*! \brief Lists the Move controllers present as HID devices, over USB or Bluetooth.

    On Windows each controller appears as three HID collections; psmoveapi opens the one with "&col01#" in its path
    (and derives the others from it), so only that one is listed.
 */
std::vector<HidController> listControllers()
{
    std::vector<HidController> controllers;
    for (unsigned short productId : {PSMOVE_PID, PSMOVE_PS4_PID}) {
        hid_device_info *devices = hid_enumerate(PSMOVE_VID, productId);
        for (hid_device_info *device = devices; device; device = device->next) {
#ifdef _WIN32
            if (!std::strstr(device->path, "&col01#"))
                continue;
#endif
            controllers.push_back({device->path, device->serial_number ? device->serial_number : L"", productId});
        }
        hid_free_enumeration(devices);
    }
    return controllers;
}

std::string takeString(char *text)
{
    std::string result = text ? text : "";
    psmove_free_mem(text);
    return result;
}

} // namespace

MoveHub::MoveHub(AbstractHubListener *listener)
    : mListener(listener)
{
}

MoveHub::~MoveHub()
{
    stop();
}

std::string MoveHub::normalizeAddress(const std::string &address)
{
    std::string digits;
    for (char c : address) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            digits += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        else if (c != ':' && c != '-')
            return {};
    }
    if (digits.size() != 12)
        return {};

    std::string out;
    for (size_t i = 0; i < digits.size(); i += 2) {
        if (!out.empty())
            out += ':';
        out += digits.substr(i, 2);
    }
    return out;
}

bool MoveHub::parseFixedId(const std::string &argument, std::string *error)
{
    const size_t separator = argument.find('=');
    const std::string idText = argument.substr(0, separator);
    if (separator == std::string::npos || idText.empty()
        || !std::all_of(idText.begin(), idText.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
        *error = "expected <id>=<address>, e.g. 0=00:06:F7:12:34:56, got \"" + argument + "\"";
        return false;
    }
    return setFixedId(std::stoi(idText), argument.substr(separator + 1), error);
}

bool MoveHub::setFixedId(int id, const std::string &address, std::string *error)
{
    const std::string normalized = normalizeAddress(address);
    if (normalized.empty()) {
        *error = "invalid Bluetooth address \"" + address + "\"";
        return false;
    }
    if (id < 0 || id >= sMaxDevices) {
        *error = "id " + std::to_string(id) + " out of range (0-" + std::to_string(sMaxDevices - 1) + ")";
        return false;
    }
    for (const auto &fixed : mFixedIds) {
        if (fixed.first == normalized) {
            *error = normalized + " has more than one id";
            return false;
        }
        if (fixed.second == id) {
            *error = "id " + std::to_string(id) + " is assigned more than once";
            return false;
        }
    }
    mFixedIds[normalized] = id;
    return true;
}

bool MoveHub::start()
{
    if (mIsRunning)
        return true;

    // Remote controllers served by psmoveapi's "moved" daemon aren't used.
    psmove_set_remote_config(PSMove_OnlyLocal);
    hid_init();

    mHostAddress = normalizeAddress(takeString(psmove_port_get_host_bluetooth_address()));
    if (mHostAddress.empty()) {
        std::fprintf(stderr, "no Bluetooth radio found, is Bluetooth switched on?\n");
        return false;
    }
    std::printf("Bluetooth radio %s\n", mHostAddress.c_str());

    // Prints why if it can't (e.g. "This program must be run as Administrator.").
    mCanPair = psmove_port_check_pairing_permissions() != 0;
    if (!mCanPair)
        std::printf("controllers on USB won't be paired, only controllers paired before are picked up\n");

    mIsRunning = true;
    mFinderThread = std::thread(&MoveHub::runFinder, this);
    return true;
}

void MoveHub::stop()
{
    if (mIsRunning)
        printConnectedDevices();

    mIsRunning = false;
    if (mFinderThread.joinable())
        mFinderThread.join();
    for (std::thread &thread : mDeviceThreads) {
        if (thread.joinable())
            thread.join();
    }
}

void MoveHub::runFinder()
{
    while (mIsRunning) {
        const std::vector<HidController> controllers = listControllers();

        // Forget unplugged USB controllers, so they're handled again when plugged back in.
        for (auto it = mUsbPaths.begin(); it != mUsbPaths.end();) {
            const bool isPresent = std::any_of(controllers.begin(), controllers.end(),
                                               [&](const HidController &c) { return c.path == *it; });
            it = isPresent ? std::next(it) : mUsbPaths.erase(it);
        }

        for (const HidController &controller : controllers) {
            if (mUsbPaths.count(controller.path))
                continue;
            {
                std::lock_guard<std::mutex> lock(mMutex);
                if (std::any_of(std::begin(mSlots), std::end(mSlots),
                                [&](const Slot &slot) { return slot.isBusy && slot.path == controller.path; }))
                    continue;
            }

            // Also fails for Bluetooth controllers that disconnected recently but are still listed (Windows).
            PSMove *move = psmove_connect_internal(controller.serial.c_str(), controller.path.c_str(), 0,
                                                   controller.productId);
            if (!move)
                continue;
            if (psmove_connection_type(move) == Conn_USB) {
                mUsbPaths.insert(controller.path);
                handleUsbController(move);
            } else {
                startDevice(move, controller.path);
            }
        }

        const Clock::time_point next = Clock::now() + sFinderInterval;
        while (mIsRunning && Clock::now() < next)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

void MoveHub::handleUsbController(PSMove *move)
{
    const std::string address = normalizeAddress(takeString(psmove_get_serial(move)));
    PSMove_Data_BTAddr host = {};
    if (address.empty() || !_psmove_read_btaddrs(move, &host, nullptr)) {
        std::printf("[usb] Motion Controller: couldn't read its Bluetooth addresses\n");
        psmove_disconnect(move);
        return;
    }
    const std::string hostAddress = normalizeAddress(takeString(_psmove_btaddr_to_string(host)));
    const char *label = psmove_get_model(move) == Model_ZCM2 ? "Motion Controller (ZCM2)" : "Motion Controller";

    if (hostAddress == mHostAddress && !mIsRepairing) {
        std::printf("[usb] %s %s is paired with this computer: unplug it and press PS to connect\n", label,
                    address.c_str());
        psmove_disconnect(move);
        return;
    }
    if (!mCanPair) {
        std::printf("[usb] %s %s is paired with %s, not this computer; run as Administrator to pair it\n", label,
                    address.c_str(), hostAddress.c_str());
        psmove_disconnect(move);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mPairing->mutex);
        if (!mPairing->addresses.insert(address).second) {
            std::printf("[usb] %s %s: pairing still waits for it: unplug it and press PS\n", label, address.c_str());
            psmove_disconnect(move);
            return;
        }
    }

    std::printf("[usb] %s %s: pairing (was paired with %s)\n", label, address.c_str(), hostAddress.c_str());
    // psmove_pair() writes the host address over USB and registers the controller with the OS. On Windows it then
    // waits, without a timeout, until the controller has connected over Bluetooth; so it runs detached, and isn't
    // waited for when quitting.
    std::shared_ptr<PairingState> pairing = mPairing;
    std::thread([pairing, move, address, label]() {
        const bool isPaired = psmove_pair(move);
        psmove_disconnect(move);
        std::printf("[usb] %s %s: %s\n", label, address.c_str(), isPaired ? "paired" : "pairing failed");
        std::lock_guard<std::mutex> lock(pairing->mutex);
        pairing->addresses.erase(address);
    }).detach();
}

void MoveHub::startDevice(PSMove *move, const std::string &path)
{
    const std::string address = normalizeAddress(takeString(psmove_get_serial(move)));

    std::lock_guard<std::mutex> lock(mMutex);
    int index = 0;
    while (index < sMaxDevices && mSlots[index].isBusy)
        ++index;
    if (index == sMaxDevices) {
        std::printf("too many controllers, ignoring %s\n", address.c_str());
        psmove_disconnect(move);
        return;
    }
    if (mDeviceThreads[index].joinable())
        mDeviceThreads[index].join(); // a finished thread that left this slot

    Slot &slot = mSlots[index];
    slot.isBusy = true;
    slot.id = chooseId(address);
    slot.address = address;
    slot.path = path;
    mDeviceThreads[index] = std::thread(&MoveHub::runDevice, this, index, move);
}

void MoveHub::runDevice(int index, PSMove *move)
{
    int id;
    std::string address;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        id = mSlots[index].id;
        address = mSlots[index].address;
    }
    const char *label = psmove_get_model(move) == Model_ZCM2 ? "Motion Controller (ZCM2)" : "Motion Controller";
    std::printf("[%d] %s %s connected\n", id, label, address.c_str());

    // The connection rumble ends before the listener takes over, so it can't cut short a rumble the listener sets
    // (psmoveapi can't tell the motor's state). Input reports queue up meanwhile.
    const Color color = sIdColors[id];
    {
        std::lock_guard<std::mutex> lock(*mOutputMutex);
        psmove_set_leds(move, color.r, color.g, color.b);
        psmove_set_rumble(move, 160);
        psmove_update_leds(move);
    }
    std::this_thread::sleep_for(sRumbleDuration);
    {
        std::lock_guard<std::mutex> lock(*mOutputMutex);
        psmove_set_rumble(move, 0);
        psmove_update_leds(move);
    }
    if (mListener)
        mListener->connected(id, move, address);

    Clock::time_point lastReport = Clock::now();
    Clock::time_point lastActivity = Clock::now();
    unsigned int lastButtons = 0;
    unsigned char lastTrigger = 0;
    while (mIsRunning) {
        const Clock::time_point now = Clock::now();
        const bool hasReport = psmove_poll(move) != 0;
        if (hasReport) {
            lastReport = now;
            if (mListener)
                mListener->update(id, move);

            const unsigned int buttons = psmove_get_buttons(move);
            const unsigned char trigger = psmove_get_trigger(move);
            if (buttons != lastButtons || trigger != lastTrigger) {
                lastActivity = now;
                lastButtons = buttons;
                lastTrigger = trigger;
            }
        } else if (now - lastReport > sReportTimeout) {
            break;
        }

        if (mIdleTimeout.count() > 0 && now - lastActivity >= mIdleTimeout) {
            // Like a PlayStation does: drop the connection, which switches the controller off. On failure, try
            // again after another timeout.
            std::printf("[%d] %s %s idle for %g min, shutting down\n", id, label, address.c_str(),
                        std::chrono::duration<double, std::ratio<60>>(mIdleTimeout).count());
            if (disconnectBluetoothDevice(address))
                break;
            std::printf("[%d] %s %s: shutting down failed\n", id, label, address.c_str());
            lastActivity = now;
        }

        // Sends changed LEDs/rumble (also the listener's), and resends them every few seconds: the controller switches
        // them off otherwise. Skipped while someone else uses the output (e.g. the tracker, calibrating).
        {
            std::unique_lock<std::mutex> outputLock(*mOutputMutex, std::try_to_lock);
            if (outputLock.owns_lock()) {
                if (mListener)
                    mListener->updateOutput(id, move);
                if (psmove_update_leds(move) == Update_Failed)
                    break;
            }
        }

        if (!hasReport)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!mIsRunning) {
        {
            std::lock_guard<std::mutex> lock(*mOutputMutex);
            psmove_set_leds(move, 0, 0, 0);
            psmove_set_rumble(move, 0);
            psmove_update_leds(move);
        }
        if (mIsShuttingDownOnExit) {
            if (disconnectBluetoothDevice(address)) {
                std::printf("[%d] %s %s: shutting down\n", id, label, address.c_str());
            } else {
                std::printf("[%d] %s %s: shutting down failed\n", id, label, address.c_str());
            }
        }
    }
    // The listener first: it may still use the handle (the tracker does) until disconnected() returns.
    if (mListener)
        mListener->disconnected(id);
    psmove_disconnect(move);
    std::printf("[%d] %s %s disconnected\n", id, label, address.c_str());

    std::lock_guard<std::mutex> lock(mMutex);
    mSlots[index] = Slot();
}

int MoveHub::chooseId(const std::string &address) const
{
    auto isUsed = [this](int id) {
        return std::any_of(std::begin(mSlots), std::end(mSlots),
                           [id](const Slot &slot) { return slot.isBusy && slot.id == id; });
    };
    auto isReserved = [this](int id) {
        return std::any_of(mFixedIds.begin(), mFixedIds.end(), [id](const auto &fixed) { return fixed.second == id; });
    };

    const auto fixed = mFixedIds.find(address);
    if (fixed != mFixedIds.end() && !isUsed(fixed->second))
        return fixed->second;
    // Keep ids of fixed controllers free while they're away, unless all others are in use.
    for (int id = 0; id < sMaxDevices; ++id) {
        if (!isUsed(id) && !isReserved(id))
            return id;
    }
    for (int id = 0; id < sMaxDevices; ++id) {
        if (!isUsed(id))
            return id;
    }
    return -1; // unreachable: there are as many ids as slots
}

void MoveHub::printConnectedDevices()
{
    std::vector<Slot> devices;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (const Slot &slot : mSlots) {
            if (slot.isBusy)
                devices.push_back(slot);
        }
    }
    if (devices.empty()) {
        std::printf("no controllers connected\n");
        return;
    }
    std::sort(devices.begin(), devices.end(), [](const Slot &a, const Slot &b) { return a.id < b.id; });

    std::string arguments;
    std::printf("connected controllers:\n");
    for (const Slot &device : devices) {
        std::printf("  [%d] %s\n", device.id, device.address.empty() ? "(unknown address)" : device.address.c_str());
        if (!device.address.empty())
            arguments += (arguments.empty() ? "" : " ") + ("--id " + std::to_string(device.id) + "=" + device.address);
    }
    if (!arguments.empty())
        std::printf("to keep these ids: %s\n", arguments.c_str());
}
