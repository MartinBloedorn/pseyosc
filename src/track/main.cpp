/*! \brief Tracker test tool: tracks the orbs of the connected Move controllers with the PS3 Eye camera (psmoveapi's
    tracker), shows the camera image, and prints the controllers' positions. */

#include <psmove.h>
#include <psmove_tracker.h>
#include <psmove_tracker_opencv.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

std::atomic<bool> sIsRunning{true};

const char sWindowName[] = "pseyosc-track";
const auto sPrintInterval = std::chrono::milliseconds(100);

struct Controller {
    PSMove *move;
    std::string address;
};

void printUsage()
{
    std::printf("usage: pseyosc-track [--exposure <0-1>] [--no-mirror]\n"
                "  --exposure <0-1>  camera exposure (default 0.3); lower makes everything but the orbs darker\n"
                "  --no-mirror       shows the camera image as the camera sees it, not mirrored\n");
}

/*! \brief Blinks the controller's orb to learn its color in the camera image, and starts tracking it. */
void calibrate(PSMoveTracker *tracker, int index, const Controller &controller)
{
    std::printf("[%d] %s: calibrating (keep the orb visible to the camera)...\n", index, controller.address.c_str());
    if (psmove_tracker_enable(tracker, controller.move) == Tracker_CALIBRATED)
        std::printf("[%d] calibrated\n", index);
    else
        std::printf("[%d] calibration failed: check that the orb is visible, and that there are no other bright "
                    "lights in view; press R to retry\n",
                    index);
}

/*! \brief Prints one line with the position of every controller. */
void printPositions(PSMoveTracker *tracker, const std::vector<Controller> &controllers)
{
    std::string line;
    char text[96];
    for (size_t i = 0; i < controllers.size(); ++i) {
        PSMove *move = controllers[i].move;
        switch (psmove_tracker_get_status(tracker, move)) {
        case Tracker_TRACKING: {
            float x, y, radius;
            psmove_tracker_get_position(tracker, move, &x, &y, &radius);
            std::snprintf(text, sizeof(text), "[%zu] x %6.1f y %6.1f r %5.1f px, %4.0f cm", i, x, y, radius,
                          psmove_tracker_distance_from_radius(tracker, radius));
            break;
        }
        case Tracker_CALIBRATED:
            std::snprintf(text, sizeof(text), "[%zu] not visible", i);
            break;
        default:
            std::snprintf(text, sizeof(text), "[%zu] not calibrated", i);
            break;
        }
        line += (i > 0 ? "   " : "") + std::string(text);
    }
    std::printf("%s\n", line.c_str());
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    // psmoveapi colors its messages with ANSI escape codes, which the Windows 10 console only renders when asked to.
    const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(console, &mode))
        SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif

    PSMoveTrackerSettings settings;
    psmove_tracker_settings_set_default(&settings);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--exposure") == 0 && i + 1 < argc) {
            settings.camera_exposure = static_cast<float>(std::atof(argv[++i]));
            if (settings.camera_exposure < 0.0f || settings.camera_exposure > 1.0f) {
                printUsage();
                return 1;
            }
        } else if (std::strcmp(argv[i], "--no-mirror") == 0) {
            settings.camera_mirror = false;
        } else {
            printUsage();
            return 1;
        }
    }

    std::signal(SIGINT, [](int) { sIsRunning = false; });
#ifdef SIGBREAK
    std::signal(SIGBREAK, [](int) { sIsRunning = false; }); // Ctrl+Break (Windows)
#endif

    // Controllers connected over Bluetooth; over USB, a controller can't be tracked while moving freely.
    psmove_set_remote_config(PSMove_OnlyLocal);
    std::vector<Controller> controllers;
    const int count = psmove_count_connected();
    for (int i = 0; i < count; ++i) {
        PSMove *move = psmove_connect_by_id(i);
        if (!move)
            continue;
        if (psmove_connection_type(move) != Conn_Bluetooth) {
            psmove_disconnect(move);
            continue;
        }
        char *serial = psmove_get_serial(move);
        controllers.push_back({move, serial ? serial : "?"});
        psmove_free_mem(serial);
    }
    if (controllers.empty()) {
        std::fprintf(stderr, "no controller connected over Bluetooth: pair it with pseyosc-pair, then press PS\n");
        return 1;
    }
    if (controllers.size() > PSMOVE_TRACKER_MAX_CONTROLLERS) {
        std::printf("tracking the first %d of %zu controllers\n", PSMOVE_TRACKER_MAX_CONTROLLERS, controllers.size());
        for (size_t i = PSMOVE_TRACKER_MAX_CONTROLLERS; i < controllers.size(); ++i)
            psmove_disconnect(controllers[i].move);
        controllers.resize(PSMOVE_TRACKER_MAX_CONTROLLERS);
    }

    PSMoveTracker *tracker = psmove_tracker_new_with_settings(&settings);
    if (!tracker) {
        std::fprintf(stderr, "could not open the camera\n");
        for (const Controller &controller : controllers)
            psmove_disconnect(controller.move);
        return 1;
    }
    const PSMoveCameraInfo *camera = psmove_tracker_get_camera_info(tracker);
    std::printf("camera: %s, %dx%d\n", camera->camera_name, camera->width, camera->height);

    cv::namedWindow(sWindowName);
    for (size_t i = 0; i < controllers.size(); ++i)
        calibrate(tracker, static_cast<int>(i), controllers[i]);
    std::printf("R: calibrate again, Esc or Ctrl+C: quit\n");

    auto nextPrint = std::chrono::steady_clock::now();
    while (sIsRunning) {
        // The tracker doesn't use the input reports; reading them keeps them from queuing up.
        for (const Controller &controller : controllers) {
            while (psmove_poll(controller.move)) {
            }
        }

        psmove_tracker_update_image(tracker);
        psmove_tracker_update(tracker, nullptr); // also keeps the orbs lit in their tracking colors
        psmove_tracker_annotate(tracker, true, false);
        IplImage *frame = psmove_tracker_opencv_get_frame(tracker);
        if (frame)
            cv::imshow(sWindowName, cv::cvarrToMat(frame));

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextPrint) {
            printPositions(tracker, controllers);
            nextPrint = now + sPrintInterval;
        }

        const int key = cv::waitKey(1);
        if (key == 27 || cv::getWindowProperty(sWindowName, cv::WND_PROP_VISIBLE) < 1.0) {
            sIsRunning = false;
        } else if (key == 'r' || key == 'R') {
            for (const Controller &controller : controllers)
                psmove_tracker_disable(tracker, controller.move);
            for (size_t i = 0; i < controllers.size(); ++i)
                calibrate(tracker, static_cast<int>(i), controllers[i]);
        }
    }

    for (const Controller &controller : controllers) {
        psmove_tracker_disable(tracker, controller.move);
        psmove_set_leds(controller.move, 0, 0, 0);
        psmove_update_leds(controller.move);
    }
    psmove_tracker_free(tracker);
    for (const Controller &controller : controllers)
        psmove_disconnect(controller.move);
    cv::destroyAllWindows();
    return 0;
}
