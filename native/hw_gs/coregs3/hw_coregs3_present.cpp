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

#include "coregs3_live_world.hpp"
#include "coregs3_lighting.hpp"
#include "coregs3_presentation.hpp"
#include "coregs3_present.hpp"
#include "coregs3_render.hpp"
#include "coregs3_sampler.hpp"
#include "coregs3_scene.hpp"
#include "coregs3_shape.hpp"
#include "coregs3_texture_frame.hpp"
#include "coregs3_vertex.hpp"
#include "coregs3_vram.hpp"
#include "coregs3_world.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace gt4::hw::coregs3 {
namespace {

constexpr VkFormat kTargetFormat = VK_FORMAT_R32_UINT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr std::uint32_t kMaxVerticesPerFrame = 1u << 20;  // 1,048,576 vertices = 16 MiB; a night frame uses ~12k
constexpr VkDeviceSize kInitialVertexBytes = 1u << 20;

// TASK-090 live world. The decoded textures are RGBA8 in one storage image; the world vertex stage reads the captured
// 0xC1 columns from a 112-byte push constant and the fragment stage samples nearest at level 0, applies MODULATE, the
// alpha test and FRAME.FBMSK (see shaders/coregs3_world.frag). Depth is 1/cw like the car pass, so both pipelines share
// the private D32 image and the world occludes the car correctly (and the car occludes the world).
constexpr VkFormat kAtlasFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr std::uint32_t kAtlasWidth = 2048;
constexpr std::uint32_t kAtlasHeight = 2048;
constexpr std::uint32_t kMaxWorldVertices = 1u << 19;  // 524,288 vertices = 12 MiB
constexpr VkDeviceSize kInitialWorldVertexBytes = 1u << 20;

// The world push-constant flags (mirrored by the fragment shader's kFlag* constants).
constexpr std::uint32_t kWorldFlagTextured = 1u << 0;
constexpr std::uint32_t kWorldFlagAlphaTest = 1u << 1;
constexpr std::uint32_t kWorldFlagFog = 1u << 2;  // the item's PRIM.FGE: apply the per-vertex fog toward FOGCOL
constexpr std::uint32_t kWorldFlagBlendDeferred = 1u << 3;
constexpr std::uint32_t kWorldFlagAlphaFailDefaulted = 1u << 4;
constexpr std::uint32_t kWorldFlagLinear = 1u << 5;  // the item's TEX1 filter is linear: GS bilinear sampling

struct WorldVertex {
  float x = 0.0f;  // captured model units (int32 lane, raw int16, or V3-32 float)
  float y = 0.0f;
  float z = 0.0f;
  std::uint8_t red = 255;
  std::uint8_t green = 255;
  std::uint8_t blue = 255;
  std::uint8_t alpha = 255;
  float u = 0.0f;
  float v = 0.0f;
  // The emitted per-vertex fog (0..255), interpolated screen-linearly like the GS fog lane. 255 is the no-fog identity
  // in the GS mix ((255*c + 0*fogcol) >> 8), used for an item whose tap record carried no fog.
  float fog = 255.0f;
};
static_assert(sizeof(WorldVertex) == 28, "the world vertex record must match the pipeline's 28-byte stride");

// The layout the two world stages declare in shaders/coregs3_world_push.glsl (std140: vec4 columns, then three uvec4s).
struct WorldPush {
  float columns[16] = {};  // c0..c3, each four lanes, in column order (c0.x..w, c1.x..w, ...)
  std::uint32_t tex0_lo = 0;
  std::uint32_t tex0_hi = 0;
  std::uint32_t clamp_lo = 0;
  std::uint32_t clamp_hi = 0;
  std::uint32_t atlas_origin = 0;  // x | y << 16
  std::uint32_t atlas_rect = 0;    // x0 | y0 << 16 (the decoded rectangle's origin in texture coordinates)
  std::uint32_t atlas_size = 0;    // width | height << 16
  std::uint32_t fog_colour = 0;  // FOGCOL (R in bits 0-7, G 8-15, B 16-23) from the tapped/pass-journal state
  std::uint32_t test = 0;
  std::uint32_t alpha = 0;
  std::uint32_t fbmsk = 0;
  std::uint32_t flags = 0;
};
static_assert(sizeof(WorldPush) == 112, "the world push constant must match coregs3_world_push.glsl");

struct WorldRange {
  std::uint32_t first = 0;  // first WorldVertex
  std::uint32_t count = 0;  // vertices (a multiple of three)
  bool sky = false;         // the sky pass: drawn as the background, no depth test or write
  std::uint64_t object_id = 0;        // the timeline's object key, for the interpolated matrix of a tick
  std::uint32_t object_instance = 0;
  WorldPush push;
};

// Where one decoded texture sits in the atlas.
struct AtlasPlacement {
  std::uint32_t x = 0;
  std::uint32_t y = 0;
};

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

  // TASK-090 live world: the second pipeline pair, its vertex buffer and the texture atlas.
  bool live_world_enabled = false;
  bool live_world_background = false;  // GT4_COREGS3_WORLD_BACKGROUND=1: draw the sky and far underlay too
  bool live_world_bilinear = true;     // GT4_COREGS3_WORLD_FILTER=nearest is the bilinear A/B
  bool live_world_fog = true;          // GT4_COREGS3_WORLD_FOG=0 is the fog A/B
  bool present_debug = false;          // GT4_COREGS3_PRESENT_DEBUG=1: one line per render with the claimed frame

  VkShaderModule world_vertex_module = VK_NULL_HANDLE;
  VkShaderModule world_fragment_module = VK_NULL_HANDLE;
  VkPipeline world_pipeline = VK_NULL_HANDLE;      // depth test GEQUAL, depth write
  VkPipeline world_sky_pipeline = VK_NULL_HANDLE;  // the sky background: no depth test, no depth write
  VkBuffer world_vertex_buffer = VK_NULL_HANDLE;
  VkDeviceMemory world_vertex_memory = VK_NULL_HANDLE;
  void *world_vertex_mapped = nullptr;
  VkDeviceSize world_vertex_capacity = 0;
  VkImage atlas_image = VK_NULL_HANDLE;
  VkDeviceMemory atlas_memory = VK_NULL_HANDLE;
  VkImageView atlas_view = VK_NULL_HANDLE;
  VkBuffer atlas_staging = VK_NULL_HANDLE;
  VkDeviceMemory atlas_staging_memory = VK_NULL_HANDLE;
  void *atlas_staging_mapped = nullptr;
  bool atlas_general = false;  // the atlas was transitioned to GENERAL and copied at least once
  VkQueryPool query_pool = VK_NULL_HANDLE;
  bool queries_enabled = false;

