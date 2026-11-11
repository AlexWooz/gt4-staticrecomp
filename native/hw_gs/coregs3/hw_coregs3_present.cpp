// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// Written from docs/COREGS3-HOOKS.md §10.3 and native/coregs3 (TASK-090 M4), clean room (decision 0070).
//
// The live `present` pass of CoreGS 3.0. Every captured draw of the newest published frame becomes one triangle-list draw
// call per measured position encoding, with the draw's own metre-transform columns in a push constant (the measured VU1
// convention, coregs3_vertex.hpp: linear columns scaled by the shape's `value` and the encoding's units), drawn
// into the composed display image by a fragment-stage imageStore (the image has no COLOR_ATTACHMENT usage; see the
// header) with a private D32 depth attachment and depth compare GEQUAL (the GS's equal-depth rule). Nothing here touches
// CoreGS 2.0's targets, its command buffers, its command pool or the GS stream; the pass runs after the compose
// submission and waits before the frame is presented or dumped.

// Vulkan create-info structs are initialised with sType only (pNext defaults to null by the spec). The device-entry
// table redefines vkCmdClearColorImage, which the loader header already names; the backend's other translation units
// carry the same two suppressions.
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wmacro-redefined"

#include "hw_coregs3_present.hpp"

#include "../shaders/hw_shader_blobs.hpp"
#include "../writeback/hw_gs_vulkan_device_table.hpp"

#include "coregs3_lighting.hpp"
#include "coregs3_presentation.hpp"
#include "coregs3_present.hpp"
#include "coregs3_render.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_vertex.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace gt4::hw::coregs3 {
namespace {

constexpr VkFormat kTargetFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kMaxVerticesPerFrame = 1u << 20;  // 1,048,576 vertices = 16 MiB; a night frame uses ~12k
constexpr VkDeviceSize kInitialVertexBytes = 1u << 20;

// The vertex record of the pass: model position in metres and the model's vertex colour. 16 bytes, matching the two
// vertex attributes the pipeline declares.
struct PresentVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  std::uint8_t red = 255;
  std::uint8_t green = 255;
  std::uint8_t blue = 255;
  std::uint8_t alpha = 255;
};
static_assert(sizeof(PresentVertex) == 16, "the present vertex record must match the pipeline's 16-byte stride");

// One captured draw's place in the shared vertex buffer, with the metre-transform columns the vertex stage consumes.
struct DrawRange {
  std::uint32_t first = 0;   // first vertex (triangle list)
  std::uint32_t count = 0;   // vertices (a multiple of three)
  float columns[16] = {};    // c0..c3 of the metre transform for this range's position encoding, in column order
};

bool find_memory_type(const VkPhysicalDeviceMemoryProperties &properties, std::uint32_t bits,
                      VkMemoryPropertyFlags wanted, std::uint32_t &index) {
  for (std::uint32_t type = 0; type < properties.memoryTypeCount; ++type) {
    if ((bits & (1u << type)) != 0 && (properties.memoryTypes[type].propertyFlags & wanted) == wanted) {
      index = type;
      return true;
    }
  }
  return false;
}

bool format_has(VkPhysicalDevice physical, VkFormat format, VkFormatFeatureFlags wanted) {
  VkFormatProperties properties{};
  gt4_hw_vkGetPhysicalDeviceFormatProperties(physical, format, &properties);
  return (properties.optimalTilingFeatures & wanted) == wanted;
}

bool finite_rows(const float *rows) {
  for (int index = 0; index < 16; ++index)
    if (!(rows[index] == rows[index]) || rows[index] == std::numeric_limits<float>::infinity() ||
        rows[index] == -std::numeric_limits<float>::infinity())
      return false;
  return true;
}

// The push-constant columns for a row-major model->clip matrix whose linear part is in the shape-scaled upload space
// (the `draw_matrix.row` form): a metre position multiplies it by 4096 for a V3-16 channel and by 1 for a V3-32
// channel (coregs3_vertex.hpp), and the translation column is never scaled.
gt4::coregs3::VertexColumns columns_from_upload_rows(const float *rows, gt4::coregs3::PositionEncoding encoding) {
  const float scale = encoding == gt4::coregs3::PositionEncoding::integer_16
                          ? gt4::coregs3::kModelPositionUnitsPerMetre
                          : 1.0f;
  gt4::coregs3::VertexColumns columns;
  for (int column = 0; column < 3; ++column)
    for (int row = 0; row < 4; ++row) columns.column[column][row] = rows[row * 4 + column] * scale;
  for (int row = 0; row < 4; ++row) columns.column[3][row] = rows[row * 4 + 3];
  return columns;
}

}  // namespace

struct PresentLayer::Impl {
  PresentConfig config;
  bool initialised = false;

