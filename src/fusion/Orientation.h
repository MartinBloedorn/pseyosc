#pragma once

/*! \brief A vector in the controller's body frame, or in the world frame.

    Body frame: x to the right, y along the controller towards the orb, z out of the face with the buttons (seen
    from behind, pointing forward, buttons up). It's right-handed, and gyro rates are right-handed about its axes.
 */
struct Vector3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/*! \brief Rotation from the controller's body frame to the world frame (z up, yaw 0 = heading at calibration). */
struct Quaternion {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

/*! \brief Angles in degrees, each a right-handed rotation about the body frame's axes.

    Rotation order: yaw about the world's vertical (positive turning left), then pitch about the controller's x axis
    (positive pointing up), then roll about its y axis, its long axis (positive rolling right, i.e. clockwise seen from
    behind). Yaw and roll are undefined while pointing straight up or down.
 */
struct EulerAngles {
    float roll = 0.0f;
    float pitch = 0.0f;
    float yaw = 0.0f;
};

EulerAngles toEulerAngles(const Quaternion &q);

/*! \brief The orientation (with yaw 0) of a controller at rest that measures \a gravity (in g, any length). */
Quaternion tiltFromGravity(const Vector3 &gravity);
