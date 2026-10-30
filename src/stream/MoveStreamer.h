#pragma once

#include "hub/MoveHub.h"

#include <chrono>
#include <initializer_list>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class OscMessage;
class OscSender;
struct OscReceivedMessage;

/*! \brief Streams the input of all connected controllers as OSC messages.

    Addresses have the form <prefix>/<id>/<path>, see doc/Streaming.md; they follow wiimosc's, with move/ in place of
    mote/. Values are only sent when they change. Buttons are sent immediately, continuous values (acceleration,
    rotation rates, trigger) at most \c maxRate times per second.

    It also takes OSC input (received()): <prefix>/<id>/move/rumble/{trigger,pulse} set a controller's rumble, and
    <prefix>/<id>/move/led its orb color.
 */
class MoveStreamer : public AbstractHubListener
{
public:
    struct Options {
        std::string prefix; //!< normalized: empty, or starting with '/' and without trailing '/'
        int maxRate = 0;    //!< per controller, in Hz; 0 is unlimited
    };

    MoveStreamer(OscSender &sender, const Options &options);

    void connected(int id, PSMove *move, const std::string &address) override;
    void disconnected(int id) override;
    void update(int id, PSMove *move) override;

    /*! \brief Handles an OSC message received on the input port. Called from the receiver's thread.

        Requests are stored, and applied by the controller's own thread on its next update(); requests for ids that
        aren't connected are dropped.
     */
    void received(const OscReceivedMessage &message);

private:
    using Clock = std::chrono::steady_clock;

    struct Device {
        Clock::time_point nextContinuous;
        std::map<std::string, std::vector<float>> sentValues; //!< by path, for change detection
    };

    /*! \brief A controller's rumble and orb color, as last requested over OSC.

        Each rumble request replaces the previous one, so a trigger cuts a pulse short and vice versa.
     */
    struct Request {
        int rumble = -1;            //!< 0-255; -1 not requested since connecting (or already applied)
        bool isPulse = false;       //!< switches off at pulseEnd
        Clock::time_point pulseEnd;
        int led[3] = {-1, -1, -1};  //!< 0-255 each; -1 not requested (or already applied)
    };

    std::string address(int id, const char *path) const;
    void send(const OscMessage &message);

    /*! \brief Sends the values as floats, if they differ from the ones last sent on \a path. */
    void sendFloats(int id, const char *path, std::initializer_list<float> values);
    /*! \brief Sends the value as int, if it differs from the one last sent on \a path. */
    void sendInt(int id, const char *path, int value);

    void applyRequest(int id, PSMove *move);
    /*! \brief Handles <prefix>/<id>/move/rumble/<command>. Called from the receiver's thread. */
    void receivedRumble(int id, const std::string &command, const OscReceivedMessage &message);
    /*! \brief Handles <prefix>/<id>/move/led. Called from the receiver's thread. */
    void receivedLed(int id, const OscReceivedMessage &message);
    /*! \brief Prints why \a message is ignored, once per address. Called from the receiver's thread. */
    void ignore(const OscReceivedMessage &message, const char *reason);

    OscSender &mSender;
    Options mOptions;
    Device mDevices[MoveHub::sMaxDevices];

    /*! \brief Requests per id: written by the receiver's thread, applied by the controller's thread (guarded by
        mRequestMutex). Kept apart from Device, which is reset by assignment.
     */
    Request mRequests[MoveHub::sMaxDevices];
    std::mutex mRequestMutex;
    std::set<std::string> mIgnoredAddresses; //!< receiver's thread only
};
