#pragma once

namespace engine {

// Which renderer produces the frame. The reference is the brute-force marcher
// that defines correct; the brick renderer is the accelerated path validated
// against it. Switchable at runtime, from a hotkey or the editor panel.
enum class RendererMode {
    Brick,     // accelerated: marches the baked brick structure
    Reference, // brute force: evaluates the whole edit list per step
};

} // namespace engine
