#include "MoveStreamer.h"

#include "osc/OscReceiver.h"
#include "osc/OscSender.h"

#include <psmove.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace {

struct ButtonName {
    unsigned int mask;
    const char *path;
};

const ButtonName sButtons[] = {
    {Btn_TRIANGLE, "move/button/triangle"}, {Btn_CIRCLE, "move/button/circle"}, {Btn_CROSS, "move/button/cross"},
    {Btn_SQUARE, "move/button/square"},     {Btn_SELECT, "move/button/select"}, {Btn_START, "move/button/start"},
    {Btn_PS, "move/button/ps"},             {Btn_MOVE, "move/button/move"},
};
// The trigger's digital state (Btn_T) isn't sent: move/button/trigger is analog, receivers threshold it themselves.

const float sRadiansToDegrees = 57.29577951f;
const float sMaxRumblePulseMs = 3000.0f;
const char sRumblePath[] = "move/rumble/";
const char sLedPath[] = "move/led";
/*! \brief Ignored addresses are printed once each, up to this many. */
const size_t sMaxIgnoredAddresses = 64;

/*! \brief The lowest rumble value that turns the motor: on a ZCM1, 0.4 (102) did nothing, 0.5 (128) rumbled. */
const int sMinRumble = 128;

/*! \brief Maps 0-1 to 0-255, clamped. */
int toByte(float value)
{
    return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
}

/*! \brief Maps an intensity 0-1 to a rumble value: 0 is off, anything above spans the motor's working range. */
int toRumble(float intensity)
{
    if (!(intensity > 0.0f))
        return 0;
    const float clamped = std::min(intensity, 1.0f);
    return static_cast<int>(std::lround(sMinRumble + clamped * (255 - sMinRumble)));
}

} // namespace

MoveStreamer::MoveStreamer(OscSender &sender, const Options &options)
    : mSender(sender)
    , mOptions(options)
{
}

void MoveStreamer::connected(int id, PSMove *move, const std::string &)
{
    {
        std::lock_guard<std::mutex> lock(mRequestMutex);
        mRequests[id] = Request{};
    }
    mDevices[id] = Device{};
    send(OscMessage(address(id, "status")).add(std::string("move")));
    // No sensor fusion yet: the orientation is sent once, as "no rotation", so receivers have the addresses.
    sendFloats(id, "move/rpy", {0.0f, 0.0f, 0.0f});
    sendFloats(id, "move/quat", {1.0f, 0.0f, 0.0f, 0.0f});
    if (mTracker) {
        {
            std::lock_guard<std::mutex> lock(mTrackMutex);
            mTracks[id] = Track{};
        }
        mTracker->addController(id, move);
    }
}

void MoveStreamer::disconnected(int id)
{
    if (mTracker)
        mTracker->removeController(id); // the tracker lets go of the handle before the hub closes it
    send(OscMessage(address(id, "status")).add(std::string("none")));
    mDevices[id] = Device{};
    std::lock_guard<std::mutex> lock(mRequestMutex);
    mRequests[id] = Request{};
}

