// MIT License. Copyright (c) 2026 João Alano and GT4Recomp contributors.
// TASK-135: see hw_hud_layer.hpp.
#include "hw_hud_layer.hpp"

// Vulkan create-info structs are initialised with sType only (pNext defaults to null by the spec).
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

#include "../writeback/hw_gs_vulkan_device_table.hpp"
#include "../shaders/hw_shader_blobs.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace gt4::hw::hud {
namespace {

constexpr double kTextPeriodMs = 500.0;
constexpr int kMarginPixels = 12;

// The text's height follows the window: 16 pixels on a 1080-line window.
int font_pixels_for(std::uint32_t window_height) {
  return std::clamp(static_cast<int>(std::lround(window_height / 1080.0 * 16.0)), 12, 44);
}

struct QuadConstants {
  float origin[2];
  float size[2];
};

}  // namespace

Layer::Layer(VkPhysicalDevice physical, VkDevice device, std::uint32_t slots, void *window, std::string gpu_name, std::string scale, Mode mode)
    : physical_(physical), device_(device), slots_(slots), window_(window), mode_(mode) {
  facts_.gpu_name = std::move(gpu_name);
  facts_.cpu_name = cpu_brand_string();
  facts_.scale = std::move(scale);
  sampler_ = std::make_unique<SystemSampler>(window_, [this](const SystemReading &reading) {
    const std::lock_guard<std::mutex> lock(mutex_);
    reading_ = reading;
  });
}

Layer::~Layer() {
  sampler_.reset();
  destroy_targets();
  destroy_panel_resources();
  destroy_pipeline();
}

double Layer::now_ms() const {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start_).count();
}

void Layer::note_present() {
  const double now = now_ms();
  const std::lock_guard<std::mutex> lock(mutex_);
  times_.note_present(now);
}

bool Layer::memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted, std::uint32_t &chosen) const {
  VkPhysicalDeviceMemoryProperties props{};
  gt4_hwd_vkGetPhysicalDeviceMemoryProperties(physical_, &props);
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & wanted) == wanted) {
      chosen = i;
      return true;
    }
  }
  return false;
}

bool Layer::create_pipeline(VkFormat format) {
  VkAttachmentDescription attachment{};
  attachment.format = format;
  attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attachment.initialLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  VkAttachmentReference colour{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colour;
  // After the picture's blit, and before the barrier that hands the picture on.
  VkSubpassDependency dependencies[2] = {};
  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  pass.attachmentCount = 1;
  pass.pAttachments = &attachment;
  pass.subpassCount = 1;
  pass.pSubpasses = &subpass;
  pass.dependencyCount = 2;
  pass.pDependencies = dependencies;
  if (vkCreateRenderPass(device_, &pass, nullptr, &render_pass_) != VK_SUCCESS) return false;

  VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler.magFilter = VK_FILTER_NEAREST;
  sampler.minFilter = VK_FILTER_NEAREST;
  sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &sampler, nullptr, &sampler_object_) != VK_SUCCESS) return false;

  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device_, &set_info, nullptr, &set_layout_) != VK_SUCCESS) return false;

  VkPushConstantRange constants{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(QuadConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &constants;
  if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;

  VkShaderModuleCreateInfo vertex_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  vertex_info.codeSize = hwgs::shaders::hw_hud_vertex_word_count * sizeof(std::uint32_t);
  vertex_info.pCode = hwgs::shaders::hw_hud_vertex_words;
  VkShaderModuleCreateInfo fragment_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  fragment_info.codeSize = hwgs::shaders::hw_hud_fragment_word_count * sizeof(std::uint32_t);
  fragment_info.pCode = hwgs::shaders::hw_hud_fragment_words;
  VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
  bool made = vkCreateShaderModule(device_, &vertex_info, nullptr, &vertex) == VK_SUCCESS &&
              vkCreateShaderModule(device_, &fragment_info, nullptr, &fragment) == VK_SUCCESS;
  if (made) {
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;  // the panel's pixels are premultiplied
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blending.attachmentCount = 1;
    blending.pAttachments = &blend;
    const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertices;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = render_pass_;
    made = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline_) == VK_SUCCESS;
  }
  if (vertex != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vertex, nullptr);
  if (fragment != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragment, nullptr);
  format_ = format;
  return made;
}