  // The world build of the newest claimed frame. Written by note_guest_frame (the claim thread) and read by render; one
  // mutex covers both sides. The texture table is local to the build (the ranges hold copies of what they need).
  std::mutex world_mutex;
  std::vector<WorldVertex> world_vertices;
  std::vector<WorldRange> world_ranges;
  std::vector<std::uint8_t> world_atlas_pixels;  // kAtlasWidth x kAtlasHeight x 4, only the used rows are meaningful
  std::uint32_t world_atlas_used_w = 0;
  std::uint32_t world_atlas_used_h = 0;
  std::uint64_t world_built_vblank = ~0ull;
  std::uint64_t world_uploaded_vblank = ~0ull;
  bool world_valid = false;
  bool world_pending = false;  // a claimed frame the world has not been built for yet
  std::uint64_t last_world_samples = 0;  // occlusion-query samples of the last tick's world draws
  std::uint64_t last_car_samples = 0;    // and of its car draws (diagnostic; not reported as a gate)
  std::uint32_t last_world_draw_calls = 0;
  // The empty base SLM image: the live world decodes only the windows a job's own record carries, so a texture page the
  // capture does not cover is refused by the decoder instead of reading stale bytes.
  gt4::coregs3::SlmImage world_base_slm;

  // Reused between frames: the claim swaps the ring's buffers into these vectors, and build_frame refills them.
  gt4::coregs3::FrameSnapshot frame;
  bool has_frame = false;      // at least one complete frame was claimed
  bool frame_drew_any = false;  // a tick of this guest frame already saved the composed base image
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

  // Releases the live world's Vulkan objects. The pipelines and shader modules go before the pipeline layout that owns
  // them, which shutdown() destroys next.
  void destroy_world() {
    const VkDevice device = config.device;
    if (device == VK_NULL_HANDLE) return;
    if (atlas_staging_mapped != nullptr) {
      gt4_hwd_vkUnmapMemory(device, atlas_staging_memory);
      atlas_staging_mapped = nullptr;
    }
    if (atlas_staging != VK_NULL_HANDLE) gt4_hwd_vkDestroyBuffer(device, atlas_staging, nullptr);
    if (atlas_staging_memory != VK_NULL_HANDLE) gt4_hwd_vkFreeMemory(device, atlas_staging_memory, nullptr);
    if (atlas_view != VK_NULL_HANDLE) gt4_hwd_vkDestroyImageView(device, atlas_view, nullptr);
    if (atlas_image != VK_NULL_HANDLE) gt4_hwd_vkDestroyImage(device, atlas_image, nullptr);
    if (atlas_memory != VK_NULL_HANDLE) gt4_hwd_vkFreeMemory(device, atlas_memory, nullptr);
    atlas_staging = VK_NULL_HANDLE;
    atlas_staging_memory = VK_NULL_HANDLE;
    atlas_view = VK_NULL_HANDLE;
    atlas_image = VK_NULL_HANDLE;
    atlas_memory = VK_NULL_HANDLE;
    atlas_general = false;
    if (world_vertex_mapped != nullptr) {
      gt4_hwd_vkUnmapMemory(device, world_vertex_memory);
      world_vertex_mapped = nullptr;
    }
    if (world_vertex_buffer != VK_NULL_HANDLE) gt4_hwd_vkDestroyBuffer(device, world_vertex_buffer, nullptr);
    if (world_vertex_memory != VK_NULL_HANDLE) gt4_hwd_vkFreeMemory(device, world_vertex_memory, nullptr);
    world_vertex_buffer = VK_NULL_HANDLE;
    world_vertex_memory = VK_NULL_HANDLE;
    world_vertex_capacity = 0;
    if (world_pipeline != VK_NULL_HANDLE) gt4_hwd_vkDestroyPipeline(device, world_pipeline, nullptr);
    if (world_sky_pipeline != VK_NULL_HANDLE) gt4_hwd_vkDestroyPipeline(device, world_sky_pipeline, nullptr);
    if (world_vertex_module != VK_NULL_HANDLE) gt4_hwd_vkDestroyShaderModule(device, world_vertex_module, nullptr);
    if (world_fragment_module != VK_NULL_HANDLE) gt4_hwd_vkDestroyShaderModule(device, world_fragment_module, nullptr);
    if (query_pool != VK_NULL_HANDLE) gt4_hwd_vkDestroyQueryPool(device, query_pool, nullptr);
    world_pipeline = VK_NULL_HANDLE;
    world_sky_pipeline = VK_NULL_HANDLE;
    world_vertex_module = VK_NULL_HANDLE;
    world_fragment_module = VK_NULL_HANDLE;
    query_pool = VK_NULL_HANDLE;
    queries_enabled = false;
  }

  void shutdown() {
    const VkDevice device = config.device;
    if (device == VK_NULL_HANDLE) return;
    destroy_depth();
    destroy_base();
    destroy_world();
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

    VkDescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1].binding = 1;  // TASK-090 live world: the decoded texture atlas
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount = 2;
    set_info.pBindings = bindings;
    if (gt4_hwd_vkCreateDescriptorSetLayout(config.device, &set_info, nullptr, &set_layout) != VK_SUCCESS) {
      error = "vkCreateDescriptorSetLayout failed for the CoreGS 3.0 present layer";
      return false;
    }

    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(WorldPush);  // vf24..vf27 for the car stage; the world stage adds its item state
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
    pool_size.descriptorCount = 2;  // the display image and the world texture atlas
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
    if (!create_world_objects(error)) return false;
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

  // ---- TASK-090 live world resources ------------------------------------------------------------------------------

