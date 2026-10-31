#include "MoveTracker.h"

#include <psmove.h>
#include <psmove_tracker.h>
#include <psmove_tracker_opencv.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

const char sWindowName[] = "pseyosc-stream: tracking";
const float sDegreesToRadians = 0.01745329252f;

} // namespace

MoveTracker::MoveTracker(AbstractTrackerListener *listener, const Options &options)
    : mListener(listener)
    , mOptions(options)
{
}

MoveTracker::~MoveTracker()
{
    stop();
}

bool MoveTracker::start()
{
    std::promise<bool> started;
    std::future<bool> isStarted = started.get_future();
    mIsRunning = true;
    mThread = std::thread(&MoveTracker::run, this, std::move(started));
    if (isStarted.get())
        return true;
    stop();
    return false;
}

void MoveTracker::stop()
{
    mIsRunning = false;
    if (mThread.joinable())
        mThread.join();
}

void MoveTracker::addController(int id, PSMove *move)
{
    std::lock_guard<std::mutex> lock(mMutex);
    Slot &slot = mSlots[id];
    slot = Slot();
    slot.move = move;
    slot.isCalibrationPending = true;
}

void MoveTracker::removeController(int id)
{
    std::unique_lock<std::mutex> lock(mMutex);
    Slot &slot = mSlots[id];
    if (slot.move && mIsThreadRunning) {
        // The tracker's thread lets go of the handle between frames (or calibrations).
        slot.isRemoving = true;
        mChanged.wait(lock, [&] { return !slot.move || !mIsThreadRunning; });
    }
    slot = Slot();
}

void MoveTracker::requestCalibration(int id)
{
    std::lock_guard<std::mutex> lock(mMutex);
    Slot &slot = mSlots[id];
    if (slot.move && !slot.isRemoving) {
        slot.isCalibrationPending = true;
        slot.isManualCalibration = true;
    }
}

bool MoveTracker::orbColor(int id, unsigned char *r, unsigned char *g, unsigned char *b)
{
    std::lock_guard<std::mutex> lock(mMutex);
    const Slot &slot = mSlots[id];
    if (!slot.move || !slot.isTracked)
        return false;
    *r = slot.color[0];
    *g = slot.color[1];
    *b = slot.color[2];
    return true;
}

void MoveTracker::run(std::promise<bool> started)
{
    PSMoveTrackerSettings settings;
    psmove_tracker_settings_set_default(&settings);
    settings.camera_exposure = mOptions.exposure;
    settings.camera_mirror = mOptions.isMirrored;
    mTracker = psmove_tracker_new_with_settings(&settings);
    if (!mTracker) {
        std::fprintf(stderr, "tracking: could not open the camera\n");
        started.set_value(false);
        return;
    }
    const PSMoveCameraInfo *camera = psmove_tracker_get_camera_info(mTracker);
    mFrameWidth = camera->width;
    mFrameHeight = camera->height;
    // Pinhole camera: half the image's diagonal is seen at half the diagonal field of view.
    const float halfDiagonal = 0.5f * std::hypot(static_cast<float>(mFrameWidth), static_cast<float>(mFrameHeight));
    mFocalLength = halfDiagonal / std::tan(0.5f * mOptions.fieldOfView * sDegreesToRadians);
    // Orbs with a smaller radius aren't tracked (psmoveapi's quality check), so this is the tracking range.
    mMaxDistance = psmove_tracker_distance_from_radius(mTracker, settings.tracker_quality_t3);
    std::printf("tracking range: %.0f cm\n", mMaxDistance);
    std::printf("tracking with %s, %dx%d\n", camera->camera_name, mFrameWidth, mFrameHeight);
    cv::namedWindow(sWindowName);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mIsThreadRunning = true;
    }
    started.set_value(true);

    while (mIsRunning) {
        handleRemovals();
        calibratePending();

        psmove_tracker_update_image(mTracker);
        psmove_tracker_update(mTracker, nullptr);
        report();

        psmove_tracker_annotate(mTracker, true, false);
        if (IplImage *frame = psmove_tracker_opencv_get_frame(mTracker))
            cv::imshow(sWindowName, cv::cvarrToMat(frame));
        cv::waitKey(1); // lets the window process its events
    }

    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (Slot &slot : mSlots) {
            if (slot.isTracked)
                psmove_tracker_disable(mTracker, slot.move);
            slot.isTracked = false;
        }
        mIsThreadRunning = false;
    }
    mChanged.notify_all();
    psmove_tracker_free(mTracker);
    mTracker = nullptr;
    cv::destroyWindow(sWindowName);
}

void MoveTracker::handleRemovals()
{
    bool isChanged = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (Slot &slot : mSlots) {
            if (!slot.isRemoving)
                continue;
            if (slot.isTracked)
                psmove_tracker_disable(mTracker, slot.move);
            slot = Slot();
            isChanged = true;
        }
    }
    if (isChanged)
        mChanged.notify_all();
}

