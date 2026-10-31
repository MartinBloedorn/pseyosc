/*! \brief Streaming tool: pairs controllers like pseyosc-pair, and streams their input as OSC. */

#include "MoveStreamer.h"
#include "tracking/MoveTracker.h"
#include "osc/OscReceiver.h"
#include "osc/OscSender.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::atomic<bool> sIsRunning{true};

/*! \brief Parses "<name>=<number>" into \a value, if \a option starts with \a name; false if it doesn't. */
bool parseNumber(const std::string &option, const char *name, float *value, bool *isValid)
{
    const size_t length = std::strlen(name);
    if (option.compare(0, length, name) != 0 || option.size() <= length || option[length] != '=')
        return false;
    char *rest = nullptr;
    *value = std::strtof(option.c_str() + length + 1, &rest);
    *isValid = *rest == '\0' && rest != option.c_str() + length + 1;
    return true;
}

/*! \brief Parses the options of "--track": comma-separated "exposure=<0-1>", "mirror", "hue", "fov=<degrees>". */
bool parseTrackOptions(const std::string &text, MoveTracker::Options *options)
{
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string option = text.substr(start, end - start);
        bool isValid = true;
        if (option == "mirror") {
            options->isMirrored = true;
        } else if (option == "hue") {
            options->isHueCalibrating = true;
        } else if (parseNumber(option, "exposure", &options->exposure, &isValid)) {
            if (!isValid || options->exposure < 0.0f || options->exposure > 1.0f)
                return false;
        } else if (parseNumber(option, "fov", &options->fieldOfView, &isValid)) {
            if (!isValid || options->fieldOfView <= 0.0f || options->fieldOfView >= 180.0f)
                return false;
        } else if (!option.empty()) {
            return false;
        }
        start = end + 1;
    }
    return true;
}

/*! \brief Turns "pseyosc", "/pseyosc/" etc. into "/pseyosc"; an empty prefix stays empty. */
std::string normalizePrefix(std::string prefix)
{
    while (!prefix.empty() && prefix.back() == '/')
        prefix.pop_back();
    if (!prefix.empty() && prefix.front() != '/')
        prefix.insert(prefix.begin(), '/');
    return prefix;
}

void printUsage()
{
    std::printf("usage: pseyosc-stream [options]\n"
                "  --host <host>      OSC target, name, IPv4 or broadcast address (default 127.0.0.1)\n"
                "  --port <port>      OSC target port (default 4321)\n"
                "  --prefix <prefix>  address prefix (default \"pseyosc\")\n"
                "  --in-port <port>   receive OSC on this UDP port, on all interfaces (default: off):\n"
                "                     <prefix>/<id>/move/rumble/trigger (intensity 0-1, 0 = off),\n"
                "                     .../rumble/pulse (intensity, duration in ms up to 3000),\n"
                "                     <prefix>/<id>/move/led (red, green, blue, 0-1 each)\n"
                "  --max-rate <hz>    max. rate of continuous values per controller, 0 = unlimited (default 0)\n"
                "  --repair           also register controllers on USB already paired with this computer again\n"
                "  --shutdown-on-exit switch off all connected controllers when quitting\n"
                "  --idle-timeout <minutes>\n"
                "                     switch off controllers that have been idle this long (default 0 = never);\n"
                "                     counts buttons and the trigger, not motion\n"
                "  --id <id>=<addr>   always give the controller with this Bluetooth address this id (0-%d),\n"
                "                     e.g. --id 0=00:13:8A:9A:FE:2E; repeatable\n"
                "  --fusion [madg|off]\n"
                "                     sensor fusion: move/rpy including yaw, and move/quat; madg = Madgwick's AHRS,\n"
                "                     the default filter. Each controller first has to be still for a moment, and\n"
                "                     rumbles briefly once it's calibrated; Select and Start together restart it\n"
                "                     (default: no fusion, placeholders)\n"
                "  --track [options]  track the orbs with the camera, and stream their positions\n"
                "                     (<prefix>/<id>/track/found, <prefix>/<id>/track/pos: x, y, z in cm from the lens;\n"
                "                     z out of the lens, y up, x to the camera's left; <prefix>/<id>/track/xyz: x, y\n"
                "                     -1-1 across the image, z 0-1 of the tracking range); the tracker sets the orb colors,\n"
                "                     <prefix>/<id>/move/led is ignored. Controllers are calibrated when they connect,\n"
                "                     and again when Select and Start are pressed together. Options, comma-separated:\n"
                "                     exposure=<0-1> (default 0.3), mirror (mirror the image), hue (calibrate the\n"
                "                     colors once, with the first controller), fov=<degrees> (the lens's diagonal\n"
                "                     field of view: 75 blue dot (default), 56 red dot),\n"
                "                     e.g. --track exposure=0.2,mirror\n",
                MoveHub::sMaxDevices - 1);
}

} // namespace

