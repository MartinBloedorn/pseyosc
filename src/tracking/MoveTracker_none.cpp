// MoveTracker without OpenCV (psmoveapi's tracker isn't built): start() fails.

#include "MoveTracker.h"

#include <cstdio>

MoveTracker::MoveTracker(AbstractTrackerListener *listener, const Options &options)
    : mListener(listener)
    , mOptions(options)
{
}

MoveTracker::~MoveTracker() = default;

bool MoveTracker::start()
{
    std::fprintf(stderr, "tracking isn't available: pseyosc was built without OpenCV (see the README)\n");
    return false;
}

void MoveTracker::stop() {}
void MoveTracker::addController(int, PSMove *) {}
void MoveTracker::removeController(int) {}
void MoveTracker::requestCalibration(int) {}

bool MoveTracker::orbColor(int, unsigned char *, unsigned char *, unsigned char *)
{
    return false;
}
