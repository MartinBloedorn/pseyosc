#pragma once

#include "AbstractOrientationFilter.h"

#include <memory>

enum class FilterType {
    None,    //!< no fusion
    Madgwick //!< MadgwickFilter
};

/*! \brief Parses a --fusion argument: "madg", or "off"/"none". */
bool parseFilterType(const char *name, FilterType *type);

/*! \brief Turns a controller's input reports into an orientation, with one AbstractOrientationFilter.

    It first waits for the controller to lie still for a moment, to measure the gyro's bias, the gravity (which gives
    the initial pitch and roll) and the report rate. The filter then starts with yaw 0, until yaw hints (hintYaw())
    correct it, by shifting a yaw offset on top of the filter's orientation.

    Each report holds two samples. Their time step comes from the report rate measured at rest, and the reports'
    sequence numbers, which count lost reports: the host's time of arrival is uneven, and reports queue up while the
    controller's thread is busy.
 */
class MotionFusion
{
public:
    explicit MotionFusion(FilterType type);

    /*! \brief Starts over: waits for the controller to be still again. */
    void restart();

    /*! \brief Adds one input report.

        \a time is the host's time of arrival in seconds, \a sequence the report's sequence number (1-16), \a gyro
        (deg/s) and \a accel (g) its two samples in the body frame, the older one first.
     */
    void addReport(double time, int sequence, const Vector3 gyro[2], const Vector3 accel[2]);

    /*! \brief Moves the yaw towards \a yaw (deg, as move/rpy's) by the share \a strength (0-1); 1 sets it.

        Ignored until ready, and while pointing nearly straight up or down, where yaw is ill-defined. Returns whether
        it's applied.
     */
    bool hintYaw(float yaw, float strength);

    /*! \brief Whether the bias is measured, and orientation() valid. */
    bool isReady() const { return mIsReady; }
    const FilterSetup &setup() const { return mSetup; }
    /*! \brief The filter's orientation, with the yaw offset. */
    Quaternion orientation() const;

private:
    /*! \brief Collects reports while the controller is still; true once it's been still for long enough. */
    bool collectRest(double time, int reports, const Vector3 gyro[2], const Vector3 accel[2]);

    std::unique_ptr<AbstractOrientationFilter> mFilter;
    bool mIsReady = false;
    bool mHasSequence = false;
    int mLastSequence = 0;
    double mStartTime = -1.0; //!< time of the first report since restart(); -1: none yet
    FilterSetup mSetup;
    float mReportPeriod = 0.0f; //!< seconds, measured at rest
    float mYawOffset = 0.0f;    //!< degrees of yaw added to the filter's, by yaw hints

    // The rest window: sums and ranges of the samples since it began.
    double mWindowStart = 0.0;
    int mWindowReports = 0; //!< reports since the window began, lost ones included
    int mWindowSamples = 0;
    Vector3 mGyroSum, mGyroMin, mGyroMax;
    Vector3 mAccelSum;
    float mAccelMin = 0.0f, mAccelMax = 0.0f; //!< magnitudes
};