void Layer::destroy_pipeline() {
  if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
  if (pipeline_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (set_layout_ != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  if (sampler_object_ != VK_NULL_HANDLE) vkDestroySampler(device_, sampler_object_, nullptr);
  if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  sampler_object_ = VK_NULL_HANDLE;
  render_pass_ = VK_NULL_HANDLE;
  format_ = VK_FORMAT_UNDEFINED;
}

bool Layer::create_panel_resources() {
  const std::uint32_t width = static_cast<std::uint32_t>(canvas_->width()), height = static_cast<std::uint32_t>(canvas_->height());
  VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_R8G8B8A8_UNORM;
  image.extent = {width, height, 1};
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (vkCreateImage(device_, &image, nullptr, &image_) != VK_SUCCESS) return false;
  VkMemoryRequirements needs{};
  vkGetImageMemoryRequirements(device_, image_, &needs);
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = needs.size;
  if (!memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, allocate.memoryTypeIndex) ||
      vkAllocateMemory(device_, &allocate, nullptr, &image_memory_) != VK_SUCCESS ||
      vkBindImageMemory(device_, image_, image_memory_, 0) != VK_SUCCESS) {
    return false;
  }
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image_;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = VK_FORMAT_R8G8B8A8_UNORM;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &view, nullptr, &image_view_) != VK_SUCCESS) return false;

  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * sizeof(std::uint32_t);
  staging_.assign(slots_, Staging{});
  for (Staging &staging : staging_) {
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = bytes;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &buffer, nullptr, &staging.buffer) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(device_, staging.buffer, &needs);
    allocate.allocationSize = needs.size;
    if (!memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, allocate.memoryTypeIndex) ||
        vkAllocateMemory(device_, &allocate, nullptr, &staging.memory) != VK_SUCCESS ||
        vkBindBufferMemory(device_, staging.buffer, staging.memory, 0) != VK_SUCCESS) {
      return false;
    }
    void *mapped = nullptr;
    if (vkMapMemory(device_, staging.memory, 0, bytes, 0, &mapped) != VK_SUCCESS) return false;
    staging.mapped = static_cast<std::uint32_t *>(mapped);
  }

  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set.descriptorPool = descriptor_pool_;
  set.descriptorSetCount = 1;
  set.pSetLayouts = &set_layout_;
  if (vkAllocateDescriptorSets(device_, &set, &descriptor_set_) != VK_SUCCESS) return false;
  VkDescriptorImageInfo described{sampler_object_, image_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = descriptor_set_;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  write.pImageInfo = &described;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  image_ready_ = false;
  next_text_ms_ = 0.0;
  return true;
}

void Layer::destroy_panel_resources() {
  if (descriptor_pool_ != VK_NULL_HANDLE) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
  descriptor_pool_ = VK_NULL_HANDLE;
  descriptor_set_ = VK_NULL_HANDLE;
  for (Staging &staging : staging_) {
    if (staging.buffer != VK_NULL_HANDLE) vkDestroyBuffer(device_, staging.buffer, nullptr);
    if (staging.memory != VK_NULL_HANDLE) vkFreeMemory(device_, staging.memory, nullptr);
  }
  staging_.clear();
  if (image_view_ != VK_NULL_HANDLE) vkDestroyImageView(device_, image_view_, nullptr);
  if (image_ != VK_NULL_HANDLE) vkDestroyImage(device_, image_, nullptr);
  if (image_memory_ != VK_NULL_HANDLE) vkFreeMemory(device_, image_memory_, nullptr);
  image_view_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
  image_memory_ = VK_NULL_HANDLE;
  image_ready_ = false;
}

void Layer::destroy_targets() {
  for (Target &target : targets_) {
    if (target.framebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device_, target.framebuffer, nullptr);
    if (target.view != VK_NULL_HANDLE) vkDestroyImageView(device_, target.view, nullptr);
  }
  targets_.clear();
}

void Layer::targets_gone() {
  destroy_targets();
}

bool Layer::ensure_resources(VkFormat format, VkExtent2D extent) {
  if (format != format_) {
    destroy_targets();
    destroy_panel_resources();
    destroy_pipeline();
    if (!create_pipeline(format)) return false;
  }
  const int pixels = font_pixels_for(extent.height);
  if (!canvas_ || pixels != font_pixels_) {
    GlyphAtlas atlas = make_glyph_atlas(pixels);
    if (atlas.coverage.empty()) return false;
    destroy_panel_resources();
    // The number of lines is fixed for the session: the names and the mode decide it, not the readings.
    const std::size_t lines = hud_lines(mode_, report_line(report_status_, perf::ReportStatus::now_ms()), facts_, FrameWindow{}, SystemReading{}).size();
    canvas_ = std::make_unique<Canvas>(std::move(atlas), mode_ == Mode::full ? kColumns : kFpsColumns, lines, mode_ == Mode::full);
    font_pixels_ = pixels;
  }
  return image_ != VK_NULL_HANDLE || create_panel_resources();
}

Layer::Target *Layer::target_for(VkImage image, VkExtent2D extent) {
  for (Target &target : targets_) {
    if (target.image == image) return &target;
  }
  Target target;
  target.image = image;
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format_;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &view, nullptr, &target.view) != VK_SUCCESS) return nullptr;
  VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  framebuffer.renderPass = render_pass_;
  framebuffer.attachmentCount = 1;
  framebuffer.pAttachments = &target.view;
  framebuffer.width = extent.width;
  framebuffer.height = extent.height;
  framebuffer.layers = 1;
  if (vkCreateFramebuffer(device_, &framebuffer, nullptr, &target.framebuffer) != VK_SUCCESS) {
    vkDestroyImageView(device_, target.view, nullptr);
    return nullptr;
  }
  targets_.push_back(target);
  return &targets_.back();
}

