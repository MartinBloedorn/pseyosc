#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

struct _PSMove;
typedef struct _PSMove PSMove;

/*! \brief Receives connection changes and input from a MoveHub.

    Each controller runs on its own thread, and all calls for an \a id come from that thread: calls for one id are
    never concurrent, but calls for different ids are. \a id is 0-based and below MoveHub::sMaxDevices. \a move stays
    valid until disconnected() returns.
 */
class AbstractHubListener
{
public:
    virtual ~AbstractHubListener() = default;

    /*! \brief A controller connected over Bluetooth, or was already connected when the hub started.

        Called after the connection rumble has ended, so the listener may set the rumble right away.
     */
    virtual void connected(int id, PSMove *move, const std::string &address) {}
    /*! \brief The controller is gone, or the hub stops. Called before \a move is closed. */
    virtual void disconnected(int id) {}

    /*! \brief Called after each new input report of a connected controller (psmove_poll() returned data).

        Only for reading input: the controller's output (LEDs, rumble) belongs into updateOutput().
     */
    virtual void update(int id, PSMove *move) {}

    /*! \brief The place to set the controller's output: psmove_set_leds(), psmove_set_rumble().

        Called on every iteration of the controller's loop, holding the hub's output mutex (see
        MoveHub::setOutputMutex()); skipped while someone else holds it. The hub sends the output afterwards.
     */
    virtual void updateOutput(int id, PSMove *move) {}
};

/*! \brief Pairs controllers plugged in over USB, picks up controllers connected over Bluetooth, and gives
    connection feedback.

    A Move can't be found by a Bluetooth inquiry: it connects to the host address stored in it, which is written over
    USB. So the hub watches for controllers on USB: one that isn't paired with this computer yet gets this computer's
    address, and is registered with the OS (psmove_pair(); on Windows this needs Administrator rights, and blocks
    until the controller has connected over Bluetooth, so it runs on its own thread). After unplugging it, pressing
    its PS button connects it.

    Controllers connected over Bluetooth are opened by a finder thread, and each runs on its own thread. New
    connections are reported on stdout; the controller rumbles briefly, and its orb lights up in its id's color.

    Ids belong to controllers, identified by their Bluetooth address: controllers with a fixed id (setFixedId())
    always get it, all others get the lowest id that is neither in use nor reserved for a fixed controller.
 */
class MoveHub
{
public:
    static const int sMaxDevices = 8;

    explicit MoveHub(AbstractHubListener *listener = nullptr);
    ~MoveHub();

    MoveHub(const MoveHub &) = delete;
    MoveHub &operator=(const MoveHub &) = delete;

    /*! \brief Formats a Bluetooth address as "XX:XX:XX:XX:XX:XX".

        Accepts any separators and case ("e0ae5e0b9e5a", "e0-ae-..."); returns an empty string if \a address doesn't
        contain exactly 12 hex digits.
     */
    static std::string normalizeAddress(const std::string &address);

    /*! \brief Parses an "--id" argument of the form "<id>=<address>", and calls setFixedId(). */
    bool parseFixedId(const std::string &argument, std::string *error);

    /*! \brief Always assigns \a id to the controller with \a address. Must be called before start(). */
    bool setFixedId(int id, const std::string &address, std::string *error);

    /*! \brief Also registers controllers on USB that are already paired with this computer.

        Needed when the OS forgot the controller (e.g. it was removed in the Bluetooth settings), although the
        controller still has this computer's address.
     */
    void setRepairing(bool isEnabled) { mIsRepairing = isEnabled; }

    /*! \brief Makes stop() switch off all connected controllers, by dropping their Bluetooth connections. */
    void setShutdownOnExit(bool isEnabled) { mIsShuttingDownOnExit = isEnabled; }

    /*! \brief Switches off controllers that have been idle for \a minutes (0 = never). Must be called before start().

        Idle means no button or trigger changes; motion doesn't count yet. Controllers are switched off by dropping
        their Bluetooth connections; listeners get disconnected() as usual.
     */
    void setIdleTimeout(double minutes)
    {
        mIdleTimeout = std::chrono::milliseconds(static_cast<long long>(minutes * 60000.0));
    }

    /*! \brief Shares the mutex that guards the controllers' output (LEDs, rumble) with someone else using the
        controllers' handles, e.g. the camera tracker. Must be called before start().

        The controller threads hold it while setting and sending their output; while someone else holds it, they skip
        their output, but keep reading input. By default, the hub uses a mutex of its own.
     */
    void setOutputMutex(std::mutex *mutex) { mOutputMutex = mutex; }

    /*! \brief Reads the host's Bluetooth address, and starts pairing and picking up controllers in the background.

        Fails without a Bluetooth radio. Without the rights to pair (Windows: Administrator), only controllers paired
        before are picked up.
     */
    bool start();

    /*! \brief Prints the connected controllers (as "--id" arguments, to keep their ids), stops picking up
        controllers, and waits for the controller threads to finish.

        With setShutdownOnExit(), the controllers are disconnected (and so switched off), and listeners get
        disconnected(). Pairings still waiting for their controller aren't waited for.
     */
    void stop();

private:
    struct Slot {
        bool isBusy = false; //!< owned by a device thread
        int id = -1;
        std::string address;
        std::string path; //!< HID device path
    };

    struct PairingState {
        std::mutex mutex;
        std::set<std::string> addresses; //!< controllers whose pairing is running
    };

    void runFinder();
    void handleUsbController(PSMove *move);
    void startDevice(PSMove *move, const std::string &path);
    void runDevice(int index, PSMove *move);
    int chooseId(const std::string &address) const;
    void printConnectedDevices();

    AbstractHubListener *mListener;
    std::map<std::string, int> mFixedIds; //!< by normalized address
    std::string mHostAddress;
    bool mCanPair = false;
    bool mIsRepairing = false;
    bool mIsShuttingDownOnExit = false;
    std::chrono::milliseconds mIdleTimeout{0}; //!< 0: never
    std::mutex mOwnOutputMutex;
    std::mutex *mOutputMutex = &mOwnOutputMutex; //!< see setOutputMutex()

    std::mutex mMutex; //!< guards mSlots
    Slot mSlots[sMaxDevices];
    std::thread mDeviceThreads[sMaxDevices];
    std::thread mFinderThread;
    std::atomic<bool> mIsRunning{false};
    std::set<std::string> mUsbPaths; //!< USB controllers already handled; finder thread only
    std::shared_ptr<PairingState> mPairing = std::make_shared<PairingState>(); //!< shared with pairing threads
};
