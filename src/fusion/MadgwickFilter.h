#pragma once

#include "AbstractOrientationFilter.h"

#include <memory>

/*! \brief Madgwick's revised AHRS (xioTechnologies Fusion), without magnetometer.

    The accelerometer corrects pitch and roll; yaw comes from the gyro alone and drifts. The gyro's bias is tracked
    whenever the controller is still (FusionBias), starting from the one measured at rest. The accelerometer is
    ignored while its magnitude is far from 1 g (swings, shakes), and trusted strongly while the controller is still,
    so the tilt settles quickly after motion. Beyond the gyro's range (if known), it relies on the accelerometer more
    for a moment.

    Taken from wiimosc (src/fusion), where it was tuned on a Wiimote Plus.
 */
class MadgwickFilter : public AbstractOrientationFilter
{
public:
    MadgwickFilter();
    ~MadgwickFilter() override;

    void start(const FilterSetup &setup) override;
    void update(const MotionSample &sample) override;
    Quaternion orientation() const override;

private:
    // Fusion's headers are C99, which C++ before C++20 can't include; only the .cpp does.
    struct State;
    std::unique_ptr<State> mState;
};