void MoveStreamer::update(int id, PSMove *move)
{
    Device &device = mDevices[id];
    const auto now = Clock::now();

    const unsigned int buttons = psmove_get_buttons(move);
    const unsigned int calibrationButtons = Btn_SELECT | Btn_START;
    const bool isCalibrationPressed = (buttons & calibrationButtons) == calibrationButtons;
    if (mTracker && isCalibrationPressed && !device.wasCalibrationPressed)
        mTracker->requestCalibration(id);
    device.wasCalibrationPressed = isCalibrationPressed;

    for (const ButtonName &button : sButtons)
        sendInt(id, button.path, (buttons & button.mask) ? 1 : 0);

    // Levels 0-5 (in steps of 20%); while charging, the level isn't known, so the last one stays.
    const int battery = psmove_get_battery(move);
    if (battery == Batt_CHARGING_DONE)
        sendFloats(id, "battery", {1.0f});
    else if (battery >= Batt_MIN && battery <= Batt_MAX)
        sendFloats(id, "battery", {static_cast<float>(battery) / Batt_MAX});

    if (now < device.nextContinuous)
        return;
    if (mOptions.maxRate > 0)
        device.nextContinuous = now + std::chrono::microseconds(1000000 / mOptions.maxRate);

    sendFloats(id, "move/button/trigger", {psmove_get_trigger(move) / 255.0f});
    // Each report holds two samples; the second half is the newer one.
    float x, y, z;
    psmove_get_accelerometer_frame(move, Frame_SecondHalf, &x, &y, &z);
    sendFloats(id, "move/accel", {x, y, z});
    psmove_get_gyroscope_frame(move, Frame_SecondHalf, &x, &y, &z);
    sendFloats(id, "move/gyro", {x * sRadiansToDegrees, y * sRadiansToDegrees, z * sRadiansToDegrees});
}

void MoveStreamer::updateOutput(int id, PSMove *move)
{
    applyRequest(id, move);
    if (mTracker) {
        // The orb shows the color the tracker looks for; it's off until the controller is calibrated.
        unsigned char r = 0, g = 0, b = 0;
        mTracker->orbColor(id, &r, &g, &b);
        psmove_set_leds(move, r, g, b); // only sent if it changed
    }
}

void MoveStreamer::tracked(int id, bool isFound, const TrackedPosition &position)
{
    std::lock_guard<std::mutex> lock(mTrackMutex);
    Track &track = mTracks[id];
    if (track.found != (isFound ? 1 : 0)) {
        track.found = isFound ? 1 : 0;
        send(OscMessage(address(id, "track/found")).add(static_cast<int32_t>(track.found)));
    }
    if (!isFound)
        return;

    const auto now = Clock::now();
    if (now < track.nextPosition)
        return;
    if (mOptions.maxRate > 0)
        track.nextPosition = now + std::chrono::microseconds(1000000 / mOptions.maxRate);
    auto sendIfChanged = [&](const char *path, const float (&values)[3], float (&sent)[3]) {
        if (track.hasSent && std::equal(std::begin(values), std::end(values), sent))
            return;
        std::copy(std::begin(values), std::end(values), sent);
        send(OscMessage(address(id, path)).add(values[0]).add(values[1]).add(values[2]));
    };
    sendIfChanged("track/pos", position.position, track.sent.position);
    sendIfChanged("track/xyz", position.normalized, track.sent.normalized);
    track.hasSent = true;
}

void MoveStreamer::applyRequest(int id, PSMove *move)
{
    int rumble;
    int led[3];
    {
        std::lock_guard<std::mutex> lock(mRequestMutex);
        Request &request = mRequests[id];
        if (request.isPulse && Clock::now() >= request.pulseEnd) {
            request.rumble = 0;
            request.isPulse = false;
        }
        // psmoveapi keeps the values and the hub sends them, so each request is applied once.
        rumble = request.rumble;
        request.rumble = -1;
        std::copy(std::begin(request.led), std::end(request.led), led);
        std::fill(std::begin(request.led), std::end(request.led), -1);
    }
    if (rumble >= 0)
        psmove_set_rumble(move, static_cast<unsigned char>(rumble));
    if (led[0] >= 0)
        psmove_set_leds(move, static_cast<unsigned char>(led[0]), static_cast<unsigned char>(led[1]),
                        static_cast<unsigned char>(led[2]));
}

