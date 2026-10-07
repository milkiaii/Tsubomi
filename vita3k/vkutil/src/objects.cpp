// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include "vkutil/objects.h"

#include "vkutil/vkutil.h"

#include <util/align.h>
#include <util/log.h>

#include <atomic>

namespace vkutil {

static vma::Allocator allocator = nullptr;
static std::atomic_uint64_t live_vertex_buffer_bytes{ 0 };
static std::atomic_uint64_t live_index_buffer_bytes{ 0 };
static std::atomic_uint64_t live_uniform_buffer_bytes{ 0 };
static std::atomic_uint64_t live_staging_buffer_bytes{ 0 };
static std::atomic_uint64_t live_other_buffer_bytes{ 0 };
static std::atomic_uint64_t live_image_count{ 0 };
static std::atomic_uint64_t live_image_bytes{ 0 };

static std::atomic_uint64_t &buffer_bytes(BufferAllocationCategory category) {
    switch (category) {
    case BufferAllocationCategory::Vertex:
        return live_vertex_buffer_bytes;
    case BufferAllocationCategory::Index:
        return live_index_buffer_bytes;
    case BufferAllocationCategory::Uniform:
        return live_uniform_buffer_bytes;
    case BufferAllocationCategory::Staging:
        return live_staging_buffer_bytes;
    default:
        return live_other_buffer_bytes;
    }
}

static void remove_buffer_allocation(std::uint64_t size, BufferAllocationCategory category) {
    buffer_bytes(category).fetch_sub(size, std::memory_order_relaxed);
}

static void remove_image_allocation(std::uint64_t size) {
    live_image_bytes.fetch_sub(size, std::memory_order_relaxed);
    live_image_count.fetch_sub(1, std::memory_order_relaxed);
}

void init(vma::Allocator vma_allocator) {
    allocator = vma_allocator;
}

void deinit() {
    allocator = nullptr;
}

BufferAllocationStats buffer_allocation_stats() {
    return {
        .vertex_bytes = live_vertex_buffer_bytes.load(std::memory_order_relaxed),
        .index_bytes = live_index_buffer_bytes.load(std::memory_order_relaxed),
        .uniform_bytes = live_uniform_buffer_bytes.load(std::memory_order_relaxed),
        .staging_bytes = live_staging_buffer_bytes.load(std::memory_order_relaxed),
        .other_bytes = live_other_buffer_bytes.load(std::memory_order_relaxed),
    };
}

ImageAllocationStats image_allocation_stats() {
    return {
        .count = live_image_count.load(std::memory_order_relaxed),
        .bytes = live_image_bytes.load(std::memory_order_relaxed),
    };
}

Image::Image() = default;

Image::Image(Image &&other) noexcept {
    memcpy(this, &other, sizeof(Image));
    other.sampler = nullptr;
    other.view = nullptr;
    other.image = nullptr;
    other.layout = ImageLayout::Undefined;
    other.allocation_size = 0;
}
Image &Image::operator=(Image &&other) noexcept {
    memcpy(this, &other, sizeof(Image));
    other.sampler = nullptr;
    other.view = nullptr;
    other.image = nullptr;
    other.layout = ImageLayout::Undefined;
    other.allocation_size = 0;
    return *this;
}

Image::Image(uint32_t width, uint32_t height, vk::Format format)
    : width(width)
    , height(height)
    , format(format) {
}

void Image::destroy() {
    if (!destroy_on_deletion || !allocator)
        return;

    vk::Device device = allocator.getAllocatorInfo().device;
    if (sampler) {
        device.destroySampler(sampler);
        sampler = nullptr;
    }
    if (view) {
        device.destroyImageView(view);
        view = nullptr;
    }
    if (image) {
        allocator.destroyImage(image, allocation);
        image = nullptr;
        remove_image_allocation(allocation_size);
        allocation_size = 0;
    }
}

void Image::track_allocation_size(vk::DeviceSize size) {
    allocation_size = size;
    live_image_count.fetch_add(1, std::memory_order_relaxed);
    live_image_bytes.fetch_add(size, std::memory_order_relaxed);
}

Image::~Image() {
    destroy();
}

void Image::init_image(vk::ImageUsageFlags usage, vk::ComponentMapping mapping, const vk::ImageCreateFlags image_create_flags, const void *pNext) {
    vk::ImageCreateInfo image_info{
        .pNext = pNext,
        .flags = image_create_flags,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = vk::Extent3D{
            .width = width,
            .height = height,
            .depth = 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };

    vma::AllocationInfo allocation_info;
    std::tie(image, allocation) = allocator.createImage(image_info, vma_auto_alloc, allocation_info);
    track_allocation_size(allocation_info.size);

    // only create a view if one of these flags is set
    constexpr vk::ImageUsageFlags view_usages = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eStorage;
    if (!(usage & view_usages))
        return;

    vk::ImageSubresourceRange range;
    if (format == vk::Format::eD16UnormS8Uint || format == vk::Format::eD24UnormS8Uint || format == vk::Format::eD32SfloatS8Uint || format == vk::Format::eX8D24UnormPack32) {
        range = vkutil::ds_subresource_range;
    } else if (format == vk::Format::eS8Uint || format == vk::Format::eD16Unorm || format == vk::Format::eD32Sfloat) {
        range = vkutil::d_subresource_range;
    } else {
        range = vkutil::color_subresource_range;
    }

    vk::ImageViewCreateInfo view_info{
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .components = mapping,
        .subresourceRange = range
    };
    view = allocator.getAllocatorInfo().device.createImageView(view_info);
}

void Image::transition_to(vk::CommandBuffer buffer, ImageLayout new_layout, const vk::ImageSubresourceRange &range) {
    transition_image_layout(buffer, image, layout, new_layout, range);
    layout = new_layout;
}

void Image::transition_to_discard(vk::CommandBuffer buffer, ImageLayout new_layout, const vk::ImageSubresourceRange &range) {
    transition_image_layout_discard(buffer, image, layout, new_layout, range);
    layout = new_layout;
}

Buffer::Buffer() = default;

Buffer::Buffer(Buffer &&other) noexcept {
    memcpy(this, &other, sizeof(Buffer));
    other.allocation = nullptr;
    other.buffer = nullptr;
    other.size = 0;
    other.allocation_size = 0;
    other.allocation_category = BufferAllocationCategory::Other;
    other.mapped_data = nullptr;
}
Buffer &Buffer::operator=(Buffer &&other) noexcept {
    memcpy(this, &other, sizeof(Buffer));
    other.allocation = nullptr;
    other.buffer = nullptr;
    other.size = 0;
    other.allocation_size = 0;
    other.allocation_category = BufferAllocationCategory::Other;
    other.mapped_data = nullptr;
    return *this;
}

Buffer::Buffer(vk::DeviceSize size)
    : size(size) {
}

void Buffer::destroy() {
    if (!destroy_on_deletion || !allocator)
        return;

    if (buffer) {
        allocator.destroyBuffer(buffer, allocation);
        buffer = nullptr;
        remove_buffer_allocation(allocation_size, allocation_category);
        allocation_size = 0;
        allocation_category = BufferAllocationCategory::Other;
    }
}

Buffer::~Buffer() {
    destroy();
}

void Buffer::init_buffer(vk::BufferUsageFlags usage_flags, const vma::AllocationCreateInfo &alloc_create_info,
    BufferAllocationCategory category) {
    vk::BufferCreateInfo buffer_info{
        .size = size,
        .usage = usage_flags,
        .sharingMode = vk::SharingMode::eExclusive
    };
    vma::AllocationInfo alloc_info;
    std::tie(buffer, allocation) = allocator.createBuffer(buffer_info, alloc_create_info, alloc_info);
    allocation_size = alloc_info.size;
    if (category == BufferAllocationCategory::Automatic) {
        if (usage_flags & vk::BufferUsageFlagBits::eVertexBuffer)
            category = BufferAllocationCategory::Vertex;
        else if (usage_flags & vk::BufferUsageFlagBits::eIndexBuffer)
            category = BufferAllocationCategory::Index;
        else if (usage_flags & (vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer))
            category = BufferAllocationCategory::Uniform;
        else
            category = BufferAllocationCategory::Other;
    }
    allocation_category = category;
    buffer_bytes(allocation_category).fetch_add(allocation_size, std::memory_order_relaxed);
    mapped_data = alloc_info.pMappedData;
}

RingBuffer::RingBuffer(vk::BufferUsageFlags usage, const size_t capacity)
    : usage(usage)
    , capacity(capacity) {
    uint32_t buffer_capacity = capacity;
    if (usage & vk::BufferUsageFlagBits::eStorageBuffer)
        // TODO: put max size of a gxm uniform buffer
        buffer_capacity += 500 * 1024;
    if (usage & vk::BufferUsageFlagBits::eVertexBuffer)
        // for AMD GPUs, in case the buffer ends exactly with an rgb16 component (which is read as rgba)
        buffer_capacity += 2;
    if (usage & vk::BufferUsageFlagBits::eUniformBuffer)
        // the descriptor set for the uniform buffers specify the max possible size while we only allocate
        // the actual size, this prevents validation errors
        buffer_capacity += 512;

    buffer = Buffer(buffer_capacity);
}

void RingBuffer::allocate(const uint32_t data_size) {
    if (cursor + data_size > capacity)
        cursor = 0;

    data_offset = cursor;

    cursor += data_size;

    cursor = align(cursor, alignment);
}

void HostRingBuffer::create() {
    buffer.init_buffer(usage, vma_mapped_alloc);

    vk::MemoryPropertyFlags memory_properties = allocator.getAllocationMemoryProperties(buffer.allocation);
    is_coherent = static_cast<bool>(memory_properties & vk::MemoryPropertyFlagBits::eHostCoherent);

    cursor = 0;
}

void HostRingBuffer::copy(vk::CommandBuffer cmd_buffer, const uint32_t size, const void *data, const uint32_t offset) {
    memcpy(static_cast<uint8_t *>(buffer.mapped_data) + data_offset + offset, data, size);

    if (!is_coherent)
        allocator.flushAllocation(buffer.allocation, data_offset + offset, size);
}

void LocalRingBuffer::create() {
    // the auto_alloc default behavior should give us memory on the gpu
    // UpdateBuffer needs the buffer to have TransferDst specified
    buffer.init_buffer(usage | vk::BufferUsageFlagBits::eTransferDst);
    cursor = 0;
}

void LocalRingBuffer::copy(vk::CommandBuffer cmd_buffer, const uint32_t size, const void *data, const uint32_t offset) {
    cmd_buffer.updateBuffer(buffer.buffer, data_offset + offset, size, data);
}

void DestroyQueue::init(vk::Device device) {
    this->device = device;
}

void DestroyQueue::add_image(Image &image) {
    if (image.sampler) {
        add(image.sampler);
        image.sampler = nullptr;
    }
    if (image.view) {
        add(image.view);
        image.view = nullptr;
    }

    if (image.image) {
        add(image.image);
        image.image = nullptr;
        destroy_list.push_back(std::bit_cast<uint64_t>(image.allocation));
        destroy_list.push_back(image.allocation_size);
        image.allocation_size = 0;
    }
}

void DestroyQueue::add_buffer(Buffer &buffer) {
    if (buffer.buffer) {
        add(buffer.buffer);
        buffer.buffer = nullptr;
        destroy_list.push_back(std::bit_cast<uint64_t>(buffer.allocation));
        destroy_list.push_back(buffer.allocation_size);
        destroy_list.push_back(static_cast<uint64_t>(buffer.allocation_category));
        buffer.allocation_size = 0;
        buffer.allocation_category = BufferAllocationCategory::Other;
    }
}

void DestroyQueue::add_cmd_buffer(vk::CommandBuffer cmd_buffer, vk::CommandPool cmd_pool) {
    add(cmd_buffer);
    destroy_list.push_back(std::bit_cast<uint64_t>(cmd_pool));
}

#define HANDLE_DESTROY(type)                          \
    case vk::ObjectType::e##type: {                   \
        auto vk_object = std::bit_cast<vk::type>(el); \
        device.destroy(vk_object);                    \
        break;                                        \
    }

void DestroyQueue::destroy_objects() {
    if (destroy_list.empty())
        return;

    int idx = 0;
    while (idx < destroy_list.size()) {
        const vk ::ObjectType type = static_cast<vk::ObjectType>(destroy_list[idx++]);
        uint64_t el = destroy_list[idx++];
        switch (type) {
            // handle special cases apart

        case vk::ObjectType::eImage: {
            // special case: this is a vma allocation
            auto image = std::bit_cast<vk::Image>(el);
            auto allocation = std::bit_cast<vma::Allocation>(destroy_list[idx++]);
            const std::uint64_t allocation_size = destroy_list[idx++];
            allocator.destroyImage(image, allocation);
            remove_image_allocation(allocation_size);
            break;
        }

        case vk::ObjectType::eBuffer: {
            // special case: this is a vma allocation
            auto buffer = std::bit_cast<vk::Buffer>(el);
            auto allocation = std::bit_cast<vma::Allocation>(destroy_list[idx++]);
            const std::uint64_t allocation_size = destroy_list[idx++];
            const auto category = static_cast<BufferAllocationCategory>(destroy_list[idx++]);
            allocator.destroyBuffer(buffer, allocation);
            remove_buffer_allocation(allocation_size, category);
            break;
        }

        case vk::ObjectType::eCommandBuffer: {
            // special case: we must specify the command pool
            auto cmd_buffer = std::bit_cast<vk::CommandBuffer>(el);
            auto cmd_pool = std::bit_cast<vk::CommandPool>(destroy_list[idx++]);
            device.freeCommandBuffers(cmd_pool, cmd_buffer);
            break;
        }

            HANDLE_DESTROY(ImageView)
            HANDLE_DESTROY(Sampler)
            HANDLE_DESTROY(Fence)
            HANDLE_DESTROY(Semaphore)
            HANDLE_DESTROY(Framebuffer)

        default:
            LOG_ERROR("Unknown object type {}", vk::to_string(type));
        }
    }

    destroy_list.clear();
}
} // namespace vkutil
