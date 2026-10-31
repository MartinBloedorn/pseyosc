#pragma once

#include "fusion/MotionFusion.h"
#include "hub/MoveHub.h"
#include "tracking/MoveTracker.h"

#include <chrono>
#include <initializer_list>
#include <map>
#include <memory>
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
    rotation rates, trigger, orientation) at most \c maxRate times per second.

    With sensor fusion (Options::filter), each controller gets a MotionFusion: once it's ready, move/rpy and move/quat
    carry its orientation. Pressing Select and Start together restarts it (yaw 0 is then the current heading).

    It also takes OSC input (received()): <prefix>/<id>/move/rumble/{trigger,pulse} set a controller's rumble,
    <prefix>/<id>/move/led its orb color, and <prefix>/<id>/move/hint/yaw moves its fused yaw.

    With a tracker (setTracker()), it adds the controllers to it as they connect, streams their tracked positions
    (<prefix>/<id>/track/found, <prefix>/<id>/track/pos, <prefix>/<id>/track/xyz), and sets the orb colors the
    tracker chose instead of the requested ones. Pressing Select and Start together also calibrates the controller's
    tracking again.
 */
class MoveStreamer : public AbstractHubListener, public AbstractTrackerListener
{
public:
    struct Options {
        std::string prefix; //!< normalized: empty, or starting with '/' and without trailing '/'
        int maxRate = 0;    //!< per controller, in Hz; 0 is unlimited
        FilterType filter = FilterType::None; //!< sensor fusion; None: move/rpy and move/quat are placeholders
    };

    MoveStreamer(OscSender &sender, const Options &options);

    /*! \brief Enables tracking with \a tracker (which must be started). Must be called before the hub starts. */
    void setTracker(MoveTracker *tracker) { mTracker = tracker; }

    void connected(int id, PSMove *move, const std::string &address) override;
    void disconnected(int id) override;
    void update(int id, PSMove *move, int sequence) override;
    void updateOutput(int id, PSMove *move) override;

    /*! \brief Streams a tracked position. Called from the tracker's thread. */
    void tracked(int id, bool isFound, const TrackedPosition &position) override;

    /*! \brief Handles an OSC message received on the input port. Called from the receiver's thread.

        Requests are stored, and applied by the controller's own thread on its next updateOutput(); requests for ids
        that aren't connected are dropped. With a tracker, <prefix>/<id>/move/led is ignored.
     */
    void received(const OscReceivedMessage &message);

private:
    using Clock = std::chrono::steady_clock;

    struct Device {
        Clock::time_point nextContinuous;
        std::map<std::string, std::vector<float>> sentValues; //!< by path, for change detection
        bool wasCalibrationPressed = false; //!< Select and Start were held together
        std::unique_ptr<MotionFusion> fusion; //!< with Options::filter
        bool wasFusionReady = false;
    };

    /*! \brief What was last sent of a controller's tracked position (tracker's thread). */
    struct Track {
        int found = -1; //!< -1: not sent yet
        TrackedPosition sent;
        bool hasSent = false;
        Clock::time_point nextPosition;
    };

    /*! \brief A controller's rumble, orb color and yaw hint, as last requested over OSC.

        Each rumble request replaces the previous one, so a trigger cuts a pulse short and vice versa.
     */
    struct Request {
        int rumble = -1;            //!< 0-255; -1 not requested since connecting (or already applied)
        bool isPulse = false;       //!< switches off at pulseEnd
        Clock::time_point pulseEnd;
        int led[3] = {-1, -1, -1};  //!< 0-255 each; -1 not requested (or already applied)
        bool isYawHinted = false;   //!< a move/hint/yaw not yet applied
        float hintYaw = 0.0f;
        float hintStrength = 1.0f;
    };

    std::string address(int id, const char *path) const;
    void send(const OscMessage &message);

    /*! \brief Sends the values as floats, if they differ from the ones last sent on \a path. */
    void sendFloats(int id, const char *path, std::initializer_list<float> values);
    /*! \brief Sends the value as int, if it differs from the one last sent on \a path. */
    void sendInt(int id, const char *path, int value);

    /*! \brief Feeds the report's motion samples to the controller's fusion, and applies a pending yaw hint. */
    void updateFusion(int id, PSMove *move, int sequence);
    /*! \brief Requests a rumble pulse, as if received over OSC. */
    void requestPulse(int id, int rumble, int durationMs);

    void applyRequest(int id, PSMove *move);
    /*! \brief Handles <prefix>/<id>/move/rumble/<command>. Called from the receiver's thread. */
    void receivedRumble(int id, const std::string &command, const OscReceivedMessage &message);
    /*! \brief Handles <prefix>/<id>/move/led. Called from the receiver's thread. */
    void receivedLed(int id, const OscReceivedMessage &message);
    /*! \brief Handles <prefix>/<id>/move/hint/<command>. Called from the receiver's thread. */
    void receivedHint(int id, const std::string &command, const OscReceivedMessage &message);
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

    MoveTracker *mTracker = nullptr;
    /*! \brief Per id: written by the tracker's thread, reset on connecting (guarded by mTrackMutex). */
    Track mTracks[MoveHub::sMaxDevices];
    std::mutex mTrackMutex;
};
