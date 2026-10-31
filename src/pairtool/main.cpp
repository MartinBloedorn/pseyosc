/*! \brief Pairing and test tool: pairs Move controllers plugged in over USB, and reports connections and button
    presses. */

#include "hub/MoveHub.h"

#include <psmove.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

std::atomic<bool> sIsRunning{true};

struct ButtonName {
    unsigned int mask;
    const char *name;
};

const ButtonName sButtonNames[] = {
    {Btn_TRIANGLE, "triangle"}, {Btn_CIRCLE, "circle"}, {Btn_CROSS, "cross"}, {Btn_SQUARE, "square"},
    {Btn_SELECT, "select"},     {Btn_START, "start"},   {Btn_PS, "PS"},       {Btn_MOVE, "move"},
    {Btn_T, "trigger"}};

/*! \brief Prints a line for every button press. */
class ButtonPrinter : public AbstractHubListener
{
public:
    void update(int id, PSMove *move, int) override
    {
        unsigned int pressed = 0;
        unsigned int released = 0;
        psmove_get_button_events(move, &pressed, &released);
        for (const ButtonName &button : sButtonNames) {
            if (pressed & button.mask)
                std::printf("[%d] %s pressed\n", id, button.name);
        }
    }
};

void printUsage()
{
    std::printf("usage: pseyosc-pair [--repair] [--shutdown-on-exit] [--idle-timeout <minutes>]\n"
                "                    [--id <id>=<address> ...]\n"
                "  --repair             also registers controllers already paired with this computer again\n"
                "                       (e.g. after removing them in the Bluetooth settings)\n"
                "  --shutdown-on-exit   switches off all connected controllers when quitting\n"
                "  --idle-timeout <min> switches off controllers that have been idle this long (default 0 = never);\n"
                "                       counts buttons and the trigger, not motion\n"
                "  --id <id>=<address>  always gives the controller with this Bluetooth address this id (0-%d),\n"
                "                       e.g. --id 0=00:06:F7:12:34:56; repeatable\n",
                MoveHub::sMaxDevices - 1);
}

} // namespace

int main(int argc, char **argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    // psmoveapi colors its warnings with ANSI escape codes, which the Windows 10 console only renders when asked to.
    const HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(console, &mode))
        SetConsoleMode(console, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif

    ButtonPrinter printer;
    MoveHub hub(&printer);
    for (int i = 1; i < argc; ++i) {
        std::string error;
        if (std::strcmp(argv[i], "--repair") == 0) {
            hub.setRepairing(true);
        } else if (std::strcmp(argv[i], "--shutdown-on-exit") == 0) {
            hub.setShutdownOnExit(true);
        } else if (std::strcmp(argv[i], "--idle-timeout") == 0 && i + 1 < argc) {
            const double minutes = std::atof(argv[++i]);
            if (minutes < 0.0) {
                printUsage();
                return 1;
            }
            hub.setIdleTimeout(minutes);
        } else if (std::strcmp(argv[i], "--id") == 0 && i + 1 < argc) {
            if (!hub.parseFixedId(argv[++i], &error)) {
                std::fprintf(stderr, "--id: %s\n", error.c_str());
                return 1;
            }
        } else {
            printUsage();
            return 1;
        }
    }

    std::signal(SIGINT, [](int) { sIsRunning = false; });
#ifdef SIGBREAK
    std::signal(SIGBREAK, [](int) { sIsRunning = false; }); // Ctrl+Break (Windows)
#endif

    if (!hub.start())
        return 1;
    std::printf("plug in a controller over USB to pair it, then unplug it and press PS to connect it;\n"
                "press its buttons to test it (Ctrl+C to quit)\n");

    while (sIsRunning)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    hub.stop();
    return 0;
}
