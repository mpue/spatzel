#include "rhi/vulkan/vk_device.hpp"

#include <algorithm>
#include <stdexcept>

namespace rhi::vulkan {

void VulkanCommandList::reset(VkCommandBuffer cmd, VkDescriptorPool descriptorPool) {
    m_cmd            = cmd;
    m_descriptorPool = descriptorPool;
    m_pipeline       = PipelineHandle::Invalid;
    m_bindings.clear();
    m_bindingsDirty = false;
}

void VulkanCommandList::bindComputePipeline(PipelineHandle handle) {
    const Pipeline& pipeline = m_device.m_pipelines.get(handle);
    vkCmdBindPipeline(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    m_pipeline      = handle;
    m_bindingsDirty = true;
}

void VulkanCommandList::pushConstants(std::span<const std::byte> data) {
    const Pipeline& pipeline = m_device.m_pipelines.get(m_pipeline);
    if (data.size() > pipeline.pushConstantSize) {
        throw std::runtime_error("rhi: pushConstants exceeds the pipeline's push constant size");
    }
    vkCmdPushConstants(m_cmd, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       static_cast<uint32_t>(data.size()), data.data());
}

void VulkanCommandList::setBinding(PendingBinding binding) {
    auto existing = std::find_if(m_bindings.begin(), m_bindings.end(),
                                 [&](const PendingBinding& b) { return b.slot == binding.slot; });
    if (existing != m_bindings.end()) {
        *existing = binding;
    } else {
        m_bindings.push_back(binding);
    }
    m_bindingsDirty = true;
}

void VulkanCommandList::bindStorageTexture(uint32_t slot, TextureHandle texture) {
    setBinding({.slot = slot, .type = BindingType::StorageTexture, .texture = texture});
}

void VulkanCommandList::bindStorageBuffer(uint32_t slot, BufferHandle buffer) {
    setBinding({.slot = slot, .type = BindingType::StorageBuffer, .buffer = buffer});
}

void VulkanCommandList::flushBindings() {
    const Pipeline& pipeline = m_device.m_pipelines.get(m_pipeline);

    // Resource state first. Emitted before every dispatch, not just when a
    // binding changed, so back-to-back dispatches on the same image get their
    // write-after-write dependency.
    for (const PendingBinding& binding : m_bindings) {
        if (binding.type != BindingType::StorageTexture) {
            continue;
        }
        Texture& texture = m_device.m_textures.get(binding.texture);
        m_device.transitionTexture(m_cmd, texture, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                   VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    if (!pipeline.hasBindings || !m_bindingsDirty) {
        return;
    }

    const VkDescriptorSetAllocateInfo allocInfo{
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext              = nullptr,
        .descriptorPool     = m_descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &pipeline.setLayout,
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    FITZEL_CHECK(vkAllocateDescriptorSets(m_device.m_device.device, &allocInfo, &set));

    // The infos must outlive vkUpdateDescriptorSets, hence the reserved
    // vectors rather than per-iteration temporaries.
    std::vector<VkDescriptorImageInfo>  imageInfos;
    std::vector<VkDescriptorBufferInfo> bufferInfos;
    std::vector<VkWriteDescriptorSet>   writes;
    imageInfos.reserve(m_bindings.size());
    bufferInfos.reserve(m_bindings.size());
    writes.reserve(m_bindings.size());

    for (const PendingBinding& binding : m_bindings) {
        VkWriteDescriptorSet write{
            .sType            = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext            = nullptr,
            .dstSet           = set,
            .dstBinding       = binding.slot,
            .dstArrayElement  = 0,
            .descriptorCount  = 1,
            .descriptorType   = toVkDescriptorType(binding.type),
            .pImageInfo       = nullptr,
            .pBufferInfo      = nullptr,
            .pTexelBufferView = nullptr,
        };

        if (binding.type == BindingType::StorageTexture) {
            const Texture& texture = m_device.m_textures.get(binding.texture);
            imageInfos.push_back(VkDescriptorImageInfo{
                .sampler     = VK_NULL_HANDLE,
                .imageView   = texture.view,
                .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            });
            write.pImageInfo = &imageInfos.back();
        } else {
            const Buffer& buffer = m_device.m_buffers.get(binding.buffer);
            bufferInfos.push_back(VkDescriptorBufferInfo{
                .buffer = buffer.buffer,
                .offset = 0,
                .range  = buffer.size,
            });
            write.pBufferInfo = &bufferInfos.back();
        }
        writes.push_back(write);
    }

    vkUpdateDescriptorSets(m_device.m_device.device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
    vkCmdBindDescriptorSets(m_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &set, 0,
                            nullptr);
    m_bindingsDirty = false;
}

void VulkanCommandList::dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    flushBindings();
    vkCmdDispatch(m_cmd, gx, gy, gz);
}

void VulkanCommandList::blitToSwapchain(TextureHandle src) {
    Texture& texture = m_device.m_textures.get(src);

    m_device.transitionTexture(m_cmd, texture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    m_device.transitionSwapchainImage(m_cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                      VK_PIPELINE_STAGE_2_BLIT_BIT,
                                      VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkExtent2D target = m_device.m_swapchain->extent();
    const VkImageBlit region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffsets     = {{0, 0, 0},
                           {static_cast<int32_t>(texture.extent.width),
                            static_cast<int32_t>(texture.extent.height), 1}},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffsets     = {{0, 0, 0},
                           {static_cast<int32_t>(target.width),
                            static_cast<int32_t>(target.height), 1}},
    };
    vkCmdBlitImage(m_cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   m_device.m_swapchain->image(m_device.m_imageIndex),
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
}

} // namespace rhi::vulkan