void MoveTracker::calibratePending()
{
    while (mIsRunning) {
        int id = -1;
        PSMove *move = nullptr;
        bool isManual = false;
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (int i = 0; i < MoveHub::sMaxDevices && id < 0; ++i) {
                Slot &slot = mSlots[i];
                if (slot.move && slot.isCalibrationPending && !slot.isRemoving) {
                    id = i;
                    move = slot.move;
                    isManual = slot.isManualCalibration;
                    slot.isCalibrationPending = false;
                    slot.isManualCalibration = false;
                }
            }
        }
        if (id < 0)
            return;
        calibrate(id, move, isManual);
        handleRemovals(); // controllers that left meanwhile
    }
}

void MoveTracker::calibrate(int id, PSMove *move, bool isManual)
{
    // The tracker blinks this orb, and switches the tracked ones off: the controller threads keep out meanwhile.
    std::lock_guard<std::mutex> outputLock(mOutputMutex);
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mSlots[id].isTracked)
            psmove_tracker_disable(mTracker, move);
        mSlots[id].isTracked = false;
    }

    if (mOptions.isHueCalibrating && (!mHasHues || isManual)) {
        std::printf("[%d] hue calibration, keep the orb visible to the camera...\n", id);
        // The colors of all controllers are chosen again from the new hues.
        {
            std::lock_guard<std::mutex> lock(mMutex);
            for (Slot &slot : mSlots) {
                if (slot.isTracked) {
                    psmove_tracker_disable(mTracker, slot.move);
                    slot.isTracked = false;
                    slot.isCalibrationPending = true;
                }
            }
        }
        psmove_tracker_reset_color_calibration(mTracker);
        const int hueCount = psmove_tracker_hue_calibration(mTracker, move);
        mHasHues = hueCount > 0;
        std::printf("[%d] hue calibration found %d colors%s\n", id, hueCount,
                    mHasHues ? "" : ", calibrating each controller by blinking instead");
    } else if (isManual) {
        // Otherwise psmoveapi reuses the color it remembers from the last calibration, without blinking.
        psmove_tracker_reset_color_calibration(mTracker);
        mHasHues = false;
    }

    std::printf("[%d] calibrating, keep the orb visible to the camera...\n", id);
    if (psmove_tracker_enable(mTracker, move) != Tracker_CALIBRATED) {
        std::printf("[%d] calibration failed: make sure the orb is visible, and there are no other bright lights in "
                    "view; press Select and Start together to retry\n",
                    id);
        return;
    }
    // From now on, the controller's thread sets the orb's color.
    psmove_tracker_set_auto_update_leds(mTracker, move, false);
    unsigned char r = 0, g = 0, b = 0;
    psmove_tracker_get_color(mTracker, move, &r, &g, &b);
    std::lock_guard<std::mutex> lock(mMutex);
    Slot &slot = mSlots[id];
    slot.isTracked = true;
    slot.color[0] = r;
    slot.color[1] = g;
    slot.color[2] = b;
    std::printf("[%d] tracking, orb color %d %d %d\n", id, r, g, b);
}

void MoveTracker::report()
{
    PSMove *moves[MoveHub::sMaxDevices] = {};
    bool isTracked[MoveHub::sMaxDevices] = {};
    {
        std::lock_guard<std::mutex> lock(mMutex);
        for (int id = 0; id < MoveHub::sMaxDevices; ++id) {
            if (mSlots[id].move && !mSlots[id].isRemoving) {
                moves[id] = mSlots[id].move;
                isTracked[id] = mSlots[id].isTracked;
            }
        }
    }
    for (int id = 0; id < MoveHub::sMaxDevices; ++id) {
        if (!moves[id] || !mListener)
            continue;
        const bool isFound = isTracked[id] && psmove_tracker_get_status(mTracker, moves[id]) == Tracker_TRACKING;
        mListener->tracked(id, isFound, isFound ? trackedPosition(moves[id]) : TrackedPosition());
    }
}

TrackedPosition MoveTracker::trackedPosition(PSMove *move) const
{
    float u, v, radius;
    psmove_tracker_get_position(mTracker, move, &u, &v, &radius);
    if (mOptions.isMirrored)
        u = mFrameWidth - u; // the camera mirrored the image; the position stays the same
    // Offsets from the image's center: an unmirrored image shows the camera's left on the left.
    const float offsetX = 0.5f * mFrameWidth - u;
    const float offsetY = 0.5f * mFrameHeight - v;

    // Pinhole camera: the ray through the pixel, along which lies psmoveapi's distance (estimated from the radius).
    const float dx = offsetX / mFocalLength;
    const float dy = offsetY / mFocalLength;
    const float distance = psmove_tracker_distance_from_radius(mTracker, radius);
    const float scale = distance / std::sqrt(dx * dx + dy * dy + 1.0f);

    TrackedPosition position;
    position.position[0] = dx * scale;
    position.position[1] = dy * scale;
    position.position[2] = scale;
    position.normalized[0] = offsetX / (0.5f * mFrameWidth);
    position.normalized[1] = offsetY / (0.5f * mFrameHeight);
    position.normalized[2] = std::min(scale / mMaxDistance, 1.0f);
    return position;
}
