#include "engine/scene.hpp"

namespace engine {
namespace {

GpuPrimitive makePrimitive(PrimitiveType type, Operator op, Vec3 position, Vec3 albedo) {
    GpuPrimitive primitive{};
    primitive.position[0] = position.x;
    primitive.position[1] = position.y;
    primitive.position[2] = position.z;
    primitive.albedo[0]   = albedo.x;
    primitive.albedo[1]   = albedo.y;
    primitive.albedo[2]   = albedo.z;
    primitive.control[0]  = static_cast<int32_t>(type);
    primitive.control[1]  = static_cast<int32_t>(op);
    return primitive;
}

void setRotation(GpuPrimitive& primitive, Quat rotation) {
    primitive.rotation[0] = rotation.x;
    primitive.rotation[1] = rotation.y;
    primitive.rotation[2] = rotation.z;
    primitive.rotation[3] = rotation.w;
}

} // namespace

std::vector<GpuPrimitive> buildScene() {
    std::vector<GpuPrimitive> scene;

    // Ground. A plane is unbounded, which is fine when there is no
    // acceleration structure to bound it against.
    GpuPrimitive ground = makePrimitive(PrimitiveType::Plane, Operator::Union, {},
                                        {0.35f, 0.36f, 0.40f});
    ground.params[0] = 0.0f;
    ground.params[1] = 1.0f;
    ground.params[2] = 0.0f;
    ground.params[3] = 0.0f;
    scene.push_back(ground);

    // Two spheres, close enough that the smooth union between them produces a
    // visible neck rather than an intersection.
    GpuPrimitive left = makePrimitive(PrimitiveType::Sphere, Operator::Union,
                                      {-0.75f, 1.0f, 0.0f}, {0.90f, 0.35f, 0.25f});
    left.params[0] = 0.8f;
    scene.push_back(left);

    GpuPrimitive right = makePrimitive(PrimitiveType::Sphere, Operator::SmoothUnion,
                                       {0.75f, 1.15f, 0.0f}, {0.25f, 0.55f, 0.90f});
    right.params[0]   = 0.6f;
    right.position[3] = 0.7f; // blend radius
    scene.push_back(right);

    // A rotated rounded box, joined with a plain union so both operators are
    // exercised side by side.
    GpuPrimitive box = makePrimitive(PrimitiveType::Box, Operator::Union,
                                     {2.4f, 0.7f, -1.2f}, {0.85f, 0.75f, 0.30f});
    box.params[0] = 0.55f;
    box.params[1] = 0.55f;
    box.params[2] = 0.55f;
    box.params[3] = 0.08f; // corner rounding
    setRotation(box, quatFromAxisAngle({0.3f, 1.0f, 0.15f}, 0.9f));
    scene.push_back(box);

    // A torus, upright, off to the other side.
    GpuPrimitive torus = makePrimitive(PrimitiveType::Torus, Operator::Union,
                                       {-2.6f, 1.1f, -0.8f}, {0.45f, 0.80f, 0.45f});
    torus.params[0] = 0.75f;
    torus.params[1] = 0.25f;
    setRotation(torus, quatFromAxisAngle({1.0f, 0.0f, 0.0f}, 1.2f));
    scene.push_back(torus);

    return scene;
}

} // namespace engine