  VkShaderModule vertex_module = VK_NULL_HANDLE;
  VkShaderModule fragment_module = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  // Depth target and its framebuffer, recreated when the composed image's size changes.
  VkImage depth_image = VK_NULL_HANDLE;
  VkDeviceMemory depth_memory = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  std::uint32_t depth_width = 0;
  std::uint32_t depth_height = 0;

  // The composed image saved before the first presentation tick of a guest frame. A rate above 60 Hz draws several
  // ticks over the same 60 Hz composed base; restoring the base before every later tick keeps a tick's interpolated
  // cars from leaving ghosts of the previous tick. R32_UINT like the target, no view needed, both images stay GENERAL.
  VkImage base_image = VK_NULL_HANDLE;
  VkDeviceMemory base_memory = VK_NULL_HANDLE;
  std::uint32_t base_width = 0;
  std::uint32_t base_height = 0;
  bool base_general = false;  // the base was transitioned out of UNDEFINED

  // Host-visible vertex buffer, grown as the captured frames need.
  VkBuffer vertex_buffer = VK_NULL_HANDLE;
  VkDeviceMemory vertex_memory = VK_NULL_HANDLE;
  void *vertex_mapped = nullptr;
  VkDeviceSize vertex_capacity = 0;

  // Reused between frames: the claim swaps the ring's buffers into these vectors, and build_frame refills them.
  gt4::coregs3::FrameSnapshot frame;
  bool has_frame = false;  // at least one complete frame was claimed
  std::vector<PresentVertex> vertices;
  std::vector<DrawRange> ranges;
  std::vector<gt4::coregs3::DrawTransform> transforms;  // the presentation instant's per-object matrices

  void destroy_depth() {
    if (framebuffer != VK_NULL_HANDLE) gt4_hwd_vkDestroyFramebuffer(config.device, framebuffer, nullptr);
    if (depth_view != VK_NULL_HANDLE) gt4_hwd_vkDestroyImageView(config.device, depth_view, nullptr);
    if (depth_image != VK_NULL_HANDLE) gt4_hwd_vkDestroyImage(config.device, depth_image, nullptr);
    if (depth_memory != VK_NULL_HANDLE) gt4_hwd_vkFreeMemory(config.device, depth_memory, nullptr);
    framebuffer = VK_NULL_HANDLE;
    depth_view = VK_NULL_HANDLE;
    depth_image = VK_NULL_HANDLE;
    depth_memory = VK_NULL_HANDLE;
    depth_width = 0;
    depth_height = 0;
  }

  void destroy_base() {
    if (base_image != VK_NULL_HANDLE) gt4_hwd_vkDestroyImage(config.device, base_image, nullptr);
    if (base_memory != VK_NULL_HANDLE) gt4_hwd_vkFreeMemory(config.device, base_memory, nullptr);
    base_image = VK_NULL_HANDLE;
    base_memory = VK_NULL_HANDLE;
    base_width = 0;
    base_height = 0;
    base_general = false;
  }

  void destroy_vertex_buffer() {
    if (vertex_mapped != nullptr) {
      gt4_hwd_vkUnmapMemory(config.device, vertex_memory);
      vertex_mapped = nullptr;
    }
    if (vertex_buffer != VK_NULL_HANDLE) gt4_hwd_vkDestroyBuffer(config.device, vertex_buffer, nullptr);
    if (vertex_memory != VK_NULL_HANDLE) gt4_hwd_vkFreeMemory(config.device, vertex_memory, nullptr);
    vertex_buffer = VK_NULL_HANDLE;
    vertex_memory = VK_NULL_HANDLE;
    vertex_capacity = 0;
  }

  void shutdown() {
    const VkDevice device = config.device;
    if (device == VK_NULL_HANDLE) return;
    destroy_depth();
    destroy_base();
    destroy_vertex_buffer();
    if (pipeline != VK_NULL_HANDLE) gt4_hwd_vkDestroyPipeline(device, pipeline, nullptr);
    if (render_pass != VK_NULL_HANDLE) gt4_hwd_vkDestroyRenderPass(device, render_pass, nullptr);
    if (pipeline_layout != VK_NULL_HANDLE) gt4_hwd_vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (descriptor_pool != VK_NULL_HANDLE) gt4_hwd_vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    if (set_layout != VK_NULL_HANDLE) gt4_hwd_vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    if (vertex_module != VK_NULL_HANDLE) gt4_hwd_vkDestroyShaderModule(device, vertex_module, nullptr);
    if (fragment_module != VK_NULL_HANDLE) gt4_hwd_vkDestroyShaderModule(device, fragment_module, nullptr);
    if (fence != VK_NULL_HANDLE) gt4_hw_vkDestroyFence(device, fence, nullptr);
    if (command_pool != VK_NULL_HANDLE) gt4_hw_vkDestroyCommandPool(device, command_pool, nullptr);
    pipeline = VK_NULL_HANDLE;
    render_pass = VK_NULL_HANDLE;
    pipeline_layout = VK_NULL_HANDLE;
    descriptor_pool = VK_NULL_HANDLE;
    set_layout = VK_NULL_HANDLE;
    vertex_module = VK_NULL_HANDLE;
    fragment_module = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE;
    command = VK_NULL_HANDLE;
    command_pool = VK_NULL_HANDLE;
    descriptor_set = VK_NULL_HANDLE;
    initialised = false;
  }