void MoveStreamer::received(const OscReceivedMessage &message)
{
    // <prefix>/<id>/<path>
    const std::string &prefix = mOptions.prefix;
    const std::string &addr = message.address;
    size_t position = prefix.size();
    if (addr.compare(0, position, prefix) != 0 || position >= addr.size() || addr[position] != '/')
        return ignore(message, "unknown address");
    int id = 0;
    const size_t idStart = ++position;
    while (position < addr.size() && position - idStart < 3 && std::isdigit(static_cast<unsigned char>(addr[position])))
        id = id * 10 + (addr[position++] - '0');
    if (position == idStart || position >= addr.size() || addr[position] != '/')
        return ignore(message, "unknown address");
    const std::string path = addr.substr(position + 1);

    const bool isRumble = path.compare(0, sizeof(sRumblePath) - 1, sRumblePath) == 0;
    const bool isLed = path == sLedPath;
    if (!isRumble && !isLed)
        return ignore(message, "unknown address");
    if (id >= MoveHub::sMaxDevices)
        return ignore(message, "id out of range");
    if (isLed) {
        if (mTracker)
            return ignore(message, "the orb's color is set by the tracker (--track)");
        return receivedLed(id, message);
    }
    receivedRumble(id, path.substr(sizeof(sRumblePath) - 1), message);
}

void MoveStreamer::receivedRumble(int id, const std::string &command, const OscReceivedMessage &message)
{
    float intensity;
    int rumble;
    bool isPulse = false;
    Clock::time_point pulseEnd;
    if (command == "trigger") {
        if (!message.toFloat(0, &intensity))
            return ignore(message, "expects an intensity (0-1)");
        rumble = toRumble(intensity);
    } else if (command == "pulse") {
        float durationMs;
        if (!message.toFloat(0, &intensity) || !message.toFloat(1, &durationMs))
            return ignore(message, "expects an intensity (0-1) and a duration (ms)");
        durationMs = std::min(durationMs, sMaxRumblePulseMs);
        // A pulse that wouldn't rumble still overrides the ongoing request: it switches the motor off.
        rumble = durationMs > 0.0f ? toRumble(intensity) : 0;
        isPulse = rumble > 0;
        pulseEnd = Clock::now() + std::chrono::microseconds(static_cast<long long>(durationMs * 1000.0f));
    } else {
        return ignore(message, "unknown address");
    }

    std::lock_guard<std::mutex> lock(mRequestMutex);
    Request &request = mRequests[id];
    request.rumble = rumble;
    request.isPulse = isPulse;
    request.pulseEnd = pulseEnd;
}

void MoveStreamer::receivedLed(int id, const OscReceivedMessage &message)
{
    float rgb[3];
    for (int i = 0; i < 3; ++i) {
        if (!message.toFloat(i, &rgb[i]))
            return ignore(message, "expects red, green and blue (0-1 each)");
    }
    std::lock_guard<std::mutex> lock(mRequestMutex);
    for (int i = 0; i < 3; ++i)
        mRequests[id].led[i] = toByte(rgb[i]);
}

void MoveStreamer::ignore(const OscReceivedMessage &message, const char *reason)
{
    if (mIgnoredAddresses.size() < sMaxIgnoredAddresses && mIgnoredAddresses.insert(message.address).second)
        std::printf("[osc] ignoring %s: %s (printed once per address)\n", message.address.c_str(), reason);
}

std::string MoveStreamer::address(int id, const char *path) const
{
    return mOptions.prefix + "/" + std::to_string(id) + "/" + path;
}

void MoveStreamer::send(const OscMessage &message)
{
    mSender.send(message);
}

void MoveStreamer::sendFloats(int id, const char *path, std::initializer_list<float> values)
{
    std::vector<float> &sent = mDevices[id].sentValues[path];
    if (sent.size() == values.size() && std::equal(values.begin(), values.end(), sent.begin()))
        return;
    sent.assign(values);

    OscMessage message(address(id, path));
    for (float value : values)
        message.add(value);
    send(message);
}

void MoveStreamer::sendInt(int id, const char *path, int value)
{
    std::vector<float> &sent = mDevices[id].sentValues[path];
    if (sent.size() == 1 && sent[0] == static_cast<float>(value))
        return;
    sent.assign({static_cast<float>(value)});
    send(OscMessage(address(id, path)).add(static_cast<int32_t>(value)));
}