int main(int argc, char **argv)
{
    std::string host = "127.0.0.1";
    int port = 4321;
    int inPort = 0; // 0: no input
    MoveStreamer::Options options;
    options.prefix = "pseyosc";
    std::vector<std::string> fixedIds;
    bool isRepairing = false;
    bool isShuttingDownOnExit = false;
    double idleTimeout = 0.0;
    bool isTracking = false;
    MoveTracker::Options trackOptions;

    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--host") == 0 && hasValue) {
            host = argv[++i];
        } else if (std::strcmp(argv[i], "--port") == 0 && hasValue) {
            port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--in-port") == 0 && hasValue) {
            inPort = std::atoi(argv[++i]);
            if (inPort <= 0 || inPort > 65535) {
                printUsage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--prefix") == 0 && hasValue) {
            options.prefix = argv[++i];
        } else if (std::strcmp(argv[i], "--max-rate") == 0 && hasValue) {
            options.maxRate = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--repair") == 0) {
            isRepairing = true;
        } else if (std::strcmp(argv[i], "--idle-timeout") == 0 && hasValue) {
            idleTimeout = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--shutdown-on-exit") == 0) {
            isShuttingDownOnExit = true;
        } else if (std::strcmp(argv[i], "--id") == 0 && hasValue) {
            fixedIds.push_back(argv[++i]);
        } else if (std::strcmp(argv[i], "--fusion") == 0) {
            // The filter's name is optional: the next argument is only taken if it is one.
            options.filter = FilterType::Madgwick;
            if (hasValue && parseFilterType(argv[i + 1], &options.filter))
                ++i;
        } else if (std::strcmp(argv[i], "--track") == 0) {
            isTracking = true;
            // The options are optional: the next argument is one, unless it's the next option.
            if (hasValue && std::strncmp(argv[i + 1], "--", 2) != 0 && !parseTrackOptions(argv[++i], &trackOptions)) {
                printUsage();
                return 1;
            }
        } else {
            printUsage();
            return 1;
        }
    }
    if (port <= 0 || port > 65535 || options.maxRate < 0 || idleTimeout < 0.0) {
        printUsage();
        return 1;
    }
    if (host == "0.0.0.0") {
        std::fprintf(stderr, "0.0.0.0 is a listening address, not a target; use 127.0.0.1 or a broadcast address\n");
        return 1;
    }
    options.prefix = normalizePrefix(options.prefix);

    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    // psmoveapi colors its warnings with ANSI escape codes, which the Windows 10 console only renders when asked to.
    const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(console, &mode))
        SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    std::signal(SIGINT, [](int) { sIsRunning = false; });
#ifdef SIGBREAK
    std::signal(SIGBREAK, [](int) { sIsRunning = false; }); // Ctrl+Break (Windows)
#endif

    OscSender sender;
    if (!sender.open(host, static_cast<uint16_t>(port))) {
        std::fprintf(stderr, "can't resolve OSC target %s\n", host.c_str());
        return 1;
    }

    OscReceiver receiver;
    if (inPort != 0 && !receiver.open(static_cast<uint16_t>(inPort))) {
        std::fprintf(stderr, "can't listen on UDP port %d (in use?)\n", inPort);
        return 1;
    }

    MoveStreamer streamer(sender, options);
    MoveTracker tracker(&streamer, trackOptions); // before the hub, so it's destroyed after it
    MoveHub hub(&streamer);
    if (isTracking) {
        if (!tracker.start())
            return 1;
        streamer.setTracker(&tracker);
        hub.setOutputMutex(&tracker.outputMutex());
    }
    for (const std::string &fixedId : fixedIds) {
        std::string error;
        if (!hub.parseFixedId(fixedId, &error)) {
            std::fprintf(stderr, "--id: %s\n", error.c_str());
            return 1;
        }
    }
    hub.setRepairing(isRepairing);
    hub.setShutdownOnExit(isShuttingDownOnExit);
    hub.setIdleTimeout(idleTimeout);
    if (!hub.start())
        return 1;
    std::printf("streaming to %s:%d as %s/<id>/..., press PS on a controller to connect it (Ctrl+C to quit)\n",
                host.c_str(), port, options.prefix.c_str());
    if (inPort != 0) {
        receiver.start([&streamer](const OscReceivedMessage &message) { streamer.received(message); });
        std::printf("receiving OSC on port %d\n", inPort);
    }

    while (sIsRunning)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    receiver.stop();
    hub.stop(); // listeners get disconnected() for every controller, so receivers get "status none"
    tracker.stop();
    return 0;
}
