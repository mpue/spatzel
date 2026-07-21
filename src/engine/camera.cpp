#include "engine/camera.hpp"

#include <algorithm>

namespace engine {
namespace {

constexpr Vec3 kWorldUp = {0.0f, 1.0f, 0.0f};

// Just short of straight up/down. Exactly vertical would make the right axis
// degenerate, and the basis would spin.
constexpr float kPitchLimit = 1.5533f; // 89 degrees

} // namespace

Vec3 FlyCamera::forward() const {
    const float cosPitch = std::cos(m_pitch);
    // Yaw of zero looks down -Z, the usual right-handed, Y-up convention.
    return {std::sin(m_yaw) * cosPitch, std::sin(m_pitch), -std::cos(m_yaw) * cosPitch};
}

Vec3 FlyCamera::right() const { return normalise(cross(forward(), kWorldUp)); }

Vec3 FlyCamera::up() const { return cross(right(), forward()); }

void FlyCamera::update(const platform::InputState& input, float deltaSeconds) {
    // Look. The delta is already zero unless the cursor is captured, so no
    // check for the right mouse button is needed here.
    m_yaw += static_cast<float>(input.cursorDeltaX()) * m_sensitivity;
    m_pitch -= static_cast<float>(input.cursorDeltaY()) * m_sensitivity;
    m_pitch = std::clamp(m_pitch, -kPitchLimit, kPitchLimit);

    // Move. Horizontal motion follows the view; vertical follows the world, so
    // looking down does not drag the camera into the floor.
    Vec3 direction{};
    if (input.isDown(platform::Key::W)) direction += forward();
    if (input.isDown(platform::Key::S)) direction += forward() * -1.0f;
    if (input.isDown(platform::Key::D)) direction += right();
    if (input.isDown(platform::Key::A)) direction += right() * -1.0f;
    if (input.isDown(platform::Key::E)) direction += kWorldUp;
    if (input.isDown(platform::Key::Q)) direction += kWorldUp * -1.0f;

    if (length(direction) > 0.0f) {
        const float speed =
            m_moveSpeed * (input.isDown(platform::Key::LeftShift) ? m_boostFactor : 1.0f);
        m_position += normalise(direction) * (speed * deltaSeconds);
    }
}

} // namespace engine
