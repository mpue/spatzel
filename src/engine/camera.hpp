#pragma once

// Free-fly camera. Reads the platform layer's neutral input snapshot; knows
// nothing about GLFW, and nothing about the renderer.

#include "engine/math.hpp"
#include "platform/window.hpp"

namespace engine {

class FlyCamera {
public:
    void update(const platform::InputState& input, float deltaSeconds);

    [[nodiscard]] Vec3  position() const { return m_position; }
    [[nodiscard]] Vec3  forward() const;
    [[nodiscard]] Vec3  right() const;
    [[nodiscard]] Vec3  up() const;
    [[nodiscard]] float verticalFovRadians() const { return m_verticalFov; }

private:
    // Yaw rotates about world up, pitch about the camera's own right axis.
    // Storing angles rather than a basis keeps roll structurally impossible,
    // which is what makes a fly camera feel stable.
    Vec3  m_position    = {0.0f, 1.4f, 5.0f};
    float m_yaw         = 0.0f;
    float m_pitch       = -0.12f;
    float m_verticalFov = 1.0472f; // 60 degrees

    float m_moveSpeed   = 4.0f;
    float m_boostFactor = 4.0f;
    float m_sensitivity = 0.0022f; // radians per pixel
};

} // namespace engine
