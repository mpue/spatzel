#pragma once

// Backend-internal: handle -> Vulkan object mapping.

#include "rhi/vulkan/vk_common.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace rhi::vulkan {

// ---------------------------------------------------------------------------
// Generational handle pool.
//
// A handle packs a slot index (low 24 bits, stored as index + 1) and a
// generation counter (high 8 bits). Index+1 guarantees a live handle is never
// zero, so H::Invalid stays distinguishable, and the generation makes a stale
// handle detectable after its slot is recycled.
// ---------------------------------------------------------------------------
template <typename T, typename H>
class Pool {
public:
    [[nodiscard]] H insert(T value) {
        uint32_t index = 0;
        if (!m_freeList.empty()) {
            index = m_freeList.back();
            m_freeList.pop_back();
            m_slots[index].value = std::move(value);
            m_slots[index].alive = true;
        } else {
            if (m_slots.size() >= kMaxSlots) {
                throw std::runtime_error("rhi: resource pool exhausted");
            }
            index = static_cast<uint32_t>(m_slots.size());
            m_slots.push_back(Slot{std::move(value), 1, true});
        }
        return pack(index, m_slots[index].generation);
    }

    [[nodiscard]] T* find(H handle) {
        if (handle == H::Invalid) {
            return nullptr;
        }
        const uint32_t index      = indexOf(handle);
        const uint8_t  generation = generationOf(handle);
        if (index >= m_slots.size()) {
            return nullptr;
        }
        Slot& slot = m_slots[index];
        if (!slot.alive || slot.generation != generation) {
            return nullptr;
        }
        return &slot.value;
    }

    [[nodiscard]] T& get(H handle) {
        T* value = find(handle);
        if (value == nullptr) {
            throw std::runtime_error("rhi: use of an invalid or stale handle");
        }
        return *value;
    }

    // Returns the removed value so the caller can queue the Vulkan objects for
    // deferred destruction. Throws on a stale handle — a double destroy is a
    // programming error, not a recoverable condition.
    [[nodiscard]] T remove(H handle) {
        T value = std::move(get(handle));
        Slot& slot = m_slots[indexOf(handle)];
        slot.alive = false;
        slot.generation = static_cast<uint8_t>(slot.generation + 1 == 0 ? 1 : slot.generation + 1);
        m_freeList.push_back(indexOf(handle));
        return value;
    }

    // Iterates the still-live entries. Used at shutdown to catch leaks.
    template <typename Fn>
    void forEachAlive(Fn&& fn) {
        for (Slot& slot : m_slots) {
            if (slot.alive) {
                fn(slot.value);
            }
        }
    }

    [[nodiscard]] size_t aliveCount() const {
        size_t count = 0;
        for (const Slot& slot : m_slots) {
            count += slot.alive ? 1 : 0;
        }
        return count;
    }

private:
    static constexpr uint32_t kIndexBits = 24;
    static constexpr uint32_t kIndexMask = (1u << kIndexBits) - 1u;
    static constexpr size_t   kMaxSlots  = kIndexMask - 1u;

    struct Slot {
        T       value{};
        uint8_t generation = 1;
        bool    alive      = false;
    };

    static H pack(uint32_t index, uint8_t generation) {
        return static_cast<H>(((index + 1u) & kIndexMask) |
                              (static_cast<uint32_t>(generation) << kIndexBits));
    }
    static uint32_t indexOf(H handle) {
        return (static_cast<uint32_t>(handle) & kIndexMask) - 1u;
    }
    static uint8_t generationOf(H handle) {
        return static_cast<uint8_t>(static_cast<uint32_t>(handle) >> kIndexBits);
    }

    std::vector<Slot>     m_slots;
    std::vector<uint32_t> m_freeList;
};

// ---------------------------------------------------------------------------
// Resource payloads
// ---------------------------------------------------------------------------
struct Texture {
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkExtent3D    extent     = {};
    VkFormat      format     = VK_FORMAT_UNDEFINED;

    // Tracked resource state, updated by the command list as it inserts
    // barriers. Never visible above the seam.
    VkImageLayout         layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage  = VK_PIPELINE_STAGE_2_NONE;
    VkAccessFlags2        access = VK_ACCESS_2_NONE;
};

struct Buffer {
    VkBuffer          buffer     = VK_NULL_HANDLE;
    VmaAllocation     allocation = VK_NULL_HANDLE;
    VmaAllocationInfo info       = {};
    VkDeviceSize      size       = 0;
    bool              hostVisible = false;
};

struct Shader {
    VkShaderModule module = VK_NULL_HANDLE;
};

struct Pipeline {
    VkPipeline            pipeline         = VK_NULL_HANDLE;
    VkPipelineLayout      layout           = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout        = VK_NULL_HANDLE;
    uint32_t              pushConstantSize = 0;
    bool                  hasBindings      = false;
};

} // namespace rhi::vulkan
