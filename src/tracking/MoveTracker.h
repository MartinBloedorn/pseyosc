#pragma once

#include "hub/MoveHub.h"

#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

struct _PSMoveTracker;
typedef struct _PSMoveTracker PSMoveTracker;

/*! \brief A tracked orb's position, in two forms.

    Both use the camera's frame: z points out of the lens (through the image's center), y up, x to the camera's left.
    Neither depends on MoveTracker::Options::isMirrored.
 */
struct TrackedPosition {
    float position[3] = {0.0f, 0.0f, 0.0f}; //!< the orb's center in cm, relative to the lens
    /*! \brief x, y: the orb's center in the image, -1 to 1 from edge to edge (0 in the center); z: position[2] as a
        fraction of the tracking range (0 at the lens, 1 as far as an orb can be tracked). */
    float normalized[3] = {0.0f, 0.0f, 0.0f};
};

/*! \brief Receives the tracked positions from a MoveTracker. */
class AbstractTrackerListener
{
public:
    virtual ~AbstractTrackerListener() = default;

    /*! \brief Called from the tracker's thread after each camera frame, for every controller added.

        \a isFound tells whether the orb was found in this frame; only then is \a position valid.
     */
    virtual void tracked(int id, bool isFound, const TrackedPosition &position) {}
};

/*! \brief Tracks the controllers' orbs with the camera (psmoveapi's tracker), on a thread of its own, and shows the
    camera image in a preview window.

    Controllers are added by id as they connect (from their own threads), and calibrated right away: the tracker
    learns the color of the orb in the camera image, by blinking it. From then on, the tracker chooses the orb's
    color: the controller threads take it from orbColor(). With Options::isHueCalibrating, the first controller to be
    calibrated runs psmoveapi's hue calibration instead, which finds the colors that track well, so the others don't
    need to blink.

    The tracker uses the controllers' handles while calibrating (blinking the orb, and switching the others off), so
    it holds outputMutex() meanwhile; the hub's controller threads skip their output then (MoveHub::setOutputMutex()).
    Otherwise it doesn't use the handles.

    Without OpenCV, a stub is built whose start() fails.
 */
class MoveTracker
{
public:
    struct Options {
        float exposure = 0.3f;         //!< the camera's fixed exposure, 0-1
        bool isMirrored = false;       //!< mirrors the camera image (and x)
        bool isHueCalibrating = false; //!< hue calibration instead of a color calibration per controller
        float fieldOfView = 75.0f;     //!< the lens's diagonal field of view, in degrees: 75 blue dot, 56 red dot
    };

    MoveTracker(AbstractTrackerListener *listener, const Options &options);
    ~MoveTracker();

    MoveTracker(const MoveTracker &) = delete;
    MoveTracker &operator=(const MoveTracker &) = delete;

    std::mutex &outputMutex() { return mOutputMutex; }

    /*! \brief Opens the camera and the preview window, and starts tracking. Fails without a camera. */
    bool start();
    /*! \brief Stops tracking, and closes the camera and the preview window. */
    void stop();

    /*! \brief Starts tracking the controller \a id, after calibrating it. */
    void addController(int id, PSMove *move);
    /*! \brief Stops tracking the controller \a id; once this returns, the tracker doesn't use its handle anymore.

        Waits for an ongoing calibration to finish.
     */
    void removeController(int id);
    /*! \brief Calibrates the controller \a id again (with hue calibration: runs it again, then assigns all colors
        again). */
    void requestCalibration(int id);

    /*! \brief The orb color the tracker chose for controller \a id; false while it isn't calibrated, when the orb
        should be off. Thread-safe. */
    bool orbColor(int id, unsigned char *r, unsigned char *g, unsigned char *b);

private:
    struct Slot {
        PSMove *move = nullptr;
        bool isCalibrationPending = false;
        bool isManualCalibration = false; //!< requested by requestCalibration(), not by joining
        bool isRemoving = false;
        bool isTracked = false; //!< calibrated; the tracker knows it
        unsigned char color[3] = {0, 0, 0};
    };

    void run(std::promise<bool> started);
    void handleRemovals();
    void calibratePending();
    void calibrate(int id, PSMove *move, bool isManual);
    void report();
    TrackedPosition trackedPosition(PSMove *move) const;

    AbstractTrackerListener *mListener;
    Options mOptions;
    std::mutex mOutputMutex;
    std::thread mThread;
    std::atomic<bool> mIsRunning{false};

    // The tracker's thread only:
    PSMoveTracker *mTracker = nullptr;
    int mFrameWidth = 0;
    int mFrameHeight = 0;
    float mFocalLength = 0.0f; //!< in pixels, from the field of view
    float mMaxDistance = 0.0f; //!< in cm: the distance of the smallest orb the tracker accepts
    bool mHasHues = false; //!< the hue calibration found colors

    std::mutex mMutex;                //!< guards mSlots and mIsThreadRunning
    std::condition_variable mChanged; //!< a removal was handled, or the thread ended
    bool mIsThreadRunning = false;
    Slot mSlots[MoveHub::sMaxDevices];
};
