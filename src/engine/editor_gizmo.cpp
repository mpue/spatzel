#include "engine/editor.hpp"

#include "engine/mat4.hpp"

// imgui.h must precede ImGuizmo.h — the gizmo header uses ImVec2/ImGuiID and pulls
// in only imconfig.h itself, not imgui.h.
#include <imgui.h>

#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace engine {

void Editor::buildGizmo(std::vector<GpuPrimitive>& scene, const ViewportCamera& camera,
                        EditorActions& actions) {
    ImGuiIO& io = ImGui::GetIO();

    // Keyboard mode switches, but not while typing in a field. W/E/R belong to the
    // fly camera, so the gizmo uses T (translate) / R (rotate) / Y (dimensions).
    if (!io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_T, false)) m_gizmoMode = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_gizmoMode = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) m_gizmoMode = 2;
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(vp->Pos.x, vp->Pos.y, vp->Size.x, vp->Size.y);

    drawSelectionOutlines(scene, camera);

    // The primitives the gizmo can actually move: selected and not planes (a
    // plane is world-space, so a rigid transform is meaningless for it).
    std::vector<int> xf;
    xf.reserve(m_selection.size());
    for (int idx : m_selection) {
        if (idx >= 0 && idx < static_cast<int>(scene.size()) &&
            scene[static_cast<size_t>(idx)].control[0] !=
                static_cast<int32_t>(PrimitiveType::Plane)) {
            xf.push_back(idx);
        }
    }

    if (!xf.empty()) {
        // Pivot: the centroid of the transformable selection, captured at drag
        // start so a rotate orbits a fixed point rather than a shifting centroid.
        Vec3 centroid{};
        for (int idx : xf) {
            centroid.x += scene[static_cast<size_t>(idx)].position[0];
            centroid.y += scene[static_cast<size_t>(idx)].position[1];
            centroid.z += scene[static_cast<size_t>(idx)].position[2];
        }
        const float inv = 1.0f / static_cast<float>(xf.size());
        centroid       = centroid * inv;
        const Vec3 pivot = m_gizmoUsing ? m_gizmoPivot : centroid;

        ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
        if (m_gizmoMode == 1) {
            op = ImGuizmo::ROTATE;
        } else if (m_gizmoMode == 2) {
            op = ImGuizmo::SCALE;
        }
        const ImGuizmo::MODE mode =
            (op == ImGuizmo::TRANSLATE) ? ImGuizmo::WORLD : ImGuizmo::LOCAL;

        // The gizmo sits at the pivot with identity rotation/scale; ImGuizmo
        // mutates it and reports the per-frame delta, which we apply to every
        // transformable primitive. Held across the drag for ImGuizmo's own state.
        Mat4 model = m_gizmoUsing ? m_gizmoMatrix : translation(pivot);
        Mat4 delta;
        ImGuizmo::Manipulate(camera.view.data(), camera.proj.data(), op, mode, model.data(),
                             delta.data());
        const bool usingNow = ImGuizmo::IsUsing();

        if (usingNow && !m_gizmoUsing) {
            // Rising edge: one undo entry per drag; snapshot each primitive's
            // params so a dimensions drag scales relative to where it began.
            m_preEdit    = snapshot(scene);
            m_gizmoPivot = pivot;
            m_dimStart.clear();
            for (int idx : xf) {
                const GpuPrimitive& p = scene[static_cast<size_t>(idx)];
                m_dimStart.push_back(
                    std::array<float, 4>{p.params[0], p.params[1], p.params[2], p.params[3]});
            }
        }

        if (usingNow) {
            m_gizmoMatrix = model;

            if (op == ImGuizmo::TRANSLATE) {
                const Vec3 t{delta.m[12], delta.m[13], delta.m[14]};
                for (int idx : xf) {
                    scene[static_cast<size_t>(idx)].position[0] += t.x;
                    scene[static_cast<size_t>(idx)].position[1] += t.y;
                    scene[static_cast<size_t>(idx)].position[2] += t.z;
                    // position[3] (smooth-union blend radius) is left untouched.
                }
            } else if (op == ImGuizmo::ROTATE) {
                const Quat rq = quatFromMat4(delta); // this frame's incremental rotation
                for (int idx : xf) {
                    GpuPrimitive& p = scene[static_cast<size_t>(idx)];
                    // Orbit the position around the pivot...
                    const Vec3 rel{p.position[0] - m_gizmoPivot.x, p.position[1] - m_gizmoPivot.y,
                                   p.position[2] - m_gizmoPivot.z};
                    const Vec3 rr = quatRotate(rq, rel);
                    p.position[0] = m_gizmoPivot.x + rr.x;
                    p.position[1] = m_gizmoPivot.y + rr.y;
                    p.position[2] = m_gizmoPivot.z + rr.z;
                    // ...and compose the incremental turn onto its own orientation.
                    Quat q = quatMul(rq, Quat{p.rotation[0], p.rotation[1], p.rotation[2],
                                              p.rotation[3]});
                    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
                    if (len > 1e-6f) {
                        q.x /= len;
                        q.y /= len;
                        q.z /= len;
                        q.w /= len;
                    }
                    // q and -q are the same rotation; keep the sign nearest the
                    // stored one so the DragFloat4 display does not jump.
                    const float d = q.x * p.rotation[0] + q.y * p.rotation[1] +
                                    q.z * p.rotation[2] + q.w * p.rotation[3];
                    if (d < 0.0f) {
                        q.x = -q.x;
                        q.y = -q.y;
                        q.z = -q.z;
                        q.w = -q.w;
                    }
                    p.rotation[0] = q.x;
                    p.rotation[1] = q.y;
                    p.rotation[2] = q.z;
                    p.rotation[3] = q.w;
                }
            } else { // SCALE -> per-object, type-specific dimensions
                const float sx   = length(Vec3{model.m[0], model.m[1], model.m[2]});
                const float sy   = length(Vec3{model.m[4], model.m[5], model.m[6]});
                const float sz   = length(Vec3{model.m[8], model.m[9], model.m[10]});
                const auto  pos_ = [](float v) { return v < 1e-4f ? 1e-4f : v; };
                for (size_t k = 0; k < xf.size(); ++k) {
                    GpuPrimitive&              p  = scene[static_cast<size_t>(xf[k])];
                    const std::array<float, 4> d0 = m_dimStart[k];
                    switch (static_cast<PrimitiveType>(p.control[0])) {
                        case PrimitiveType::Sphere: // uniform: one radius
                            p.params[0] = pos_(d0[0] * (sx + sy + sz) / 3.0f);
                            break;
                        case PrimitiveType::Box: // three half-extents, per axis
                            p.params[0] = pos_(d0[0] * sx);
                            p.params[1] = pos_(d0[1] * sy);
                            p.params[2] = pos_(d0[2] * sz);
                            break;
                        case PrimitiveType::Torus:
                            p.params[0] = pos_(d0[0] * sx); // major
                            p.params[1] = pos_(d0[1] * sy); // minor
                            break;
                        case PrimitiveType::RoundCone:
                            p.params[0] = pos_(d0[0] * sy); // height (local y)
                            p.params[1] = pos_(d0[1] * sx); // base radius
                            p.params[2] = pos_(d0[2] * sz); // tip radius
                            break;
                        case PrimitiveType::Cylinder:
                            p.params[0] = pos_(d0[0] * 0.5f * (sx + sz)); // radius
                            p.params[1] = pos_(d0[1] * sy);               // half-height
                            break;
                        case PrimitiveType::Plane:
                            break; // excluded from xf
                    }
                }
            }
            actions.sceneChanged = true; // live re-upload + unmeasured re-bake
        }

        if (!usingNow && m_gizmoUsing) {
            // Falling edge: commit one undo step and time the resulting re-bake.
            pushUndo(m_preEdit);
            actions.bakeMeasure = true;
        }
        m_gizmoUsing = usingNow;
    } else {
        m_gizmoUsing = false;
    }

    // A left click on empty viewport space — not on the gizmo, not over the panel
    // — asks the engine to pick. Ctrl makes it additive (toggle into a multi-
    // selection). The ray matches the marcher's primary ray (same ndc.y flip).
    if (io.MouseClicked[0] && !io.WantCaptureMouse && !ImGuizmo::IsOver() &&
        !ImGuizmo::IsUsing()) {
        m_pendingPickAdditive = io.KeyCtrl;
        const float nx = (io.MousePos.x / io.DisplaySize.x) * 2.0f - 1.0f;
        float       ny = (io.MousePos.y / io.DisplaySize.y) * 2.0f - 1.0f;
        ny             = -ny;
        const Vec3 dir = normalise(camera.forward +
                                   camera.right * (nx * camera.aspect * camera.tanHalfFov) +
                                   camera.up * (ny * camera.tanHalfFov));
        actions.pickRequested    = true;
        actions.pickRayOrigin[0] = camera.position.x;
        actions.pickRayOrigin[1] = camera.position.y;
        actions.pickRayOrigin[2] = camera.position.z;
        actions.pickRayDir[0]    = dir.x;
        actions.pickRayDir[1]    = dir.y;
        actions.pickRayDir[2]    = dir.z;
    }
}