  bool make_shader(const std::uint32_t *words, std::size_t count, VkShaderModule &module, std::string &error) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = count * sizeof(std::uint32_t);
    info.pCode = words;
    if (gt4_hwd_vkCreateShaderModule(config.device, &info, nullptr, &module) != VK_SUCCESS) {
      error = "vkCreateShaderModule failed for the CoreGS 3.0 present stage";
      return false;
    }
    return true;
  }

  bool create_objects(std::string &error) {
    if (!make_shader(hwgs::shaders::coregs3_present_vertex_words, hwgs::shaders::coregs3_present_vertex_word_count,
                     vertex_module, error) ||
        !make_shader(hwgs::shaders::coregs3_present_fragment_words, hwgs::shaders::coregs3_present_fragment_word_count,
                     fragment_module, error)) {
      return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 1;
    set_info.pBindings = &binding;
    if (gt4_hwd_vkCreateDescriptorSetLayout(config.device, &set_info, nullptr, &set_layout) != VK_SUCCESS) {
      error = "vkCreateDescriptorSetLayout failed for the CoreGS 3.0 present layer";
      return false;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = 16u * sizeof(float);  // vf24..vf27
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layout;
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    if (gt4_hwd_vkCreatePipelineLayout(config.device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
      error = "vkCreatePipelineLayout failed for the CoreGS 3.0 present layer";
      return false;
    }

    // One D32 depth attachment, no colour attachment: the fragment stage writes the display image as a storage image.
    VkAttachmentDescription depth{};
    depth.format = kDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // the contents are cleared every pass
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference depth_reference{};
    depth_reference.attachment = 0;
    depth_reference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depth_reference;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 1;
    pass_info.pAttachments = &depth;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    if (gt4_hwd_vkCreateRenderPass(config.device, &pass_info, nullptr, &render_pass) != VK_SUCCESS) {
      error = "vkCreateRenderPass failed for the CoreGS 3.0 present layer";
      return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = sizeof(PresentVertex);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attributes[2]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    attributes[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12};
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 2;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    // The clip z is constant 1 with w = cw: depth clamp keeps a vertex with 0 < cw < 1 instead of clipping it away,
    // and clamps its 1/cw depth into the [0, 1] range. The device feature is checked before this layer is created.
    raster.depthClampEnable = VK_TRUE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;  // the VU1's own cull is not reproduced here (docs/COREGS3-PLAN.md step 1e)
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = VK_TRUE;
    // depth = 1/cw: the nearer vertex has the greater value. The GS rule measured by the CPU reference is GEQUAL
    // (coregs3_render.cpp: a later primitive wins at equal depth), which matters for the car's coincident body layers:
    // with strict GREATER the earlier, untextured layer wins the tie and hides the lit body.
    depth_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    depth_state.depthBoundsTestEnable = VK_FALSE;
    depth_state.stencilTestEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 0;  // no blending: the fragment stage stores the packed word itself
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &assembly;
    pipeline_info.pViewportState = &viewport;
    pipeline_info.pRasterizationState = &raster;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_state;
    pipeline_info.pColorBlendState = &blend;
    pipeline_info.pDynamicState = &dynamic;
    pipeline_info.layout = pipeline_layout;
    pipeline_info.renderPass = render_pass;
    if (gt4_hwd_vkCreateGraphicsPipelines(config.device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      error = "vkCreateGraphicsPipelines failed for the CoreGS 3.0 present layer";
      return false;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    pool_size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    if (gt4_hwd_vkCreateDescriptorPool(config.device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
      error = "vkCreateDescriptorPool failed for the CoreGS 3.0 present layer";
      return false;
    }
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = descriptor_pool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout;
    if (gt4_hwd_vkAllocateDescriptorSets(config.device, &allocate, &descriptor_set) != VK_SUCCESS) {
      error = "vkAllocateDescriptorSets failed for the CoreGS 3.0 present layer";
      return false;
    }

    VkCommandPoolCreateInfo command_pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_info.queueFamilyIndex = config.queue_family;
    if (gt4_hw_vkCreateCommandPool(config.device, &command_pool_info, nullptr, &command_pool) != VK_SUCCESS) {
      error = "vkCreateCommandPool failed for the CoreGS 3.0 present layer";
      return false;
    }
    VkCommandBufferAllocateInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_info.commandPool = command_pool;
    command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_info.commandBufferCount = 1;
    if (gt4_hw_vkAllocateCommandBuffers(config.device, &command_info, &command) != VK_SUCCESS) {
      error = "vkAllocateCommandBuffers failed for the CoreGS 3.0 present layer";
      return false;
    }
    // Signalled, so the first render's wait returns at once; every submit resets it first.
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (gt4_hw_vkCreateFence(config.device, &fence_info, nullptr, &fence) != VK_SUCCESS) {
      error = "vkCreateFence failed for the CoreGS 3.0 present layer";
      return false;
    }

    if (!create_vertex_buffer(kInitialVertexBytes, error)) return false;
    initialised = true;
    return true;
  }

  bool create_vertex_buffer(VkDeviceSize bytes, std::string &error) {
    destroy_vertex_buffer();
    const VkDeviceSize size = std::max<VkDeviceSize>(bytes, kInitialVertexBytes);
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (gt4_hwd_vkCreateBuffer(config.device, &buffer_info, nullptr, &vertex_buffer) != VK_SUCCESS) {
      error = "vkCreateBuffer failed for the CoreGS 3.0 present vertices";
      return false;
    }
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(config.device, vertex_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    gt4_hwd_vkGetPhysicalDeviceMemoryProperties(config.physical, &memory);
    std::uint32_t type = 0;
    if (!find_memory_type(memory, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type)) {
      error = "no host-visible coherent memory for the CoreGS 3.0 present vertices";
      return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(config.device, &allocate, nullptr, &vertex_memory) != VK_SUCCESS) {
      error = "vkAllocateMemory failed for the CoreGS 3.0 present vertices";
      return false;
    }
    if (gt4_hwd_vkBindBufferMemory(config.device, vertex_buffer, vertex_memory, 0) != VK_SUCCESS) {
      error = "vkBindBufferMemory failed for the CoreGS 3.0 present vertices";
      return false;
    }
    if (gt4_hwd_vkMapMemory(config.device, vertex_memory, 0, size, 0, &vertex_mapped) != VK_SUCCESS) {
      error = "vkMapMemory failed for the CoreGS 3.0 present vertices";
      return false;
    }
    vertex_capacity = size;
    return true;
  }

  bool ensure_vertex_buffer(VkDeviceSize bytes, std::string &error) {
    if (vertex_capacity >= bytes) return true;
    const VkDeviceSize grown = std::max<VkDeviceSize>(bytes, vertex_capacity * 2u);
    return create_vertex_buffer(grown, error);
  }

  bool ensure_depth(std::uint32_t width, std::uint32_t height, std::string &error) {
    if (depth_image != VK_NULL_HANDLE && depth_width == width && depth_height == height) return true;
    destroy_depth();
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kDepthFormat;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (gt4_hwd_vkCreateImage(config.device, &image_info, nullptr, &depth_image) != VK_SUCCESS) {
      error = "vkCreateImage failed for the CoreGS 3.0 present depth";
      return false;
    }
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetImageMemoryRequirements(config.device, depth_image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    gt4_hwd_vkGetPhysicalDeviceMemoryProperties(config.physical, &memory);
    std::uint32_t type = 0;
    if (!find_memory_type(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
      error = "no device-local memory for the CoreGS 3.0 present depth";
      return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(config.device, &allocate, nullptr, &depth_memory) != VK_SUCCESS) {
      error = "vkAllocateMemory failed for the CoreGS 3.0 present depth";
      return false;
    }
    if (gt4_hwd_vkBindImageMemory(config.device, depth_image, depth_memory, 0) != VK_SUCCESS) {
      error = "vkBindImageMemory failed for the CoreGS 3.0 present depth";
      return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = depth_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kDepthFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (gt4_hwd_vkCreateImageView(config.device, &view_info, nullptr, &depth_view) != VK_SUCCESS) {
      error = "vkCreateImageView failed for the CoreGS 3.0 present depth";
      return false;
    }
    VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &depth_view;
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = 1;
    if (gt4_hwd_vkCreateFramebuffer(config.device, &framebuffer_info, nullptr, &framebuffer) != VK_SUCCESS) {
      error = "vkCreateFramebuffer failed for the CoreGS 3.0 present layer";
      return false;
    }
    depth_width = width;
    depth_height = height;
    return true;
  }

  // The scratch copy of the composed image (see `base_image`). TRANSFER_DST for the save, TRANSFER_SRC for the restores.
  bool ensure_base_image(std::uint32_t width, std::uint32_t height, std::string &error) {
    if (base_image != VK_NULL_HANDLE && base_width == width && base_height == height) return true;
    destroy_base();
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kTargetFormat;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (gt4_hwd_vkCreateImage(config.device, &image_info, nullptr, &base_image) != VK_SUCCESS) {
      error = "vkCreateImage failed for the CoreGS 3.0 present base image";
      return false;
    }
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetImageMemoryRequirements(config.device, base_image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    gt4_hwd_vkGetPhysicalDeviceMemoryProperties(config.physical, &memory);
    std::uint32_t type = 0;
    if (!find_memory_type(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
      error = "no device-local memory for the CoreGS 3.0 present base image";
      destroy_base();
      return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(config.device, &allocate, nullptr, &base_memory) != VK_SUCCESS) {
      error = "vkAllocateMemory failed for the CoreGS 3.0 present base image";
      destroy_base();
      return false;
    }
    if (gt4_hwd_vkBindImageMemory(config.device, base_image, base_memory, 0) != VK_SUCCESS) {
      error = "vkBindImageMemory failed for the CoreGS 3.0 present base image";
      destroy_base();
      return false;
    }
    base_width = width;
    base_height = height;
    return true;
  }

  // One captured draw -> one triangle-list draw call per measured position encoding. Positions are metres (build_mesh's
  // scale) and colours are the per-item lit vertex colours (coregs3_lighting.hpp, shade_mesh_items): every item is
  // shaded with its own program and its own TOPS+0xC9 block, and the push constant carries the draw's metre columns
  // (coregs3_vertex.hpp: the linear part scaled by the shape's `value` and by the encoding's units, translation
  // unscaled). `overrides` is the presentation timeline's matrix per object of this frame: a `captured_newer` entry
  // draws exactly as the capture does (byte-identical at 60 Hz), any other entry builds the metre columns from its
  // row-major upload-space matrix. Returns false when the frame as a whole cannot be drawn (capacity); a draw that
  // fails to decode is counted in `refused` and left to the GS-drawn image underneath. `unlit_draws` counts the drawn
  // draws with no lit vertex; `unlit_vertices` counts the uploaded triangle corners that kept their base colour.
  // `lighting` accumulates the per-family and per-status detail the F8 report prints.
  bool build_frame(const std::vector<gt4::coregs3::DrawTransform> *overrides, std::uint32_t &refused,
                   std::uint32_t &unlit_draws, std::uint64_t &unlit_vertices, gt4::coregs3::LightCounters &lighting) {
    vertices.clear();
    ranges.clear();
    refused = 0;
    unlit_draws = 0;
    unlit_vertices = 0;
    std::vector<gt4::coregs3::ShadedVertex> shades;
    // Instance numbering exactly as timeline_frame numbers it: accepted draws (valid matrix) in draw order, per id.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> instances;
    for (const gt4::coregs3::CapturedDraw &draw : frame.draws) {
      const gt4::coregs3::DrawMatrix matrix = gt4::coregs3::draw_matrix(draw);
      if (!matrix.valid) {
        ++refused;
        continue;
      }
      gt4::coregs3::ObjectKey key = gt4::coregs3::object_key(draw);
      std::uint32_t instance = 0;
      bool known = false;
      for (auto &entry : instances)
        if (entry.first == key.id) {
          instance = entry.second;
          entry.second += 1;
          known = true;
          break;
        }
      if (!known) instances.push_back({key.id, 1u});
      key.instance = instance;
      const gt4::coregs3::DrawTransform *override = nullptr;
      if (overrides != nullptr)
        for (const gt4::coregs3::DrawTransform &candidate : *overrides)
          if (candidate.key.id == key.id && candidate.key.instance == key.instance) {
            override = &candidate;
            break;
          }
      const bool blend = override != nullptr && override->source != gt4::coregs3::TransformSource::captured_newer;
      if (blend && !finite_rows(override->matrix)) {
        ++refused;
        continue;
      }
      gt4::coregs3::Mesh mesh;
      std::string mesh_error;
      if (!gt4::coregs3::build_mesh(frame.packets.data(), frame.packets.size(), draw, gt4::coregs3::MeshOptions{},
                                    mesh, mesh_error)) {
        ++refused;
        continue;
      }
      const std::size_t triangles = mesh.indices.size() / 3u;
      if (triangles == 0) {
        ++refused;
        continue;
      }
      // A triangle must ride one position encoding: its three corners were written by one item's position channel. A
      // mesh that mixes encodings inside a triangle is not something the measured format produces, so refuse the draw
      // instead of drawing it with a guessed matrix.
      bool mixed_encoding = false;
      for (std::size_t index = 0; index + 2 < mesh.indices.size() && !mixed_encoding; index += 3) {
        const gt4::coregs3::PositionEncoding encoding =
            gt4::coregs3::mesh_position_encoding(mesh.vertices[mesh.indices[index]]);
        for (std::size_t corner = 1; corner < 3; ++corner)
          if (gt4::coregs3::mesh_position_encoding(mesh.vertices[mesh.indices[index + corner]]) != encoding)
            mixed_encoding = true;
      }
      if (mixed_encoding) {
        ++refused;
        continue;
      }
      const std::uint64_t needed = static_cast<std::uint64_t>(vertices.size()) + triangles * 3u;
      if (needed > kMaxVerticesPerFrame) {
        vertices.clear();
        ranges.clear();
        return false;
      }
      // Shade every mesh vertex with its own item's program and block; an item whose selector or block cannot be used
      // keeps its base colour and is counted, never shaded with guessed constants.
      gt4::coregs3::LightCounters draw_lighting;
      gt4::coregs3::shade_mesh_items(draw, mesh, draw_lighting, &shades);
      lighting.add(draw_lighting);
      if (draw_lighting.draws_fallback != 0) ++unlit_draws;
      // One range per position encoding: the push constant carries the metre columns, and the V3-16 and V3-32 sets
      // differ by 4096 raw units per metre (coregs3_vertex.hpp). Triangles of one encoding stay contiguous because each
      // item appends its own vertices in one channel, so a range boundary is exact.
      gt4::coregs3::PositionEncoding range_encoding = gt4::coregs3::PositionEncoding::integer_16;
      bool have_range = false;
      DrawRange range;
      const auto open_range = [&](gt4::coregs3::PositionEncoding encoding) {
        gt4::coregs3::VertexColumns built;
        const gt4::coregs3::VertexColumns *columns = encoding == gt4::coregs3::PositionEncoding::integer_16
                                                         ? &matrix.integer_positions
                                                         : &matrix.float_positions;
        if (blend) {
          built = columns_from_upload_rows(override->matrix, encoding);
          columns = &built;
        }
        range = DrawRange{};
        range.first = static_cast<std::uint32_t>(vertices.size());
        range.count = 0;
        range_encoding = encoding;
        have_range = true;
        std::memcpy(range.columns, &columns->column[0][0], sizeof range.columns);
      };
      for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const gt4::coregs3::PositionEncoding encoding =
            gt4::coregs3::mesh_position_encoding(mesh.vertices[mesh.indices[index]]);
        if (!have_range || encoding != range_encoding) {
          if (have_range) ranges.push_back(range);
          open_range(encoding);
        }
        for (std::size_t corner = 0; corner < 3; ++corner) {
          const std::uint32_t source_index = mesh.indices[index + corner];
          const gt4::coregs3::MeshVertex &source = mesh.vertices[source_index];
          PresentVertex vertex;
          vertex.x = source.position[0];
          vertex.y = source.position[1];
          vertex.z = source.position[2];
          vertex.red = source.color[0];
          vertex.green = source.color[1];
          vertex.blue = source.color[2];
          vertex.alpha = 255;  // the CPU reference writes an opaque layer (coregs3_render)
          if (shades[source_index].status != gt4::coregs3::LightStatus::ok) ++unlit_vertices;
          vertices.push_back(vertex);
          range.count += 1;
        }
      }
      if (have_range) ranges.push_back(range);
    }
    return !ranges.empty();
  }

  // Claims the newest complete captured frame and records it in the presentation timeline. Called once per guest
  // VBlank by the backend, whether or not a picture is drawn: the timeline must sample every 60 Hz guest state.
  bool note_guest_frame(std::string &error) {
    if (!initialised) {
      error = "the CoreGS 3.0 present layer is not initialised";
      return false;
    }
    if (!gt4::coregs3::present::claim_latest(frame)) return true;  // no new complete frame
    has_frame = true;
    gt4::coregs3::present::note_guest_frame(frame);
    return true;
  }

  // Records the copy of the composed image into `base_image` (the first tick of a guest frame) or the restore of that
  // base into the target (every later tick). Both images stay in GENERAL; only the access masks change. Without the
  // restore, the second tick of a 120 Hz frame would draw over the first tick's cars.
  void record_base_copy(VkImage target, std::uint32_t width, std::uint32_t height, bool first_tick) {
    const auto make = [](VkImage image, VkAccessFlags source, VkAccessFlags destination, bool from_undefined) {
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.srcAccessMask = source;
      barrier.dstAccessMask = destination;
      barrier.oldLayout = from_undefined ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      return barrier;
    };
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {width, height, 1};
    if (first_tick) {
      // Composition (or the previous tick) wrote the target with a compute/fragment shader; the base starts undefined.
      VkImageMemoryBarrier before[2] = {
          make(target, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, false),
          make(base_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, !base_general)};
      gt4_hw_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, before);
      gt4_hwd_vkCmdCopyImage(command, target, VK_IMAGE_LAYOUT_GENERAL, base_image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      VkImageMemoryBarrier after[2] = {
          make(base_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, false),
          make(target, VK_ACCESS_TRANSFER_READ_BIT,
               VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, false)};
      gt4_hw_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                  0, nullptr, 2, after);
      base_general = true;
    } else {
      VkImageMemoryBarrier before = make(target, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, false);
      gt4_hw_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &before);
      gt4_hwd_vkCmdCopyImage(command, base_image, VK_IMAGE_LAYOUT_GENERAL, target, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      VkImageMemoryBarrier after =
          make(target, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT, false);
      gt4_hw_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                      VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  0, 0, nullptr, 0, nullptr, 1, &after);
    }
  }

  // One presentation tick: uploads the vertices build_frame produced and draws them over `target` (after saving or
  // restoring the composed base). Waits for the submission before returning.
  bool draw_tick(std::uint64_t target, std::uint32_t width, std::uint32_t height, const SubmitOverlay &submit,
                 bool first_tick, std::string &error) {
    const VkDevice device = config.device;
    const VkDeviceSize vertex_bytes = static_cast<VkDeviceSize>(vertices.size()) * sizeof(PresentVertex);
    if (!ensure_vertex_buffer(vertex_bytes, error)) return false;

    // The previous submission is always waited for before returning, but keep the rule explicit: nothing may still use
    // the vertex buffer, the descriptor set or the temporary view when they are rewritten.
    if (gt4_hw_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
      error = "vkWaitForFences failed before the CoreGS 3.0 present pass";
      return false;
    }
    if (gt4_hw_vkResetFences(device, 1, &fence) != VK_SUCCESS) {
      error = "vkResetFences failed for the CoreGS 3.0 present pass";
      return false;
    }
    std::memcpy(vertex_mapped, vertices.data(), static_cast<std::size_t>(vertex_bytes));  // coherent: no flush

    VkImageView target_view = VK_NULL_HANDLE;
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = reinterpret_cast<VkImage>(target);
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kTargetFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (gt4_hwd_vkCreateImageView(device, &view_info, nullptr, &target_view) != VK_SUCCESS) {
      error = "vkCreateImageView failed for the composed display image";
      return false;
    }
    const auto destroy_target_view = [&] {
      if (target_view != VK_NULL_HANDLE) gt4_hwd_vkDestroyImageView(device, target_view, nullptr);
      target_view = VK_NULL_HANDLE;
    };
    VkDescriptorImageInfo image_descriptor{VK_NULL_HANDLE, target_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &image_descriptor;
    gt4_hwd_vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

    if (gt4_hw_vkResetCommandBuffer(command, 0) != VK_SUCCESS) {
      destroy_target_view();
      error = "vkResetCommandBuffer failed for the CoreGS 3.0 present pass";
      return false;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (gt4_hw_vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
      destroy_target_view();
      error = "vkBeginCommandBuffer failed for the CoreGS 3.0 present pass";
      return false;
    }
    record_base_copy(reinterpret_cast<VkImage>(target), width, height, first_tick);
    VkClearValue clear{};
    clear.depthStencil = {0.0f, 0};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = render_pass;
    pass.framebuffer = framebuffer;
    pass.renderArea = {{0, 0}, {width, height}};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    gt4_hwd_vkCmdBeginRenderPass(command, &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    gt4_hwd_vkCmdSetViewport(command, 0, 1, &viewport);
    VkRect2D scissor{{0, 0}, {width, height}};
    gt4_hwd_vkCmdSetScissor(command, 0, 1, &scissor);
    gt4_hwd_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    gt4_hwd_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor_set, 0,
                                    nullptr);
    const VkDeviceSize offset = 0;
    gt4_hwd_vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer, &offset);
    for (const DrawRange &range : ranges) {
      gt4_hwd_vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof range.columns,
                                 range.columns);
      gt4_hwd_vkCmdDraw(command, range.count, 1, range.first, 0);
    }
    gt4_hwd_vkCmdEndRenderPass(command);
    {
      // The next reader is the present copy, the dump readback (a transfer) or the next tick's restore copy.
      VkImageMemoryBarrier after{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      after.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      after.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      after.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      after.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      after.image = reinterpret_cast<VkImage>(target);
      after.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      after.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      after.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT |
                            VK_ACCESS_SHADER_WRITE_BIT;
      gt4_hw_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                                  nullptr, 1, &after);
    }
    if (gt4_hw_vkEndCommandBuffer(command) != VK_SUCCESS) {
      destroy_target_view();
      error = "vkEndCommandBuffer failed for the CoreGS 3.0 present pass";
      return false;
    }
    if (!submit(command, fence, error)) {
      destroy_target_view();
      return false;
    }
    if (gt4_hw_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
      destroy_target_view();
      error = "vkWaitForFences failed after the CoreGS 3.0 present pass";
      return false;
    }
    destroy_target_view();
    return true;
  }

  bool render(std::uint64_t target, std::uint32_t width, std::uint32_t height, const SubmitOverlay &submit,
              std::string &error) {
    if (!initialised) {
      error = "the CoreGS 3.0 present layer is not initialised";
      return false;
    }
    if (width == 0 || height == 0) return true;  // nothing to draw into
    if (!has_frame) return true;                 // no captured frame yet
    const gt4::coregs3::PresentationClock::Due batch = gt4::coregs3::present::due_ticks();
    if (batch.count == 0) return true;  // no presentation instant due with the frame last noted
    const gt4::coregs3::present::TimelineConfig &timeline = gt4::coregs3::present::timeline_config();
    std::uint32_t first_tick = 0;
    std::uint32_t tick_count = batch.count;
    if (timeline.selected_tick != UINT32_MAX) {
      // Diagnostic for image evidence: draw only the selected tick of this guest frame, over the saved base.
      first_tick = std::min(timeline.selected_tick, batch.count - 1u);
      tick_count = 1;
    }
    if (!ensure_depth(width, height, error)) return false;
    if (!ensure_base_image(width, height, error)) return false;
    bool drew_any = false;
    for (std::uint32_t tick = first_tick; tick < first_tick + tick_count; ++tick) {
      const std::uint64_t instant = batch.first + static_cast<std::uint64_t>(tick) * batch.step;
      gt4::coregs3::present::interpolated_transforms(instant, transforms, /*count=*/false);
      std::uint32_t refused = 0;
      std::uint32_t unlit_draws = 0;
      std::uint64_t unlit_vertices = 0;
      gt4::coregs3::LightCounters lighting;
      if (frame.draw_count == 0 ||
          !build_frame(transforms.empty() ? nullptr : &transforms, refused, unlit_draws, unlit_vertices, lighting)) {
        gt4::coregs3::present::note_pass_refused(refused);
        continue;
      }
      if (!draw_tick(target, width, height, submit, !drew_any, error)) return false;
      drew_any = true;
      gt4::coregs3::present::note_draw_tick();
      const std::uint64_t vertex_count = static_cast<std::uint64_t>(vertices.size());
      gt4::coregs3::present::note_render(frame.vblank, static_cast<std::uint32_t>(ranges.size()), refused, unlit_draws,
                                        vertex_count, unlit_vertices, vertex_count / 3u, lighting);
    }
    return true;
  }
};

PresentLayer::PresentLayer() : impl_(std::make_unique<Impl>()) {}
PresentLayer::~PresentLayer() { shutdown(); }

bool PresentLayer::init(const PresentConfig &config, std::string &error) {
  Impl &s = *impl_;
  if (s.initialised) {
    error = "the CoreGS 3.0 present layer is already initialised";
    return false;
  }
  s.config = config;
  if (config.instance == VK_NULL_HANDLE || config.physical == VK_NULL_HANDLE || config.device == VK_NULL_HANDLE ||
      config.queue == VK_NULL_HANDLE) {
    error = "the CoreGS 3.0 present layer needs the backend's instance, physical device, device and queue";
    return false;
  }
  // The pass needs the clamp: the clip z is 1 with w = cw, so without it every vertex with 0 < cw < 1 is clipped away.
  if (!config.depth_clamp_enabled) {
    error = "the device has no depthClamp feature enabled; the live present pass needs it";
    return false;
  }
  if (!format_has(config.physical, kDepthFormat, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    error = "the device cannot use D32_SFLOAT as a depth attachment";
    return false;
  }
  if (!format_has(config.physical, kTargetFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
    error = "the device cannot use R32_UINT as a storage image";
    return false;
  }
  std::vector<std::string> missing;
  if (!gt4::hw::vulkan_device::load_device_entry_points(config.instance, missing)) {
    error = "missing Vulkan entry points for the CoreGS 3.0 present layer:";
    for (const std::string &name : missing) error += " " + name;
    return false;
  }
  if (!s.create_objects(error)) {
    s.shutdown();
    return false;
  }
  // The presentation timeline is configured by the hooks at startup; when they did not run (an unusual build order),
  // parse the same variables here rather than defaulting silently.
  if (!gt4::coregs3::present::timeline_configured()) {
    gt4::coregs3::present::TimelineConfig timeline;
    std::string config_error;
    if (!gt4::coregs3::present::timeline_config_from_environment(timeline, config_error)) {
      error = config_error;
      s.shutdown();
      return false;
    }
    gt4::coregs3::present::set_timeline_config(timeline);
  }
  const gt4::coregs3::present::TimelineConfig &timeline = gt4::coregs3::present::timeline_config();
  std::fprintf(stderr,
               "[coregs3] present layer: R32_UINT storage image, D32 depth, depth clamp, vf24..vf27 push constant, "
               "%u Hz %s%s\n",
               timeline.hz, gt4::coregs3::presentation_mode_name(timeline.mode),
               timeline.selected_tick == UINT32_MAX ? "" : " (diagnostic: one tick per frame)");
  return true;
}

void PresentLayer::shutdown() { impl_->shutdown(); }

bool PresentLayer::note_guest_frame(std::string &error) { return impl_->note_guest_frame(error); }

bool PresentLayer::render(std::uint64_t target, std::uint32_t width, std::uint32_t height, const SubmitOverlay &submit,
                          std::string &error) {
  return impl_->render(target, width, height, submit, error);
}

}  // namespace gt4::hw::coregs3