  bool create_world_vertex_buffer(VkDeviceSize bytes, std::string &error) {
    if (world_vertex_mapped != nullptr) {
      gt4_hwd_vkUnmapMemory(config.device, world_vertex_memory);
      world_vertex_mapped = nullptr;
    }
    if (world_vertex_buffer != VK_NULL_HANDLE) gt4_hwd_vkDestroyBuffer(config.device, world_vertex_buffer, nullptr);
    if (world_vertex_memory != VK_NULL_HANDLE) gt4_hwd_vkFreeMemory(config.device, world_vertex_memory, nullptr);
    world_vertex_buffer = VK_NULL_HANDLE;
    world_vertex_memory = VK_NULL_HANDLE;
    world_vertex_capacity = 0;
    const VkDeviceSize size = std::max<VkDeviceSize>(bytes, kInitialWorldVertexBytes);
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (gt4_hwd_vkCreateBuffer(config.device, &buffer_info, nullptr, &world_vertex_buffer) != VK_SUCCESS) {
      error = "vkCreateBuffer failed for the CoreGS 3.0 world vertices";
      return false;
    }
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetBufferMemoryRequirements(config.device, world_vertex_buffer, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    gt4_hwd_vkGetPhysicalDeviceMemoryProperties(config.physical, &memory);
    std::uint32_t type = 0;
    if (!find_memory_type(memory, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type)) {
      error = "no host-visible coherent memory for the CoreGS 3.0 world vertices";
      return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(config.device, &allocate, nullptr, &world_vertex_memory) != VK_SUCCESS) {
      error = "vkAllocateMemory failed for the CoreGS 3.0 world vertices";
      return false;
    }
    if (gt4_hwd_vkBindBufferMemory(config.device, world_vertex_buffer, world_vertex_memory, 0) != VK_SUCCESS) {
      error = "vkBindBufferMemory failed for the CoreGS 3.0 world vertices";
      return false;
    }
    if (gt4_hwd_vkMapMemory(config.device, world_vertex_memory, 0, size, 0, &world_vertex_mapped) != VK_SUCCESS) {
      error = "vkMapMemory failed for the CoreGS 3.0 world vertices";
      return false;
    }
    world_vertex_capacity = size;
    return true;
  }

  bool ensure_world_vertex_buffer(VkDeviceSize bytes, std::string &error) {
    if (world_vertex_capacity >= bytes) return true;
    return create_world_vertex_buffer(std::max<VkDeviceSize>(bytes, world_vertex_capacity * 2u), error);
  }

  // One world pipeline: the same storage-image fragment as the car pass, with the world's 24-byte vertex and 112-byte
  // push constant. `depth_test` is false only for the sky background (no depth test, no depth write).
  bool make_world_pipeline(VkPipeline &pipeline, bool depth_test, std::string &error) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = world_vertex_module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = world_fragment_module;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vertex_binding{};
    vertex_binding.binding = 0;
    vertex_binding.stride = sizeof(WorldVertex);
    vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attributes[4]{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};  // captured model position
    attributes[1] = {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 12};    // V4-8 colour
    attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, 16};     // u, v
    attributes[3] = {3, 0, VK_FORMAT_R32_SFLOAT, 24};        // emitted per-vertex fog (0..255)
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &vertex_binding;
    vertex_input.vertexAttributeDescriptionCount = 4;
    vertex_input.pVertexAttributeDescriptions = attributes;

    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = VK_TRUE;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_state.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_state.depthWriteEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    depth_state.depthBoundsTestEnable = VK_FALSE;
    depth_state.stencilTestEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
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
      error = "vkCreateGraphicsPipelines failed for the CoreGS 3.0 world stage";
      return false;
    }
    return true;
  }

  // The decoded-texture atlas: one storage image the world fragment stage samples with imageLoad.
  bool create_atlas(std::string &error) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = kAtlasFormat;
    image_info.extent = {kAtlasWidth, kAtlasHeight, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (gt4_hwd_vkCreateImage(config.device, &image_info, nullptr, &atlas_image) != VK_SUCCESS) {
      error = "vkCreateImage failed for the CoreGS 3.0 world atlas";
      return false;
    }
    VkMemoryRequirements requirements{};
    gt4_hwd_vkGetImageMemoryRequirements(config.device, atlas_image, &requirements);
    VkPhysicalDeviceMemoryProperties memory{};
    gt4_hwd_vkGetPhysicalDeviceMemoryProperties(config.physical, &memory);
    std::uint32_t type = 0;
    if (!find_memory_type(memory, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, type)) {
      error = "no device-local memory for the CoreGS 3.0 world atlas";
      return false;
    }
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(config.device, &allocate, nullptr, &atlas_memory) != VK_SUCCESS) {
      error = "vkAllocateMemory failed for the CoreGS 3.0 world atlas";
      return false;
    }
    if (gt4_hwd_vkBindImageMemory(config.device, atlas_image, atlas_memory, 0) != VK_SUCCESS) {
      error = "vkBindImageMemory failed for the CoreGS 3.0 world atlas";
      return false;
    }
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = atlas_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = kAtlasFormat;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (gt4_hwd_vkCreateImageView(config.device, &view_info, nullptr, &atlas_view) != VK_SUCCESS) {
      error = "vkCreateImageView failed for the CoreGS 3.0 world atlas";
      return false;
    }
    const VkDeviceSize staging_bytes = static_cast<VkDeviceSize>(kAtlasWidth) * kAtlasHeight * 4u;
    VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer_info.size = staging_bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (gt4_hwd_vkCreateBuffer(config.device, &buffer_info, nullptr, &atlas_staging) != VK_SUCCESS) {
      error = "vkCreateBuffer failed for the CoreGS 3.0 world atlas staging";
      return false;
    }
    gt4_hwd_vkGetBufferMemoryRequirements(config.device, atlas_staging, &requirements);
    if (!find_memory_type(memory, requirements.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, type)) {
      error = "no host-visible coherent memory for the CoreGS 3.0 world atlas staging";
      return false;
    }
    VkMemoryAllocateInfo staging_allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    staging_allocate.allocationSize = requirements.size;
    staging_allocate.memoryTypeIndex = type;
    if (gt4_hwd_vkAllocateMemory(config.device, &staging_allocate, nullptr, &atlas_staging_memory) != VK_SUCCESS) {
      error = "vkAllocateMemory failed for the CoreGS 3.0 world atlas staging";
      return false;
    }
    if (gt4_hwd_vkBindBufferMemory(config.device, atlas_staging, atlas_staging_memory, 0) != VK_SUCCESS) {
      error = "vkBindBufferMemory failed for the CoreGS 3.0 world atlas staging";
      return false;
    }
    if (gt4_hwd_vkMapMemory(config.device, atlas_staging_memory, 0, staging_bytes, 0, &atlas_staging_mapped) !=
        VK_SUCCESS) {
      error = "vkMapMemory failed for the CoreGS 3.0 world atlas staging";
      return false;
    }
    world_atlas_pixels.assign(static_cast<std::size_t>(kAtlasWidth) * kAtlasHeight * 4u, 0);
    return true;
  }

  bool create_world_objects(std::string &error) {
    if (!live_world_enabled) return true;
    if (!format_has(config.physical, kAtlasFormat, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {
      error = "the device cannot use R8G8B8A8_UNORM as a storage image; the live world stays off";
      std::fprintf(stderr, "[coregs3] live world refused: %s\n", error.c_str());
      live_world_enabled = false;
      error.clear();
      return true;  // the car layer keeps working without the world
    }
    if (!make_shader(hwgs::shaders::coregs3_world_vertex_words, hwgs::shaders::coregs3_world_vertex_word_count,
                     world_vertex_module, error) ||
        !make_shader(hwgs::shaders::coregs3_world_fragment_words, hwgs::shaders::coregs3_world_fragment_word_count,
                     world_fragment_module, error)) {
      return false;
    }
    if (!make_world_pipeline(world_pipeline, true, error) || !make_world_pipeline(world_sky_pipeline, false, error))
      return false;
    if (!create_world_vertex_buffer(kInitialWorldVertexBytes, error)) return false;
    if (!create_atlas(error)) return false;
    VkDescriptorImageInfo atlas_descriptor{VK_NULL_HANDLE, atlas_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_set;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &atlas_descriptor;
    gt4_hwd_vkUpdateDescriptorSets(config.device, 1, &write, 0, nullptr);
    VkQueryPoolCreateInfo query_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    query_info.queryType = VK_QUERY_TYPE_OCCLUSION;
    query_info.queryCount = 2;
    if (gt4_hwd_vkCreateQueryPool(config.device, &query_info, nullptr, &query_pool) == VK_SUCCESS) {
      queries_enabled = true;
    } else {
      std::fprintf(stderr, "[coregs3] live world: occlusion queries unavailable; the sample counter stays 0\n");
    }
    return true;
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
      // TASK-090 live world: with the world on, the snapshot also carries its passes (all_draws); this build is the car
      // pass only. With the world off the snapshot is car-only and the filter is inert, so the A/B path is unchanged.
      if (live_world_enabled && draw.pass != gt4::coregs3::kPassCars) continue;
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

  // TASK-090 live world: selects and builds the captured sky/far-underlay/course passes of the frame the claim just
  // swapped in. The selection is the offline world path's shared rule (`coregs3::world_item_exclusion`); the per-item GS
  // state is the K7 pass journal (no `.gt4tap` exists in the live path, so the table's journal branch supplies it), and
  // the pixels come from the live per-job store (`coregs3_live_world.hpp`). Every exclusion, refusal and simplification
  // is counted; nothing is guessed. Called on the claim thread with `world_mutex` held.
  void build_world() {
    world_valid = false;
    world_vertices.clear();
    world_ranges.clear();
    world_atlas_used_w = 0;
    world_atlas_used_h = 0;
    world_built_vblank = frame.vblank;
    gt4::coregs3::present::WorldFrameCounters counts;
    const auto is_world_draw = [](std::uint32_t pass) {
      return pass == gt4::coregs3::kPassSky || pass == gt4::coregs3::kPassFarUnderlay ||
             pass == gt4::coregs3::kPassCourse;
    };

    // 1. The pixels. Each world item's packet address names the VU1 job it was posted as; the live store holds the
    // texture/palette windows that job sampled, copied before it executed. The VramRecord copies own their bytes and
    // live in a stable map, so the table's pointers stay valid for the build.
    std::map<std::uint32_t, gt4::coregs3::VramRecord> live_records;
    std::map<std::uint32_t, const gt4::coregs3::VramRecord *> live_map;
    std::vector<std::uint32_t> world_addresses;  // the addresses the tap snapshot needs (deduplicated below)
    for (const gt4::coregs3::CapturedDraw &draw : frame.draws) {
      if (!is_world_draw(draw.pass)) continue;
      for (std::uint32_t index = 0; index < draw.item_count; ++index) {
        const gt4::coregs3::CapturedItem &item = draw.items[index];
        if (item.packet_bytes == 0 || live_map.count(item.address) != 0) continue;
        const std::shared_ptr<const gt4::coregs3::live_world::JobTexture> record =
            gt4::coregs3::live_world::find(item.address);
        if (record == nullptr) {
          counts.texture_missing += 1;
          continue;
        }
        // The measured phase: the worker's VBlank counter at a job is the capture frame's own + 1 (the tap's records
        // carry the same relation). A record at or above that is this frame's; anything older is the same material
        // uploaded into the same pages in an earlier frame, a counted fallback.
        if (record->vblank >= frame.vblank + 1) {
          counts.texture_live += 1;
        } else {
          counts.texture_stale += 1;
        }
        gt4::coregs3::VramRecord vram;
        vram.kind = gt4::coregs3::vram::kRecordJobTexture;
        vram.label = static_cast<std::uint32_t>(record->job);
        vram.vblank = record->vblank;
        vram.windows = record->windows;
        vram.bytes = record->bytes;
        live_records.emplace(item.address, std::move(vram));
        world_addresses.push_back(item.address);
      }
    }
    for (const auto &entry : live_records) live_map.emplace(entry.first, &entry.second);

    // 2. The per-frame texture table: the emitted (live tap) or record state for the items, the pass journal for the
    // world items' GS state, and the live job record for their pixels. The base image is the frame-end GS local memory
    // over the measured world range (the `.gt4vram` vblank record's role offline): a page no job window covers decodes
    // from it, never from a zero fill. Without a base yet the table refuses those decodes and counts them.
    {
      std::vector<std::uint8_t> base_bytes;
      std::uint32_t base_first = 0, base_blocks = 0;
      std::uint64_t base_vblank = 0;
      auto bytes = world_base_slm.local.bytes();
      std::fill(bytes.begin(), bytes.end(), 0);
      world_base_slm.covered.clear();
      if (gt4::coregs3::live_world::copy_vram_base(base_bytes, base_first, base_blocks, base_vblank)) {
        const std::size_t at = static_cast<std::size_t>(base_first) * gt4::coregs3::vram::kBlockBytes;
        if (at + base_bytes.size() <= bytes.size()) {
          std::memcpy(bytes.data() + at, base_bytes.data(), base_bytes.size());
          world_base_slm.cover(gt4::coregs3::vram::Window{base_first, base_blocks});
        }
      }
      if (present_debug)
        std::fprintf(stderr, "[coregs3] world base: blocks 0x%04X..0x%04X vblank %llu\n", base_first,
                     base_blocks == 0 ? 0u : base_first + base_blocks - 1u,
                     static_cast<unsigned long long>(base_vblank));
    }
    gt4::coregs3::TextureFrameOptions texture_options;
    texture_options.world_channels = true;
    texture_options.live_job_textures = &live_map;
    // TASK-090 live world: the job records captured on the GS worker are the same PATH1 bytes the `.gt4tap` file holds.
    // Synthesizing their record view lets the offline pairing attach the emitted per-item TEX0/TEX1/CLAMP state, the
    // emitted UVs and the per-vertex fog without a capture file; the pixels still come from the live job store. When no
    // record is available the table uses the 40-byte record path and counts the refusals.
    std::vector<std::uint8_t> tap_data;
    std::vector<gt4::coregs3::TapFileRecord> tap_records;
    std::sort(world_addresses.begin(), world_addresses.end());
    world_addresses.erase(std::unique(world_addresses.begin(), world_addresses.end()), world_addresses.end());
    const bool live_tap =
        gt4::coregs3::live_world::build_tap_snapshot(frame.vblank, world_addresses, tap_data, tap_records);
    texture_options.use_tap = live_tap;
    gt4::coregs3::FrameTextureTable table;
    gt4::coregs3::TextureFrameCounters texture_counters;
    std::string table_error;
    if (!gt4::coregs3::build_frame_texture_table(frame, world_base_slm, live_tap ? tap_data.data() : nullptr,
                                                 tap_records, texture_options, table, texture_counters, table_error)) {
      std::fprintf(stderr, "[coregs3] live world: texture table refused: %s\n", table_error.c_str());
      counts.refused_draws += 1;
      gt4::coregs3::present::note_world_frame(counts);
      return;
    }
    counts.mip_defaulted += static_cast<std::uint32_t>(texture_counters.records_level0_fallback);
    if (present_debug)
      std::fprintf(stderr,
                   "[coregs3] world table: tap %d textured %llu no_record %llu no_uv %llu decode_refused %llu "
                   "state %llu record_fallback %llu live_job %llu live_missing %llu paired %llu defaulted %llu "
                   "mip %llu\n",
                   live_tap ? 1 : 0, static_cast<unsigned long long>(texture_counters.items_textured),
                   static_cast<unsigned long long>(texture_counters.items_no_texture),
                   static_cast<unsigned long long>(texture_counters.items_no_uv),
                   static_cast<unsigned long long>(texture_counters.items_decode_refused),
                   static_cast<unsigned long long>(texture_counters.world_state_decoded),
                   static_cast<unsigned long long>(texture_counters.world_record_fallback),
                   static_cast<unsigned long long>(texture_counters.world_live_job),
                   static_cast<unsigned long long>(texture_counters.world_live_job_missing),
                   static_cast<unsigned long long>(texture_counters.tap_items_paired),
                   static_cast<unsigned long long>(texture_counters.tap_items_defaulted),
                   static_cast<unsigned long long>(texture_counters.records_level0_fallback));
    if (present_debug) {
      const gt4::coregs3::live_world::Counters &lw = gt4::coregs3::live_world::counters();
      std::fprintf(stderr, "[coregs3] world tap: snapshot jobs %llu records %llu refused %llu; state_refused %llu\n",
                   static_cast<unsigned long long>(lw.snapshot_jobs.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(lw.snapshot_vertices.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(lw.snapshot_refused.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(texture_counters.world_state_refused));
      for (std::size_t index = 0; index < texture_counters.state_refusals.size() && index < 4; ++index)
        std::fprintf(stderr, "[coregs3] world tap refusal: %s\n", texture_counters.state_refusals[index].c_str());
    }

    // 3. Select and build. Instance numbering is the timeline's exactly (accepted draws in draw order, per id), so the
    // per-tick overrides find the same object.
    std::vector<std::pair<std::uint64_t, std::uint32_t>> instances;
    std::map<const gt4::coregs3::SampledTexture *, AtlasPlacement> atlas;
    std::uint32_t atlas_x = 0, atlas_y = 0, atlas_row_height = 0;
    for (const gt4::coregs3::CapturedDraw &draw : frame.draws) {
      const gt4::coregs3::DrawMatrix matrix = gt4::coregs3::draw_matrix(draw);
      if (!matrix.valid) continue;
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
      if (!is_world_draw(draw.pass)) {
        // The reflection pass joins when its alpha-only contribution is drawn (the offline wet-reflection milestone);
        // this first live world leaves it to the GS base and counts it.
        if (draw.pass == gt4::coregs3::kPassReflection) counts.skipped_reflection += draw.item_count;
        continue;
      }
      if (!live_world_background &&
          (draw.pass == gt4::coregs3::kPassSky || draw.pass == gt4::coregs3::kPassFarUnderlay)) {
        // The sky and the far underlay are the pass's own background layers: the sky is a skybox that covers the whole
        // viewport and the far underlay a distant fill. Drawing either over the CoreGS 2.0 image would hide the GS sky,
        // the HUD and every 2D layer the base already has, so the first live world keeps the base's background and draws
        // only the course (`GT4_COREGS3_WORLD_BACKGROUND=1` restores both). The items are counted per pass.
        for (std::uint32_t index = 0; index < draw.item_count; ++index) {
          if (draw.items[index].packet_bytes == 0) continue;
          if (draw.pass == gt4::coregs3::kPassSky) ++counts.sky_skipped;
          else ++counts.far_skipped;
        }
        continue;
      }

      const gt4::coregs3::ChannelScan scan =
          gt4::coregs3::scan_channels(frame.packets.data(), frame.packets.size(), draw, true);
      const std::vector<gt4::coregs3::TextureBinding> *bindings = table.for_draw(draw.sequence);
      std::vector<std::uint8_t> item_mask(draw.item_count, 0);
      std::uint32_t selected = 0;
      for (std::uint32_t index = 0; index < draw.item_count; ++index) {
        const gt4::coregs3::CapturedItem &item = draw.items[index];
        gt4::coregs3::WorldItemInput input;
        input.pass = draw.pass;
        input.run_valid = item.run_valid != 0;
        input.dispatch = input.run_valid ? (item.run_itop & 0x3FFu) * 8u : 0u;
        input.has_position = index < scan.items.size() && scan.items[index].has_position && item.packet_bytes != 0;
        const gt4::coregs3::MorphBlend morph = gt4::coregs3::morph_blend_of(draw);
        input.shape_status =
            item.packet_bytes != 0
                ? gt4::coregs3::world_shape_status(frame.packets.data(), frame.packets.size(), item, morph, nullptr)
                : gt4::coregs3::WorldShapeStatus::unsupported_program;
        const gt4::coregs3::SampleState *state =
            bindings != nullptr && index < bindings->size() ? &(*bindings)[index].state : nullptr;
        if (state != nullptr) {
          input.state_known = state->from_tap;
          input.xyoffset_known = state->xyoffset_known;
          input.xyoffset = state->xyoffset;
          input.test_known = state->test_known;
          input.test = state->test;
          input.test_zero = state->world_test_zero;
        }
        switch (gt4::coregs3::world_item_exclusion(input)) {
        case gt4::coregs3::WorldExclusion::none: item_mask[index] = 1; ++selected; break;
        case gt4::coregs3::WorldExclusion::itop_other: ++counts.excluded_itop; break;
        case gt4::coregs3::WorldExclusion::shape_refused: ++counts.excluded_shape; break;
        case gt4::coregs3::WorldExclusion::no_position: ++counts.excluded_no_position; break;
        case gt4::coregs3::WorldExclusion::no_state: ++counts.excluded_no_state; break;
        case gt4::coregs3::WorldExclusion::mirror: ++counts.excluded_mirror; break;
        default: break;
        }
      }
      if (selected == 0) continue;
      gt4::coregs3::MeshOptions mesh_options;
      mesh_options.world_channels = true;
      mesh_options.item_mask = item_mask.data();
      gt4::coregs3::Mesh mesh;
      std::string mesh_error;
      if (!gt4::coregs3::build_mesh(frame.packets.data(), frame.packets.size(), draw, mesh_options, mesh, mesh_error) ||
          mesh.indices.empty()) {
        ++counts.refused_draws;
        continue;
      }
      ++counts.draws;
      counts.items += selected;
      counts.vertices += mesh.vertices.size();
      counts.triangles += mesh.indices.size() / 3;

      // One range per contiguous run of triangles of one item: each carries its item's GS state, texture binding and
      // emitted per-vertex fog. An item without a usable texture binding is skipped (the CoreGS 2.0 base already holds
      // the faithful pixels there) and counted by reason, never drawn with a constant bright vertex colour.
      std::vector<std::uint32_t> first_vertex(mesh.vertices.size(), 0xFFFFFFFFu);
      for (std::uint32_t index = 0; index < mesh.vertices.size(); ++index)
        if (first_vertex[mesh.vertices[index].item] == 0xFFFFFFFFu) first_vertex[mesh.vertices[index].item] = index;
      WorldRange current;
      bool have_range = false;
      std::uint32_t current_item = 0xFFFFFFFFu;
      std::uint32_t skipped_item = 0xFFFFFFFFu;
      const std::vector<std::uint8_t> *current_fog = nullptr;
      std::uint32_t current_first = 0;
      bool current_fog_on = false;
      const auto close_range = [&] {
        if (!have_range) return;
        if (current.count != 0) world_ranges.push_back(current);
        have_range = false;
      };
      // Fills `next` with the item's state and texture; false when the item has no usable binding (counted by reason).
      const auto open_range = [&](std::uint32_t item_index, bool item_has_uv) -> bool {
        WorldRange next;
        next.sky = draw.pass == gt4::coregs3::kPassSky;
        next.object_id = key.id;
        next.object_instance = key.instance;
        WorldPush &push = next.push;
        for (int column = 0; column < 4; ++column)
          for (int row = 0; row < 4; ++row) push.columns[column * 4 + row] = matrix.row[row][column];
        const gt4::coregs3::TextureBinding *binding =
            bindings != nullptr && item_index < bindings->size() ? &(*bindings)[item_index] : nullptr;
        const gt4::coregs3::SampleState state =
            binding != nullptr ? binding->state : gt4::coregs3::measured_sample_state();
        push.test = static_cast<std::uint32_t>(state.test & 0xFFFFFFFFu);
        push.alpha = static_cast<std::uint32_t>(state.alpha & 0xFFFFFFFFu);
        push.fbmsk = state.fbmsk;
        push.fog_colour = state.fog_colour & 0xFFFFFFu;
        std::uint32_t flags = 0;
        const bool alpha_test = (state.test & 1u) != 0;
        const std::uint32_t afail = static_cast<std::uint32_t>((state.test >> 12) & 3u);
        if (alpha_test) {
          flags |= kWorldFlagAlphaTest;
          if (afail != 0) {
            flags |= kWorldFlagAlphaFailDefaulted;
            ++counts.alpha_fail_defaulted;
          }
        }
        // Fog: the emitted per-vertex fog toward FOGCOL when the item's PRIM.FGE is on. The sky pass's measured state
        // has no fog; it is drawn only with GT4_COREGS3_WORLD_BACKGROUND and stays unfogged then (the offline rule).
        if (live_world_fog && ((state.prim >> 5) & 1u) != 0 && draw.pass != gt4::coregs3::kPassSky)
          flags |= kWorldFlagFog;
        if (((state.prim >> 6) & 1u) != 0) {
          flags |= kWorldFlagBlendDeferred;
          ++counts.blend_deferred;
        }
        if (draw.pass != gt4::coregs3::kPassSky) {
          // The world pipeline implements the measured course rule (ZTE on, ZTST GEQUAL). A pass or item whose journal
          // TEST asks for another depth rule is drawn with that rule and counted, never silently assumed to match.
          const std::uint32_t zte = (push.test >> 16) & 1u;
          const std::uint32_t ztst = (push.test >> 17) & 3u;
          if (zte == 0 || ztst != 2) ++counts.depth_defaulted;
        }
        bool textured = false;
        if (binding != nullptr && binding->texture != nullptr &&
            gt4::coregs3::check_world_binding(*binding) == gt4::coregs3::BindingStatus::ok) {
          const gt4::coregs3::SampledTexture &sampler = *binding->texture;
          auto found = atlas.find(&sampler);
          if (found == atlas.end()) {
            if (sampler.rgba == nullptr || sampler.width == 0 || sampler.height == 0 ||
                sampler.width > kAtlasWidth || sampler.height > kAtlasHeight) {
              ++counts.atlas_refused;
            } else {
              if (atlas_x + sampler.width > kAtlasWidth) {
                atlas_x = 0;
                atlas_y += atlas_row_height;
                atlas_row_height = 0;
              }
              if (atlas_y + sampler.height > kAtlasHeight) {
                ++counts.atlas_refused;
              } else {
                AtlasPlacement placement;
                placement.x = atlas_x;
                placement.y = atlas_y;
                for (std::uint32_t row = 0; row < sampler.height; ++row)
                  std::memcpy(world_atlas_pixels.data() +
                                  (static_cast<std::size_t>(placement.y + row) * kAtlasWidth + placement.x) * 4u,
                              sampler.rgba + static_cast<std::size_t>(row) * sampler.width * 4u,
                              static_cast<std::size_t>(sampler.width) * 4u);
                atlas_x += sampler.width;
                atlas_row_height = std::max(atlas_row_height, sampler.height);
                world_atlas_used_w = std::max(world_atlas_used_w, placement.x + sampler.width);
                world_atlas_used_h = std::max(world_atlas_used_h, placement.y + sampler.height);
                found = atlas.emplace(&sampler, placement).first;
                ++counts.atlas_textures;
              }
            }
          }
          if (found != atlas.end()) {
            const AtlasPlacement &placement = found->second;
            push.tex0_lo = static_cast<std::uint32_t>(sampler.source_tex0 & 0xFFFFFFFFu);
            push.tex0_hi = static_cast<std::uint32_t>(sampler.source_tex0 >> 32);
            push.clamp_lo = static_cast<std::uint32_t>(sampler.source_clamp & 0xFFFFFFFFu);
            push.clamp_hi = static_cast<std::uint32_t>(sampler.source_clamp >> 32);
            push.atlas_origin = placement.x | (placement.y << 16);
            push.atlas_rect = (sampler.x0 | (sampler.y0 << 16));
            push.atlas_size = (sampler.width | (sampler.height << 16));
            flags |= kWorldFlagTextured;
            textured = true;
            if (sampler.mmag != 0 || sampler.mmin != 0) {
              if (live_world_bilinear) {
                flags |= kWorldFlagLinear;
                ++counts.filter_linear;
              } else {
                ++counts.filter_defaulted;  // the A/B forces nearest
              }
            }
          }
        }
        if (!textured) {
          // Name the reason and leave the pixel to the CoreGS 2.0 base: a missing binding never becomes a bright
          // constant colour.
          ++counts.texture_untextured;
          if (binding != nullptr && binding->texture != nullptr) {
            ++counts.texture_refused;
          } else if (item_index < draw.item_count &&
                     draw.items[item_index].texture_record == gt4::coregs3::kNoRecord) {
            ++counts.texture_no_record;
          } else if (!item_has_uv) {
            ++counts.texture_no_uv;
          } else if (gt4::coregs3::light_program_of_item(draw.items[item_index]) ==
                     gt4::coregs3::LightProgram::body_paint) {
            ++counts.texture_texgen;
          } else {
            ++counts.texture_refused;
          }
          return false;
        }
        push.flags = flags;
        current = next;
        have_range = true;
        return true;
      };

      for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3) {
        const gt4::coregs3::MeshVertex &first = mesh.vertices[mesh.indices[triangle]];
        if (!have_range && first.item == skipped_item) continue;
        if (!have_range || first.item != current_item) {
          close_range();
          current_item = first.item;
          const gt4::coregs3::TextureBinding *binding =
              bindings != nullptr && first.item < bindings->size() ? &(*bindings)[first.item] : nullptr;
          const bool item_has_uv =
              first_vertex[first.item] < mesh.vertices.size() && mesh.vertices[first_vertex[first.item]].has_uv;
          if (!open_range(first.item, item_has_uv)) {
            skipped_item = first.item;
            current_fog = nullptr;
            continue;
          }
          skipped_item = 0xFFFFFFFFu;
          current_fog = binding != nullptr && !binding->vertex_fog.empty() ? &binding->vertex_fog : nullptr;
          current_first = first_vertex[first.item];
          current_fog_on = (current.push.flags & kWorldFlagFog) != 0;
          current.first = static_cast<std::uint32_t>(world_vertices.size());
          if (current_fog_on && current_fog != nullptr) ++counts.fog_items;
        }
        for (std::uint32_t corner = 0; corner < 3; ++corner) {
          const gt4::coregs3::MeshVertex &source = mesh.vertices[mesh.indices[triangle + corner]];
          WorldVertex vertex;
          if (source.has_lane_position) {
            vertex.x = static_cast<float>(source.lane_position[0]);
            vertex.y = static_cast<float>(source.lane_position[1]);
            vertex.z = static_cast<float>(source.lane_position[2]);
          } else if (source.has_raw_position) {
            vertex.x = static_cast<float>(source.raw_position[0]);
            vertex.y = static_cast<float>(source.raw_position[1]);
            vertex.z = static_cast<float>(source.raw_position[2]);
          } else {
            vertex.x = source.position[0];
            vertex.y = source.position[1];
            vertex.z = source.position[2];
          }
          vertex.red = source.color[0];
          vertex.green = source.color[1];
          vertex.blue = source.color[2];
          vertex.alpha = source.color[3];
          vertex.u = source.uv[0];
          vertex.v = source.uv[1];
          if (current_fog_on) {
            const std::uint32_t local = static_cast<std::uint32_t>(mesh.indices[triangle + corner]) - current_first;
            if (current_fog != nullptr && local < current_fog->size()) {
              vertex.fog = static_cast<float>((*current_fog)[local]);
            } else {
              ++counts.fog_missing;  // the 255 identity keeps the colour; counted, never guessed
            }
          }
          world_vertices.push_back(vertex);
          current.count += 1;
        }
      }
      close_range();
    }
    world_valid = !world_ranges.empty();
    if (!world_valid && (counts.draws != 0 || counts.excluded_itop + counts.excluded_shape +
                                              counts.excluded_no_state + counts.excluded_mirror +
                                              counts.excluded_no_position != 0)) {
      std::fprintf(stderr, "[coregs3] live world: no drawable world ranges in frame %llu (draws %u)\n",
                   static_cast<unsigned long long>(frame.vblank), counts.draws);
    }
    gt4::coregs3::present::note_world_frame(counts);
  }

  // Claims the newest complete captured frame and records it in the presentation timeline. Called once per guest
  // VBlank by the backend, whether or not a picture is drawn: the timeline must sample every 60 Hz guest state.
  bool note_guest_frame(std::string &error) {
    if (!initialised) {
      error = "the CoreGS 3.0 present layer is not initialised";
      return false;
    }
    if (!gt4::coregs3::present::claim_latest(frame)) {
      // No new complete frame: the previous batch must not be drawn (or presented) again.
      gt4::coregs3::present::clear_host_clock_plan();
      return true;
    }
    has_frame = true;
    frame_drew_any = false;
    gt4::coregs3::present::note_guest_frame(frame);
    if (live_world_enabled) {
      // The build itself is lazy (render() runs it when a tick is due), so a headless run that draws one frame in 200
      // does not pay the world's CPU work on the other 199. The flag is set under the mutex that also guards the build.
      const std::lock_guard<std::mutex> lock(world_mutex);
      world_pending = true;
    }
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

    // TASK-090 live world: a new frame's world data is uploaded once, before its first tick. The claim thread built it
    // under `world_mutex` (render holds it for the whole tick loop), and the previous submission was waited for above.
    const bool upload_world = live_world_enabled && world_valid && world_uploaded_vblank != world_built_vblank;
    if (upload_world) {
      const VkDeviceSize world_bytes = static_cast<VkDeviceSize>(world_vertices.size()) * sizeof(WorldVertex);
      if (!ensure_world_vertex_buffer(world_bytes, error)) return false;
      if (world_bytes != 0)
        std::memcpy(world_vertex_mapped, world_vertices.data(), static_cast<std::size_t>(world_bytes));
      // The atlas staging holds only the used top-left rectangle, row by row, which is the region copied to the image.
      if (world_atlas_used_w != 0 && world_atlas_used_h != 0) {
        std::uint8_t *mapped = static_cast<std::uint8_t *>(atlas_staging_mapped);
        for (std::uint32_t row = 0; row < world_atlas_used_h; ++row)
          std::memcpy(mapped + static_cast<std::size_t>(row) * world_atlas_used_w * 4u,
                      world_atlas_pixels.data() + static_cast<std::size_t>(row) * kAtlasWidth * 4u,
                      static_cast<std::size_t>(world_atlas_used_w) * 4u);
      }
      world_uploaded_vblank = world_built_vblank;
    }

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
    // The atlas copy goes before the render pass: its last use in the previous tick was the fragment stage, and the
    // barrier below hands it to the transfer stage and back to the fragment stage as a readable storage image.
    if (upload_world && world_atlas_used_w != 0 && world_atlas_used_h != 0) {
      VkImageMemoryBarrier to_transfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      to_transfer.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_transfer.oldLayout = atlas_general ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
      to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_transfer.image = atlas_image;
      to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      gt4_hw_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &to_transfer);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {world_atlas_used_w, world_atlas_used_h, 1};
      region.bufferRowLength = world_atlas_used_w;
      gt4_hwd_vkCmdCopyBufferToImage(command, atlas_staging, atlas_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                     &region);
      VkImageMemoryBarrier to_shader = to_transfer;
      to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_shader.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      gt4_hw_vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                                  nullptr, 0, nullptr, 1, &to_shader);
      atlas_general = true;
    }
    if (queries_enabled) gt4_hwd_vkCmdResetQueryPool(command, query_pool, 0, 2);
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
    gt4_hwd_vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout, 0, 1, &descriptor_set, 0,
                                    nullptr);
    last_world_draw_calls = 0;
    // TASK-090 live world: the sky first (background: no depth test or write), then the far underlay and the course
    // (depth = 1/cw, GEQUAL like the car pass, so the world and the cars occlude each other correctly), then the cars.
    if (live_world_enabled && world_valid && !world_ranges.empty()) {
      if (queries_enabled) gt4_hwd_vkCmdBeginQuery(command, query_pool, 0, 0);
      const VkDeviceSize world_offset = 0;
      gt4_hwd_vkCmdBindVertexBuffers(command, 0, 1, &world_vertex_buffer, &world_offset);
      gt4_hwd_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, world_sky_pipeline);
      const auto draw_world_range = [&](const WorldRange &range) {
        WorldPush push = range.push;
        // The presentation timeline's matrix for this object: at 60 Hz it is the captured one (byte-identical), between
        // ticks it is the interpolated one, in the same row-major upload space draw_matrix.row produces.
        for (const gt4::coregs3::DrawTransform &candidate : transforms) {
          if (candidate.key.id != range.object_id || candidate.key.instance != range.object_instance) continue;
          for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
              push.columns[column * 4 + row] = candidate.matrix[row * 4 + column];
          break;
        }
        gt4_hwd_vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                   0, sizeof push, &push);
        gt4_hwd_vkCmdDraw(command, range.count, 1, range.first, 0);
        ++last_world_draw_calls;
      };
      for (const WorldRange &range : world_ranges)
        if (range.sky) draw_world_range(range);
      gt4_hwd_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, world_pipeline);
      for (const WorldRange &range : world_ranges)
        if (!range.sky) draw_world_range(range);
      if (queries_enabled) gt4_hwd_vkCmdEndQuery(command, query_pool, 0);
    }
    if (queries_enabled) gt4_hwd_vkCmdBeginQuery(command, query_pool, 1, 0);
    gt4_hwd_vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkDeviceSize offset = 0;
    gt4_hwd_vkCmdBindVertexBuffers(command, 0, 1, &vertex_buffer, &offset);
    for (const DrawRange &range : ranges) {
      gt4_hwd_vkCmdPushConstants(command, pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof range.columns,
                                 range.columns);
      gt4_hwd_vkCmdDraw(command, range.count, 1, range.first, 0);
    }
    if (queries_enabled) gt4_hwd_vkCmdEndQuery(command, query_pool, 1);
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
    last_world_samples = 0;
    last_car_samples = 0;
    if (queries_enabled) {
      std::uint64_t results[2] = {0, 0};
      if (gt4_hwd_vkGetQueryPoolResults(device, query_pool, 0, 2, sizeof results, results, sizeof(std::uint64_t),
                                        VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        last_world_samples = results[0];
        last_car_samples = results[1];
      }
    }
    destroy_target_view();
    return true;
  }

  // One presentation tick of `batch`: interpolates the instant, builds its mesh and draws it over `target`. A tick with
  // nothing drawable is counted and leaves `target` as it was; the caller then decides whether to present it. The first
  // drawn tick of a guest frame saves the composed image as the base (draw_tick's `first_tick`), so drawing tick k,
  // handing its image to the presenter and then drawing tick k+1 does not leave ghosts.
  bool draw_presentation_tick(std::uint32_t tick, const gt4::coregs3::PresentationClock::Due &batch, std::uint64_t target,
                              std::uint32_t width, std::uint32_t height, const SubmitOverlay &submit, bool &drew,
                              std::string &error) {
    drew = false;
    // TASK-090 live world: the world build of a newly claimed frame happens on its first presentation tick, and the
    // built ranges/vertices/atlas are used by every tick of that frame; one mutex covers the build and the draw. The
    // host clock calls this once per tick, so a tick handed to the presenter and the next one see the same world.
    const std::lock_guard<std::mutex> world_lock(world_mutex);
    if (live_world_enabled && world_pending) {
      build_world();
      world_pending = false;
    }
    const std::uint64_t instant = batch.first + static_cast<std::uint64_t>(tick) * batch.step;
    gt4::coregs3::present::interpolated_transforms(instant, transforms, /*count=*/true);
    std::uint32_t refused = 0;
    std::uint32_t unlit_draws = 0;
    std::uint64_t unlit_vertices = 0;
    gt4::coregs3::LightCounters lighting;
    const bool car_drawn =
        frame.draw_count != 0 &&
        build_frame(transforms.empty() ? nullptr : &transforms, refused, unlit_draws, unlit_vertices, lighting);
    if (!car_drawn) gt4::coregs3::present::note_pass_refused(refused);
    const bool world_drawn = live_world_enabled && world_valid && !world_ranges.empty();
    if (!car_drawn && !world_drawn) return true;
    if (!draw_tick(target, width, height, submit, !frame_drew_any, error)) return false;
    frame_drew_any = true;
    drew = true;
    gt4::coregs3::present::note_draw_tick();
    if (world_drawn) gt4::coregs3::present::note_world_tick(last_world_draw_calls, last_world_samples);
    const std::uint64_t vertex_count = static_cast<std::uint64_t>(vertices.size());
    gt4::coregs3::present::note_render(frame.vblank, static_cast<std::uint32_t>(ranges.size()), refused, unlit_draws,
                                      vertex_count, unlit_vertices, vertex_count / 3u, lighting);
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
    if (present_debug)
      std::fprintf(stderr, "[coregs3] present render frame %llu ticks %u first %llu\n",
                   static_cast<unsigned long long>(frame.vblank), tick_count,
                   static_cast<unsigned long long>(batch.first));
    if (!ensure_depth(width, height, error)) return false;
    if (!ensure_base_image(width, height, error)) return false;
    frame_drew_any = false;  // a whole-batch render draws one guest frame
    for (std::uint32_t tick = first_tick; tick < first_tick + tick_count; ++tick) {
      bool drew = false;
      if (!draw_presentation_tick(tick, batch, target, width, height, submit, drew, error)) return false;
    }
    return true;
  }

  bool render_tick(std::uint64_t target, std::uint32_t width, std::uint32_t height, std::uint32_t tick,
                   const SubmitOverlay &submit, bool &drew, std::string &error) {
    drew = false;
    if (!initialised) {
      error = "the CoreGS 3.0 present layer is not initialised";
      return false;
    }
    if (width == 0 || height == 0) return true;  // nothing to draw into
    if (!has_frame) return true;                 // no captured frame yet
    const gt4::coregs3::PresentationClock::Due batch = gt4::coregs3::present::due_ticks();
    if (tick >= batch.count) return true;  // not a tick of the batch last noted
    if (!ensure_depth(width, height, error)) return false;
    if (!ensure_base_image(width, height, error)) return false;
    return draw_presentation_tick(tick, batch, target, width, height, submit, drew, error);
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
  // The live world's resources are created inside create_objects, so its switch must be known before that call: the
  // store is configured by the hooks in `present`, and only an unusual build order needs the fallback parse here.
  if (!gt4::coregs3::live_world::configured()) gt4::coregs3::live_world::configure_from_environment();
  s.live_world_enabled = gt4::coregs3::live_world::active();
  if (const char *background = std::getenv("GT4_COREGS3_WORLD_BACKGROUND");
      background != nullptr && std::strcmp(background, "1") == 0)
    s.live_world_background = true;
  if (const char *filter = std::getenv("GT4_COREGS3_WORLD_FILTER"); filter != nullptr && std::strcmp(filter, "nearest") == 0)
    s.live_world_bilinear = false;
  if (const char *fog = std::getenv("GT4_COREGS3_WORLD_FOG"); fog != nullptr && std::strcmp(fog, "0") == 0)
    s.live_world_fog = false;
  s.present_debug = std::getenv("GT4_COREGS3_PRESENT_DEBUG") != nullptr;

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
               "%u Hz %s%s, live world %s\n",
               timeline.hz, gt4::coregs3::presentation_mode_name(timeline.mode),
               timeline.selected_tick == UINT32_MAX ? "" : " (diagnostic: one tick per frame)",
               s.live_world_enabled ? "on" : "off");
  return true;
}

void PresentLayer::shutdown() { impl_->shutdown(); }

bool PresentLayer::note_guest_frame(std::string &error) { return impl_->note_guest_frame(error); }

bool PresentLayer::render(std::uint64_t target, std::uint32_t width, std::uint32_t height, const SubmitOverlay &submit,
                          std::string &error) {
  return impl_->render(target, width, height, submit, error);
}

bool PresentLayer::render_tick(std::uint64_t target, std::uint32_t width, std::uint32_t height, std::uint32_t tick,
                               const SubmitOverlay &submit, bool &drew, std::string &error) {
  return impl_->render_tick(target, width, height, tick, submit, drew, error);
}

}  // namespace gt4::hw::coregs3