void Editor::drawSelectionOutlines(const std::vector<GpuPrimitive>& scene,
                                   const ViewportCamera& camera) const {
    if (m_selection.empty()) {
        return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList*          dl = ImGui::GetBackgroundDrawList();
    const Mat4           viewProj = mul(camera.proj, camera.view);
    const int            prim     = primary();

    for (int idx : m_selection) {
        if (idx < 0 || idx >= static_cast<int>(scene.size())) {
            continue;
        }
        const GpuPrimitive& p    = scene[static_cast<size_t>(idx)];
        const auto          type = static_cast<PrimitiveType>(p.control[0]);
        if (type == PrimitiveType::Plane) {
            continue; // infinite: no box to draw
        }

        // A local-space box (centre + half extents) that bounds the primitive.
        Vec3 c{};
        Vec3 h{};
        switch (type) {
            case PrimitiveType::Sphere: {
                const float r = p.params[0];
                h             = {r, r, r};
                break;
            }
            case PrimitiveType::Box:
                h = {p.params[0] + p.params[3], p.params[1] + p.params[3],
                     p.params[2] + p.params[3]};
                break;
            case PrimitiveType::Torus: {
                const float major = p.params[0];
                const float minor = p.params[1];
                h                 = {major + minor, minor, major + minor};
                break;
            }
            case PrimitiveType::RoundCone: {
                const float height = p.params[0];
                const float r0     = p.params[1];
                const float r1     = p.params[2];
                const float rad    = std::max(r0, r1);
                const float ymin   = -r0;        // base sphere bottom
                const float ymax   = height + r1; // tip sphere top
                c                  = {0.0f, 0.5f * (ymin + ymax), 0.0f};
                h                  = {rad, 0.5f * (ymax - ymin), rad};
                break;
            }
            case PrimitiveType::Cylinder: {
                const float rad = p.params[0];
                h               = {rad, p.params[1], rad};
                break;
            }
            case PrimitiveType::Plane:
                continue;
        }

        const Quat q{p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]};
        const Vec3 pos{p.position[0], p.position[1], p.position[2]};

        // The eight corners, projected to screen. Corner bits: 1=x, 2=y, 4=z.
        ImVec2 sc[8];
        bool   ok = true;
        for (int i = 0; i < 8; ++i) {
            const Vec3 local = c + Vec3{(i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y,
                                        (i & 4) ? h.z : -h.z};
            const Vec3 world = pos + quatRotate(q, local);
            const auto clip  = transformVec4(viewProj, world.x, world.y, world.z, 1.0f);
            if (clip[3] <= 1e-4f) { // a corner behind the camera: skip this box
                ok = false;
                break;
            }
            const float nx = clip[0] / clip[3];
            const float ny = clip[1] / clip[3];
            sc[i]          = ImVec2(vp->Pos.x + (nx * 0.5f + 0.5f) * vp->Size.x,
                                    vp->Pos.y + (1.0f - (ny * 0.5f + 0.5f)) * vp->Size.y);
        }
        if (!ok) {
            continue;
        }

        // The primary (last-selected) box is brighter and thicker.
        const bool  isPrimary = (idx == prim);
        const ImU32 col       = isPrimary ? IM_COL32(255, 158, 51, 235)
                                          : IM_COL32(255, 200, 120, 150);
        const float th        = isPrimary ? 2.5f : 1.5f;

        static const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7},  // along x
                                         {0, 2}, {1, 3}, {4, 6}, {5, 7},  // along y
                                         {0, 4}, {1, 5}, {2, 6}, {3, 7}}; // along z
        for (const auto& e : edges) {
            dl->AddLine(sc[e[0]], sc[e[1]], col, th);
        }
    }
}

} // namespace engine
