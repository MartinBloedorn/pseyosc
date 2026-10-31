#include "MotionFusion.h"

#include "MadgwickFilter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

/*! \brief Reports are ignored this long (s) after a restart: those that queued up while connecting arrive at once,
    and would distort the measured report rate.
 */
const double sSettleDuration = 0.5;
/*! \brief How long the controller must be still at the start. */
const double sRestDuration = 1.0;
/*! \brief The most each gyro axis (deg/s) and the acceleration's magnitude (g) may vary while still. */
const float sRestGyroSpread = 4.0f;
const float sRestAccelSpread = 0.1f;
/*! \brief psmove_poll()'s sequence numbers count from 1 to 16. */
const int sSequenceLength = 16;

float magnitude(const Vector3 &v)
{
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

} // namespace

bool parseFilterType(const char *name, FilterType *type)
{
    if (std::strcmp(name, "madg") == 0)
        *type = FilterType::Madgwick;
    else if (std::strcmp(name, "off") == 0 || std::strcmp(name, "none") == 0)
        *type = FilterType::None;
    else
        return false;
    return true;
}

MotionFusion::MotionFusion(FilterType type)
{
    if (type == FilterType::Madgwick)
        mFilter = std::make_unique<MadgwickFilter>();
}

void MotionFusion::restart()
{
    mIsReady = false;
    mHasSequence = false;
    mStartTime = -1.0;
    mWindowSamples = 0;
}

void MotionFusion::addReport(double time, int sequence, const Vector3 gyro[2], const Vector3 accel[2])
{
    if (!mFilter)
        return;

    // Reports since the previous one: 1, unless some were lost (up to 15 can be told apart).
    int reports = 1;
    if (mHasSequence) {
        reports = (sequence - mLastSequence + sSequenceLength) % sSequenceLength;
        if (reports == 0)
            reports = sSequenceLength;
    }
    mHasSequence = true;
    mLastSequence = sequence;

    if (!mIsReady) {
        if (mStartTime < 0.0)
            mStartTime = time;
        if (time - mStartTime < sSettleDuration)
            return;
        if (collectRest(time, reports, gyro, accel)) {
            mFilter->start(mSetup);
            mIsReady = true;
        }
        return;
    }

    // The two samples are half a report period apart; lost reports lengthen the first step.
    const float halfPeriod = 0.5f * mReportPeriod;
    for (int i = 0; i < 2; ++i) {
        MotionSample sample;
        sample.dt = i == 0 ? reports * mReportPeriod - halfPeriod : halfPeriod;
        sample.gyro = gyro[i];
        sample.accel = accel[i];
        mFilter->update(sample);
    }
}

Quaternion MotionFusion::orientation() const
{
    if (!mIsReady)
        return Quaternion{};
    return mFilter->orientation();
}

bool MotionFusion::collectRest(double time, int reports, const Vector3 gyro[2], const Vector3 accel[2])
{
    if (mWindowSamples == 0) {
        mWindowStart = time;
        mWindowReports = 0;
        mGyroSum = mAccelSum = Vector3{};
        mGyroMin = mGyroMax = gyro[0];
        mAccelMin = mAccelMax = magnitude(accel[0]);
    } else {
        mWindowReports += reports;
    }
    for (int i = 0; i < 2; ++i) {
        const Vector3 &g = gyro[i];
        const Vector3 &a = accel[i];
        const float accelMagnitude = magnitude(a);
        ++mWindowSamples;
        mGyroSum = Vector3{mGyroSum.x + g.x, mGyroSum.y + g.y, mGyroSum.z + g.z};
        mAccelSum = Vector3{mAccelSum.x + a.x, mAccelSum.y + a.y, mAccelSum.z + a.z};
        mGyroMin = Vector3{std::min(mGyroMin.x, g.x), std::min(mGyroMin.y, g.y), std::min(mGyroMin.z, g.z)};
        mGyroMax = Vector3{std::max(mGyroMax.x, g.x), std::max(mGyroMax.y, g.y), std::max(mGyroMax.z, g.z)};
        mAccelMin = std::min(mAccelMin, accelMagnitude);
        mAccelMax = std::max(mAccelMax, accelMagnitude);
    }

    // Any movement starts the window over. (A constant slow rotation can't be told apart from bias.)
    const bool isMoving = mGyroMax.x - mGyroMin.x > sRestGyroSpread || mGyroMax.y - mGyroMin.y > sRestGyroSpread
                          || mGyroMax.z - mGyroMin.z > sRestGyroSpread || mAccelMax - mAccelMin > sRestAccelSpread;
    if (isMoving) {
        mWindowSamples = 0;
        return false;
    }

    const double duration = time - mWindowStart;
    if (duration < sRestDuration || mWindowReports == 0)
        return false;

    const float count = static_cast<float>(mWindowSamples);
    mSetup.gyroBias = Vector3{mGyroSum.x / count, mGyroSum.y / count, mGyroSum.z / count};
    mSetup.gravity = Vector3{mAccelSum.x / count, mAccelSum.y / count, mAccelSum.z / count};
    mReportPeriod = static_cast<float>(duration / mWindowReports);
    mSetup.sampleRate = 2.0f / mReportPeriod;
    mWindowSamples = 0;
    return true;
}