void Layer::record(VkCommandBuffer cmd, std::uint32_t slot, VkImage target_image, VkFormat format, VkExtent2D extent) {
  if (failed_) return;
  Target *target = nullptr;
  if (ensure_resources(format, extent)) target = target_for(target_image, extent);
  if (target == nullptr) {
    failed_ = true;
    sampler_.reset();
    std::fprintf(stderr, "[hud] the performance HUD could not be set up; it stays off\n");
    return;
  }

  // The panel on the CPU: the graph on every presentation, the text at its own pace.
  const double now = now_ms();
  const auto record_start = std::chrono::steady_clock::now();
  const bool text_due = now >= next_text_ms_;
  SessionFacts facts = facts_;
  facts.output_width = extent.width;
  facts.output_height = extent.height;
  FrameWindow frames;
  SystemReading reading;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (canvas_->has_graph()) times_.recent_intervals(intervals_, static_cast<std::size_t>(canvas_->graph_width()));
    if (text_due) {
      frames = times_.window(now);
      reading = reading_;
    }
  }
  if (text_due) {
    canvas_->draw_text(hud_lines(mode_, report_line(report_status_, perf::ReportStatus::now_ms()), facts, frames, reading));
    next_text_ms_ = now + kTextPeriodMs;
  }
  canvas_->draw_graph(intervals_);  // nothing for the FPS only panel

  // Only the rows that changed go to the image. The first time, all of them.
  const std::uint32_t width = static_cast<std::uint32_t>(canvas_->width());
  VkBufferImageCopy copies[2] = {};
  std::uint32_t copy_count = 0;
  const auto add_rows = [&](int top, int rows) {
    std::memcpy(staging_[slot].mapped + static_cast<std::size_t>(top) * width, canvas_->pixels().data() + static_cast<std::size_t>(top) * width,
                static_cast<std::size_t>(rows) * width * sizeof(std::uint32_t));
    VkBufferImageCopy &copy = copies[copy_count++];
    copy.bufferOffset = static_cast<VkDeviceSize>(top) * width * sizeof(std::uint32_t);
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = {0, top, 0};
    copy.imageExtent = {width, static_cast<std::uint32_t>(rows), 1};
  };
  if (!image_ready_) {
    add_rows(0, canvas_->height());
  } else {
    if (text_due) add_rows(canvas_->text_top(), canvas_->text_rows());
    if (canvas_->has_graph()) add_rows(canvas_->graph_top(), canvas_->graph_rows());
  }
  VkImageMemoryBarrier to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  to_copy.oldLayout = image_ready_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_copy.image = image_;
  to_copy.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_copy.srcAccessMask = image_ready_ ? VK_ACCESS_SHADER_READ_BIT : 0;
  to_copy.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, image_ready_ ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_copy);
  vkCmdCopyBufferToImage(cmd, staging_[slot].buffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_count, copies);
  VkImageMemoryBarrier to_read = to_copy;
  to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_read);
  image_ready_ = true;

  // Over the picture, in the window's top right corner.
  const int panel_width = std::min(canvas_->width(), static_cast<int>(extent.width));
  const int panel_height = std::min(canvas_->height(), static_cast<int>(extent.height));
  const int left = std::max(0, static_cast<int>(extent.width) - panel_width - kMarginPixels);
  const int top = std::min(kMarginPixels, static_cast<int>(extent.height) - panel_height);
  VkRenderPassBeginInfo begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  begin.renderPass = render_pass_;
  begin.framebuffer = target->framebuffer;
  begin.renderArea.offset = {left, top};
  begin.renderArea.extent = {static_cast<std::uint32_t>(panel_width), static_cast<std::uint32_t>(panel_height)};
  vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
  const VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &begin.renderArea);
  QuadConstants quad{};
  quad.origin[0] = 2.0f * static_cast<float>(left) / static_cast<float>(extent.width) - 1.0f;
  quad.origin[1] = 2.0f * static_cast<float>(top) / static_cast<float>(extent.height) - 1.0f;
  quad.size[0] = 2.0f * static_cast<float>(canvas_->width()) / static_cast<float>(extent.width);
  quad.size[1] = 2.0f * static_cast<float>(canvas_->height()) / static_cast<float>(extent.height);
  vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof quad, &quad);
  vkCmdDraw(cmd, 4, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  record_us_ += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - record_start).count();
  if (++records_ % 1200 == 0) {
    std::fprintf(stderr, "[hud] panel: %llu presentations, %.1f us of processor per presentation\n",
                 static_cast<unsigned long long>(records_), record_us_ / static_cast<double>(records_));
  }
}

}  // namespace gt4::hw::hud
