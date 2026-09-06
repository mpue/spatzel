#include "engine/editor.hpp"

#include "engine/mat4.hpp"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace engine {
namespace {

// A "nice" tick spacing (1/2/5 * 10^k seconds) giving roughly `targetPx` between
// ruler ticks at the current zoom.
float niceStep(float pxPerSec, float targetPx) {
    const float raw = targetPx / std::max(pxPerSec, 1e-3f);
    const float mag = std::pow(10.0f, std::floor(std::log10(std::max(raw, 1e-6f))));
    const float n   = raw / mag;
    const float m   = n <= 1.0f ? 1.0f : (n <= 2.0f ? 2.0f : (n <= 5.0f ? 5.0f : 10.0f));
    return m * mag;
}

} // namespace

void Editor::buildTimelinePanel(std::vector<GpuPrimitive>& scene, AnimationClip& anim,
                                AnimationState& st, const ViewportCamera& camera) {
    if (!ImGui::Begin("Timeline")) {
        ImGui::End();
        return;
    }
    const float em = ImGui::GetFontSize();

    // --- transport ---------------------------------------------------------
    if (st.playing) {
        if (ImGui::Button("Pause")) {
            st.playing = false;
        }
    } else if (ImGui::Button("Play")) {
        st.playing = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        st.playing = false;
        st.time    = 0.0f;
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &st.loop);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 4.5f);
    ImGui::DragFloat("Speed", &st.speed, 0.05f, 0.05f, 8.0f, "%.2fx");
    ImGui::SameLine();
    ImGui::Checkbox("Ref preview", &st.previewReference);

    // --- settings / actions -----------------------------------------------
    ImGui::SetNextItemWidth(em * 5.0f);
    ImGui::DragFloat("Duration", &anim.duration, 0.05f, 0.1f, 600.0f, "%.2fs");
    // One undo entry per drag, the same activate/commit pattern the Properties
    // panel uses for its number fields.
    if (ImGui::IsItemActivated()) {
        m_preEdit = snapshot(scene);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        pushUndo(m_preEdit);
    }
    anim.duration = std::max(anim.duration, 0.1f);
    st.time       = std::clamp(st.time, 0.0f, anim.duration);
    ImGui::SameLine();
    if (ImGui::Checkbox("Linear", &anim.linear)) {
        // Checkbox writes the flag before it returns, so flip it back for the
        // instant the snapshot is taken.
        anim.linear = !anim.linear;
        pushUndo(scene);
        anim.linear = !anim.linear;
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) {
        m_tlPxPerSec = 0.0f; // re-fit next draw
    }

    const int sel = primary();
    ImGui::SameLine();
    ImGui::BeginDisabled(sel < 0 || sel >= static_cast<int>(scene.size()));
    if (ImGui::Button("Add key")) {
        pushUndo(scene);
        GpuPrimitive& p  = scene[static_cast<size_t>(sel)];
        std::uint32_t id = objectIdOf(p);
        if (id == 0) {
            reseedObjectIds(scene, st);
            id = st.nextId;
            setObjectId(p, id);
        }
        PoseKey key;
        key.time     = st.time;
        key.position = {p.position[0], p.position[1], p.position[2]};
        key.rotation = {p.rotation[0], p.rotation[1], p.rotation[2], p.rotation[3]};
        for (int c = 0; c < 4; ++c) {
            key.dims[c] = p.params[c];
        }
        setPoseKey(anim, id, key);
        m_tlSelObject = id;
        m_tlSelTime   = st.time;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Capture the current viewport camera (position, orientation, fov) as a key.
    if (ImGui::Button("Add cam key")) {
        pushUndo(scene);
        PoseKey key;
        key.time     = st.time;
        key.position = camera.position;
        key.rotation = quatFromCameraBasis(camera.right, camera.up, camera.forward);
        key.dims[0]  = 2.0f * std::atan(camera.tanHalfFov); // vertical fov (radians)
        setPoseKey(anim, kCameraTrackId, key);
        m_tlSelObject = kCameraTrackId;
        m_tlSelTime   = st.time;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(m_tlSelTime < 0.0f);
    if (ImGui::Button("Delete key")) {
        pushUndo(scene);
        removePoseKeyNear(anim, m_tlSelObject, m_tlSelTime);
        m_tlSelTime = -1.0f;
    }
    ImGui::EndDisabled();

    // --- dope sheet --------------------------------------------------------
    const float gutter   = em * 5.5f; // left label column
    const float rulerH   = em * 1.4f;
    const float laneH    = em * 1.5f;

    const ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2       canvas    = ImGui::GetContentRegionAvail();
    canvas.x               = std::max(canvas.x, gutter + em * 4.0f);
    canvas.y               = std::max(canvas.y, rulerH + laneH * 2.0f);

    ImGui::InvisibleButton("##dope", canvas);
    const bool  hovered = ImGui::IsItemHovered();
    ImGuiIO&    io      = ImGui::GetIO();
    ImDrawList* dl      = ImGui::GetWindowDrawList();
    const ImVec2 p1{canvasPos.x + canvas.x, canvasPos.y + canvas.y};

    const float trackX0 = canvasPos.x + gutter;
    const float trackW  = canvas.x - gutter;
    const float trackTop = canvasPos.y + rulerH;

    // Fit the whole clip on first use / after "Fit".
    if (m_tlPxPerSec <= 0.0f) {
        m_tlPxPerSec  = trackW / std::max(anim.duration, 0.1f);
        m_tlViewStart = 0.0f;
    }
    const auto timeToX = [&](float t) { return trackX0 + (t - m_tlViewStart) * m_tlPxPerSec; };
    const auto xToTime = [&](float x) { return m_tlViewStart + (x - trackX0) / m_tlPxPerSec; };

    // Tracks with at least one key, in stable order.
    std::vector<const ObjectTrack*> lanes;
    for (const ObjectTrack& t : anim.tracks) {
        if (!t.keys.empty()) {
            lanes.push_back(&t);
        }
    }
    // --- interaction -------------------------------------------------------
    const ImVec2 m = io.MousePos;
    if (ImGui::IsItemActivated()) {
        bool hitKey = false;
        for (int li = 0; li < static_cast<int>(lanes.size()) && !hitKey; ++li) {
            const float y0 = trackTop + laneH * static_cast<float>(li);
            if (m.y < y0 || m.y > y0 + laneH) {
                continue;
            }
            for (const PoseKey& k : lanes[static_cast<size_t>(li)]->keys) {
                if (std::fabs(timeToX(k.time) - m.x) < 7.0f) {
                    m_tlSelObject = lanes[static_cast<size_t>(li)]->objectId;
                    m_tlSelTime   = k.time;
                    m_tlDrag      = 1;
                    m_tlDragStart = k.time;   // to tell a move from a plain click
                    m_preEdit     = snapshot(scene);
                    hitKey        = true;
                    break;
                }
            }
        }
        if (!hitKey) {
            m_tlDrag = 2; // scrub the playhead
            st.time  = std::clamp(xToTime(m.x), 0.0f, anim.duration);
        }
    }
    if (ImGui::IsItemActive()) {
        if (m_tlDrag == 1 && m_tlSelTime >= 0.0f) {
            const float nt = std::clamp(xToTime(m.x), 0.0f, anim.duration);
            if (ObjectTrack* tr = findTrack(anim, m_tlSelObject)) {
                for (PoseKey& k : tr->keys) {
                    if (std::fabs(k.time - m_tlSelTime) < 1e-3f) {
                        k.time = nt;
                        break;
                    }
                }
                std::sort(tr->keys.begin(), tr->keys.end(),
                          [](const PoseKey& a, const PoseKey& b) { return a.time < b.time; });
            }
            m_tlSelTime = nt;
        } else if (m_tlDrag == 2) {
            st.time = std::clamp(xToTime(m.x), 0.0f, anim.duration);
        }
    }
    if (ImGui::IsItemDeactivated()) {
        // One undo entry per key drag, and none for a click that only selected.
        if (m_tlDrag == 1 && std::fabs(m_tlSelTime - m_tlDragStart) > 1e-6f) {
            pushUndo(m_preEdit);
        }
        m_tlDrag      = 0;
        m_tlDragStart = -1.0f;
    }
    if (hovered) {
        // Wheel zooms the time axis around the cursor; middle-drag pans it.
        if (io.MouseWheel != 0.0f) {
            const float pivot = xToTime(m.x);
            m_tlPxPerSec = std::clamp(m_tlPxPerSec * std::exp(io.MouseWheel * 0.15f), 2.0f,
                                      6000.0f);
            m_tlViewStart = pivot - (m.x - trackX0) / m_tlPxPerSec;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            m_tlViewStart -= io.MouseDelta.x / m_tlPxPerSec;
        }
        if (m_tlSelTime >= 0.0f && ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
            pushUndo(scene);
            removePoseKeyNear(anim, m_tlSelObject, m_tlSelTime);
            m_tlSelTime = -1.0f;
        }
    }

    // --- draw --------------------------------------------------------------
    dl->PushClipRect(canvasPos, p1, true);
    dl->AddRectFilled(canvasPos, p1, IM_COL32(20, 20, 24, 255));
    dl->AddRectFilled(canvasPos, ImVec2(p1.x, canvasPos.y + rulerH), IM_COL32(28, 28, 34, 255));
    dl->AddRectFilled(canvasPos, ImVec2(trackX0, p1.y), IM_COL32(24, 24, 28, 255)); // gutter

    // Ruler ticks + vertical grid.
    const float step   = niceStep(m_tlPxPerSec, em * 4.5f);
    const float tStart = std::floor(std::max(m_tlViewStart, 0.0f) / step) * step;
    for (float t = tStart; timeToX(t) <= p1.x; t += step) {
        const float x = timeToX(t);
        if (x < trackX0) {
            continue;
        }
        dl->AddLine(ImVec2(x, canvasPos.y + rulerH), ImVec2(x, p1.y), IM_COL32(255, 255, 255, 16));
        char lbl[24];
        std::snprintf(lbl, sizeof(lbl), "%g", static_cast<double>(t));
        dl->AddText(ImVec2(x + 2.0f, canvasPos.y + 2.0f), IM_COL32(160, 160, 170, 255), lbl);
    }
    // Duration end marker.
    {
        const float xe = timeToX(anim.duration);
        if (xe >= trackX0 && xe <= p1.x) {
            dl->AddLine(ImVec2(xe, canvasPos.y), ImVec2(xe, p1.y), IM_COL32(255, 90, 90, 90));
        }
    }

    // Lanes, labels, keyframes.
    for (int li = 0; li < static_cast<int>(lanes.size()); ++li) {
        const ObjectTrack* tr  = lanes[static_cast<size_t>(li)];
        const float        y0  = trackTop + laneH * static_cast<float>(li);
        const float        yc  = y0 + laneH * 0.5f;
        const bool         selObj =
            (sel >= 0 && sel < static_cast<int>(scene.size()) &&
             objectIdOf(scene[static_cast<size_t>(sel)]) == tr->objectId);

        if (li % 2 == 1) {
            dl->AddRectFilled(ImVec2(trackX0, y0), ImVec2(p1.x, y0 + laneH),
                              IM_COL32(255, 255, 255, 8));
        }
        if (selObj) {
            dl->AddRectFilled(ImVec2(canvasPos.x, y0), ImVec2(p1.x, y0 + laneH),
                              IM_COL32(245, 158, 51, 26));
        }
        const bool isCam = tr->objectId == kCameraTrackId;
        char       lbl[24];
        if (isCam) {
            std::snprintf(lbl, sizeof(lbl), "Camera");
        } else {
            std::snprintf(lbl, sizeof(lbl), "obj %u", tr->objectId);
        }
        const ImU32 lblCol = isCam    ? IM_COL32(140, 200, 255, 255)
                             : selObj ? IM_COL32(245, 200, 120, 255)
                                      : IM_COL32(180, 180, 190, 255);
        dl->AddText(ImVec2(canvasPos.x + 6.0f, yc - em * 0.5f), lblCol, lbl);

        for (const PoseKey& k : tr->keys) {
            const float x = timeToX(k.time);
            if (x < trackX0 - 8.0f || x > p1.x + 8.0f) {
                continue;
            }
            const bool  isSel = (tr->objectId == m_tlSelObject &&
                                std::fabs(k.time - m_tlSelTime) < 1e-3f);
            const float r     = isSel ? 6.0f : 5.0f;
            const ImVec2 pts[4]{{x, yc - r}, {x + r, yc}, {x, yc + r}, {x - r, yc}};
            dl->AddConvexPolyFilled(pts, 4,
                                    isSel ? IM_COL32(255, 235, 200, 255)
                                          : IM_COL32(240, 180, 90, 255));
            dl->AddPolyline(pts, 4, IM_COL32(30, 25, 15, 220), ImDrawFlags_Closed,
                            isSel ? 2.0f : 1.0f);
        }
    }

    // Playhead.
    {
        const float x = timeToX(st.time);
        if (x >= trackX0 - 1.0f && x <= p1.x + 1.0f) {
            dl->AddLine(ImVec2(x, canvasPos.y), ImVec2(x, p1.y), IM_COL32(255, 170, 70, 230), 1.5f);
            const ImVec2 tri[3]{{x - 5.0f, canvasPos.y}, {x + 5.0f, canvasPos.y},
                                {x, canvasPos.y + 8.0f}};
            dl->AddConvexPolyFilled(tri, 3, IM_COL32(255, 170, 70, 255));
        }
    }
    dl->PopClipRect();

    if (lanes.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(trackX0 + em, trackTop + laneH * 0.5f));
        ImGui::TextDisabled("Select an object and press \"Add key\" to start animating.");
    }

    ImGui::End();
}

} // namespace engine
