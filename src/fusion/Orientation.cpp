#include "Orientation.h"

#include <algorithm>
#include <cmath>

namespace {

const float sRadToDeg = 57.29577951f;

} // namespace

EulerAngles toEulerAngles(const Quaternion &q)
{
    // The rotation is R = Rz(yaw) * Rx(pitch) * Ry(roll). Its last row is gravity in the body frame,
    // (-cos(pitch) sin(roll), sin(pitch), cos(pitch) cos(roll)).
    const float r20 = 2.0f * (q.x * q.z - q.w * q.y);
    const float r21 = 2.0f * (q.y * q.z + q.w * q.x);
    const float r22 = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
    const float r01 = 2.0f * (q.x * q.y - q.w * q.z);
    const float r11 = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);

    EulerAngles angles;
    angles.roll = std::atan2(-r20, r22) * sRadToDeg;
    angles.pitch = std::asin(std::clamp(r21, -1.0f, 1.0f)) * sRadToDeg;
    angles.yaw = std::atan2(-r01, r11) * sRadToDeg;
    return angles;
}

Quaternion tiltFromGravity(const Vector3 &gravity)
{
    const float length = std::sqrt(gravity.x * gravity.x + gravity.y * gravity.y + gravity.z * gravity.z);
    if (length <= 0.0f)
        return Quaternion{};

    // Inverse of the gravity row above, with yaw 0: q = qx(pitch) * qy(roll).
    const float pitch = std::asin(std::clamp(gravity.y / length, -1.0f, 1.0f));
    const float roll = std::atan2(-gravity.x, gravity.z);
    const float cp = std::cos(0.5f * pitch);
    const float sp = std::sin(0.5f * pitch);
    const float cr = std::cos(0.5f * roll);
    const float sr = std::sin(0.5f * roll);
    return Quaternion{cp * cr, sp * cr, cp * sr, sp * sr};
}
