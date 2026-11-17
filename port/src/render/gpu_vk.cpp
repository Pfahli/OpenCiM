// The Vulkan device (docs/renderer/vulkan.md §4-5): emulates the GL-shaped gpu::Device on Vulkan 1.3.
//
// * Coordinates stay GL's: a target's rows are stored as GL stores them (NDC y = −1 at row 0), so the engine's
//   bottom-left rectangles, gl_FragCoord and texture reads of targets are unchanged. The frame is drawn into an
//   offscreen back buffer that is copied to the swapchain upside down at present. Front faces are clockwise in
//   Vulkan's terms (the same triangles GL calls counter-clockwise); clip depth is remapped in the vertex shader.
// * Every image lives in VK_IMAGE_LAYOUT_GENERAL; each rendering pass, copy and upload ends with a full memory
//   barrier. Coarse, but simple and correct; finer barriers are an optimisation for later.
// * Rendering (dynamic rendering, LOAD/STORE) starts lazily at the first draw or clear on the bound framebuffer and
//   ends when the framebuffer changes or a copy needs to run, which keeps GL's in-order meaning of uploads and blits.
// * Uniforms: each program keeps its default uniform block on the CPU; a draw copies it into the frame's ring buffer
//   and pushes it with the sampled images (VK_KHR_push_descriptor).
#include <windows.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "platform/window.h"
#include "render/gpu.h"
#include "render/vk_shader.h"

namespace regs::render::gpu {

namespace {

// ---- loader: vulkan-1.dll at run time (no import library) ----

#define REGS_VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties) X(vkEnumerateInstanceLayerProperties)

#define REGS_VK_INSTANCE(X)                                                                                         \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)                              \
  X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)                                   \
  X(vkEnumerateDeviceExtensionProperties) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR)                         \
  X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                              \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define REGS_VK_DEVICE(X)                                                                                            \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
  X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit2) X(vkDeviceWaitIdle) X(vkCreateCommandPool)        \
  X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkResetCommandPool) X(vkBeginCommandBuffer)                 \
  X(vkEndCommandBuffer) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore)  \
  X(vkDestroySemaphore) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)      \
  X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)   \
  X(vkBindBufferMemory) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateSampler)       \
  X(vkDestroySampler) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout)                \
  X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines)  \
  X(vkDestroyPipeline) X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline)     \
  X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport)             \
  X(vkCmdSetScissor) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetDepthTestEnable)                             \
  X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetDepthBiasEnable) X(vkCmdSetDepthBias)             \
  X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCmdBlitImage)  \
  X(vkCmdCopyImage) X(vkCmdResolveImage) X(vkCmdPushDescriptorSetKHR) X(vkCmdClearColorImage)                        \
  X(vkCmdClearDepthStencilImage)

#define REGS_VK_DECLARE(name) PFN_##name name = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
REGS_VK_GLOBAL(REGS_VK_DECLARE)
REGS_VK_INSTANCE(REGS_VK_DECLARE)
REGS_VK_DEVICE(REGS_VK_DECLARE)
#undef REGS_VK_DECLARE

std::string vkError(const char* what, VkResult r) { return std::string(what) + " failed (VkResult " + std::to_string(int(r)) + ")"; }

// ---- memory: blocks per memory type, first-fit free lists (NVIDIA allows only 4096 allocations in all) ----

struct Allocation {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0, size = 0;
  uint8_t* mapped = nullptr;
  int pool = -1, block = -1;  // pool −1: a dedicated allocation
};

class Allocator {
 public:
  void init(VkPhysicalDevice pd, VkDevice d) {
    dev_ = d;
    vkGetPhysicalDeviceMemoryProperties(pd, &props_);
  }
  void shutdown() {
    for (Pool& p : pools_)
      for (Block& b : p.blocks)
        if (b.memory) vkFreeMemory(dev_, b.memory, nullptr);
    pools_.clear();
  }
  bool alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, bool linear, Allocation& out) {
    int type = findType(req.memoryTypeBits, want);
    if (type < 0) return false;
    bool host = (props_.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    if (req.size > kBlockSize / 2) {  // big resources get their own allocation
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, uint32_t(type)};
      if (vkAllocateMemory(dev_, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
      out.offset = 0;
      out.size = req.size;
      out.pool = -1;
      if (host) vkMapMemory(dev_, out.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&out.mapped));
      return true;
    }
    int pi = -1;
    for (size_t i = 0; i < pools_.size(); ++i)
      if (pools_[i].type == type && pools_[i].linear == linear) pi = int(i);
    if (pi < 0) {
      pools_.push_back({type, linear, {}});
      pi = int(pools_.size()) - 1;
    }
    Pool& p = pools_[size_t(pi)];
    for (size_t bi = 0;; ++bi) {
      if (bi == p.blocks.size()) {
        Block b;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, kBlockSize, uint32_t(type)};
        if (vkAllocateMemory(dev_, &ai, nullptr, &b.memory) != VK_SUCCESS) return false;
        if (host) vkMapMemory(dev_, b.memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&b.mapped));
        b.free[0] = kBlockSize;
        p.blocks.push_back(std::move(b));
      }
      Block& b = p.blocks[bi];
      for (auto it = b.free.begin(); it != b.free.end(); ++it) {
        VkDeviceSize start = (it->first + req.alignment - 1) / req.alignment * req.alignment;
        VkDeviceSize end = it->first + it->second;
        if (start + req.size > end) continue;
        VkDeviceSize fo = it->first;
        b.free.erase(it);
        if (start > fo) b.free[fo] = start - fo;
        if (start + req.size < end) b.free[start + req.size] = end - (start + req.size);
        out.memory = b.memory;
        out.offset = start;
        out.size = req.size;
        out.mapped = b.mapped ? b.mapped + start : nullptr;
        out.pool = pi;
        out.block = int(bi);
        return true;
      }
    }
  }
  void free(Allocation& a) {
    if (!a.memory) return;
    if (a.pool < 0) {
      vkFreeMemory(dev_, a.memory, nullptr);
    } else {
      auto& fl = pools_[size_t(a.pool)].blocks[size_t(a.block)].free;
      VkDeviceSize off = a.offset, size = a.size;
      auto next = fl.lower_bound(off);
      if (next != fl.end() && off + size == next->first) {
        size += next->second;
        next = fl.erase(next);
      }
      if (next != fl.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == off) {
          prev->second += size;
          a = Allocation();
          return;
        }
      }
      fl[off] = size;
    }
    a = Allocation();
  }

 private:
  static constexpr VkDeviceSize kBlockSize = 64ull << 20;
  struct Block {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset → size
  };
  struct Pool {
    int type;
    bool linear;
    std::vector<Block> blocks;
  };
  int findType(uint32_t bits, VkMemoryPropertyFlags want) const {
    for (uint32_t i = 0; i < props_.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (props_.memoryTypes[i].propertyFlags & want) == want) return int(i);
    return -1;
  }
  VkDevice dev_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties props_{};
  std::vector<Pool> pools_;
};

// ---- helpers ----

float halfToFloat(uint16_t h) {
  uint32_t s = uint32_t(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF, f;
  if (e == 0) {
    if (m == 0) f = s;
    else {
      e = 127 - 15 + 1;
      while (!(m & 0x400)) m <<= 1, --e;
      f = s | (e << 23) | ((m & 0x3FF) << 13);
    }
  } else if (e == 31) {
    f = s | 0x7F800000u | (m << 13);
  } else {
    f = s | ((e + 127 - 15) << 23) | (m << 13);
  }
  float out;
  std::memcpy(&out, &f, 4);
  return out;
}

uint16_t floatToHalf(float v) {
  uint32_t f;
  std::memcpy(&f, &v, 4);
  uint32_t s = (f >> 16) & 0x8000;
  int e = int((f >> 23) & 0xFF) - 127 + 15;
  uint32_t m = f & 0x7FFFFF;
  if (e <= 0) return uint16_t(s);
  if (e >= 31) return uint16_t(s | 0x7C00);
  return uint16_t(s | uint32_t(e) << 10 | (m + 0x1000) >> 13);
}

bool isDepth(Format f) { return f == Format::Depth24; }
bool isCompressed(Format f) { return f == Format::DXT1 || f == Format::DXT3 || f == Format::DXT5; }

VkBlendFactor vkBlend(Blend b) {
  switch (b) {
    case Blend::Zero: return VK_BLEND_FACTOR_ZERO;
    case Blend::One: return VK_BLEND_FACTOR_ONE;
    case Blend::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case Blend::OneMinusSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case Blend::DstColor: return VK_BLEND_FACTOR_DST_COLOR;
    case Blend::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkSamplerAddressMode vkWrap(Wrap w) {
  switch (w) {
    case Wrap::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case Wrap::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case Wrap::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

void fullBarrier(VkCommandBuffer cmd) {
  VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
  mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.memoryBarrierCount = 1;
  di.pMemoryBarriers = &mb;
  vkCmdPipelineBarrier2(cmd, &di);
}

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
  VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  di.imageMemoryBarrierCount = 1;
  di.pImageMemoryBarriers = &b;
  vkCmdPipelineBarrier2(cmd, &di);
}

// ---- the device ----

class VkDeviceImpl final : public Device {
 public:
  explicit VkDeviceImpl(platform::Window& w) : window_(w) {}
  ~VkDeviceImpl() override { shutdown(); }

  bool init(bool vsync, std::string* error);

  const char* name() const override { return "Vulkan"; }
  std::string info() const override { return info_; }
  float maxAnisotropy() override { return maxAniso_; }

  // ---- textures ----
  uint32_t createTexture(TexKind kind, int levels) override {
    uint32_t h = newTexture();
    Tex& t = textures_[h];
    t.cube = kind == TexKind::Cube;
    t.levelsHint = std::max(1, levels);
    return h;
  }
  void texImage(uint32_t tex, int face, int level, Format f, int w, int h, Pixels p, const void* data,
                size_t bytes = 0) override;
  void texSubImage(uint32_t tex, int level, int x, int y, int w, int h, Pixels p, const void* data) override;
  void setSampler(uint32_t tex, const SamplerDesc& s) override {
    if (Tex* t = texture(tex)) t->sampler = s, t->vkSampler = VK_NULL_HANDLE;
  }
  void setAnisotropy(uint32_t tex, float a) override {
    if (Tex* t = texture(tex)) t->aniso = a, t->vkSampler = VK_NULL_HANDLE;
  }
  void readTextureRed(uint32_t tex, int w, int h, float* out) override;
  void destroyTexture(uint32_t tex) override { releaseTexture(tex); }

  // ---- renderbuffers ----
  uint32_t createRenderbuffer(Format f, int w, int h, int samples) override {
    uint32_t hnd = newTexture();
    Tex& t = textures_[hnd];
    t.renderbuffer = true;
    makeImage(t, f, w, h, 1, std::max(1, samples));
    return hnd | kRenderbufferBit;
  }
  void destroyRenderbuffer(uint32_t rb) override { releaseTexture(rb & ~kRenderbufferBit); }

  // ---- framebuffers ----
  uint32_t createFramebuffer(uint32_t color, uint32_t depth) override {
    Tex* c = color ? texture(color & ~kRenderbufferBit) : nullptr;
    Tex* d = depth ? texture(depth & ~kRenderbufferBit) : nullptr;
    if ((color && (!c || !c->image)) || (depth && (!d || !d->image))) return 0;
    if (c && d && (c->w != d->w || c->h != d->h || c->samples != d->samples)) return 0;
    uint32_t h = 0;
    for (uint32_t i = 1; i < framebuffers_.size(); ++i)
      if (!framebuffers_[i].used) h = i;
    if (!h) {
      h = uint32_t(framebuffers_.size());
      framebuffers_.emplace_back();
    }
    framebuffers_[h] = {true, color & ~kRenderbufferBit, depth & ~kRenderbufferBit};
    return h;
  }
  void destroyFramebuffer(uint32_t fb) override {
    if (fb == 0 || fb >= framebuffers_.size()) return;
    if (renderingFb_ == int(fb)) endRendering();
    framebuffers_[fb] = {};
    if (boundFb_ == fb) boundFb_ = 0;
  }
  void bindFramebuffer(uint32_t fb) override {
    if (fb >= framebuffers_.size() || (fb && !framebuffers_[fb].used)) fb = 0;
    boundFb_ = fb;
  }
  void blit(uint32_t src, uint32_t dst, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
            bool color, bool depth) override;
  void readPixels(uint32_t fb, int x, int y, int w, int h, uint8_t* rgba) override;

  // ---- buffers and vertex layouts ----
  uint32_t createBuffer() override {
    for (uint32_t i = 1; i < buffers_.size(); ++i)
      if (!buffers_[i].used) {
        buffers_[i] = Buf{};
        buffers_[i].used = true;
        return i;
      }
    buffers_.emplace_back();
    buffers_.back().used = true;
    return uint32_t(buffers_.size() - 1);
  }
  void bufferData(uint32_t buf, size_t bytes, const void* data, bool stream) override;
  void bufferSubData(uint32_t buf, size_t offset, size_t bytes, const void* data) override;
  void destroyBuffer(uint32_t buf) override {
    if (!buf || buf >= buffers_.size()) return;
    Buf& b = buffers_[buf];
    if (b.buffer) {
      VkBuffer vb = b.buffer;
      Allocation a = b.mem;
      later([this, vb, a]() mutable {
        vkDestroyBuffer(dev_, vb, nullptr);
        alloc_.free(a);
      });
    }
    b = Buf{};
  }
  uint32_t createVertexInput(const VertexAttrib* attribs, int count, uint32_t indexBuffer) override {
    VertexInput vi;
    vi.used = true;
    vi.attribs.assign(attribs, attribs + count);
    vi.index = indexBuffer;
    // the layout without the buffers (pipelines depend on it, not on which buffers)
    std::string key;
    for (const VertexAttrib& a : vi.attribs) {
      key += std::to_string(a.slot) + "," + std::to_string(a.components) + "," + std::to_string(int(a.type)) + "," +
             std::to_string(a.stride) + "," + std::to_string(a.offset) + "," + std::to_string(bufferSlotOf(attribs, count, a)) + ";";
    }
    auto it = layoutIds_.find(key);
    vi.layout = it != layoutIds_.end() ? it->second : (layoutIds_[key] = int(layoutIds_.size()) + 1);
    for (uint32_t i = 1; i < vertexInputs_.size(); ++i)
      if (!vertexInputs_[i].used) {
        vertexInputs_[i] = std::move(vi);
        return i;
      }
    vertexInputs_.push_back(std::move(vi));
    return uint32_t(vertexInputs_.size() - 1);
  }
  void destroyVertexInput(uint32_t vi) override {
    if (vi && vi < vertexInputs_.size()) vertexInputs_[vi] = VertexInput{};
    if (boundVi_ == vi) boundVi_ = 0;
  }
  void bindVertexInput(uint32_t vi) override { boundVi_ = vi < vertexInputs_.size() ? vi : 0; }

  // ---- programs ----
  uint32_t createProgram(const std::string& vs, const std::string& fs,
                         const std::vector<std::pair<int, const char*>>& attribs, std::string* error) override;
  void useProgram(uint32_t prog) override { program_ = prog < programs_.size() ? prog : 0; }
  uint32_t currentProgram() const override { return program_; }
  int uniformLocation(uint32_t prog, const char* name) override;
  void uniform1i(int loc, int v) override;
  void uniform1f(int loc, float v) override { setFloats(loc, &v, 1, 1, 0); }
  void uniform3f(int loc, float x, float y, float z) override {
    const float v[3] = {x, y, z};
    setFloats(loc, v, 3, 1, 0);
  }
  void uniform4f(int loc, float x, float y, float z, float w) override {
    const float v[4] = {x, y, z, w};
    setFloats(loc, v, 4, 1, 0);
  }
  void uniform3fv(int loc, int count, const float* v) override { setFloats(loc, v, 3, count, 0); }
  void uniform4fv(int loc, int count, const float* v) override { setFloats(loc, v, 4, count, 0); }
  void uniformMatrix3fv(int loc, int count, const float* m) override { setFloats(loc, m, 3, count, 3); }
  void uniformMatrix4fv(int loc, int count, const float* m) override { setFloats(loc, m, 4, count, 4); }

  // ---- fixed state ----
  void viewport(int x, int y, int w, int h) override { st_.vp = {x, y, w, h}; }
  void scissorTest(bool on) override { st_.scissorOn = on; }
  void scissor(int x, int y, int w, int h) override { st_.sc = {x, y, w, h}; }
  void clear(bool color, bool depth, float r, float g, float b, float a, float depthValue) override;
  void blend(bool on) override { st_.blendOn = on; }
  void blendFunc(Blend src, Blend dst) override { st_.src = src, st_.dst = dst; }
  void depthTest(bool on) override { st_.depthTest = on; }
  void depthMask(bool on) override { st_.depthWrite = on; }
  void depthFunc(Compare c) override { st_.depthFunc = c; }
  void cull(bool on) override { st_.cullOn = on; }
  void cullFace(Face f) override { st_.cullFace = f; }
  void polygonOffset(bool on, float factor, float units) override {
    st_.polyOn = on;
    if (on) st_.polyFactor = factor, st_.polyUnits = units;
  }
  void bindTexture(int unit, uint32_t tex, bool cube) override {
    if (unit >= 0 && unit < kUnits) units_[size_t(unit)] = {tex, cube};
  }

  // ---- drawing ----
  void drawArrays(Prim p, int first, int count) override { draw(p, false, count, size_t(first)); }
  void drawElements(Prim p, int count, size_t firstIndex) override { draw(p, true, count, firstIndex); }

  // ---- frame ----
  void beginFrame(int width, int height) override;
  void present() override;
  void setVsync(bool on) override {
    if (on != vsync_) vsync_ = on, swapchainDirty_ = true;
  }

 private:
  static constexpr int kUnits = 16;
  static constexpr int kFrames = 2;

  struct Tex {
    bool used = false, cube = false, renderbuffer = false;
    int levelsHint = 1;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    Allocation mem;
    Format format = Format::None;
    VkFormat vkFormat = VK_FORMAT_UNDEFINED;
    int w = 0, h = 0, levels = 1, layers = 1, samples = 1;
    SamplerDesc sampler;
    float aniso = 1;
    VkSampler vkSampler = VK_NULL_HANDLE;  // cached from sampler + aniso
  };
  struct Fb {
    bool used = false;
    uint32_t color = 0, depth = 0;  // texture table indices (renderbuffer bit cleared)
  };
  struct Buf {
    bool used = false, stream = false;
    VkBuffer buffer = VK_NULL_HANDLE;  // static buffers: their own; stream buffers: the ring's
    Allocation mem;
    size_t size = 0;
    VkDeviceSize offset = 0;  // stream: the current region in the ring
    uint8_t* ptr = nullptr;
    uint64_t frame = 0;
  };
  struct VertexInput {
    bool used = false;
    std::vector<VertexAttrib> attribs;
    uint32_t index = 0;
    int layout = 0;
  };
  struct Program {
    bool used = false, null = false;  // null: no shaders (bring-up without glslang), draws are skipped
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::vector<vk::UniformInfo> uniforms;
    std::vector<int> samplerUnit;  // per uniform (samplers): the unit set with uniform1i
    std::vector<uint8_t> block;
    int blockBinding = -1;
    std::vector<int> inputs;
    std::unordered_map<std::string, int> locations;
  };
  struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<std::function<void()>> garbage;
    // ring buffers (uniforms, stream vertices, staging): chunks reused when the frame comes round again
    struct Chunk {
      VkBuffer buffer = VK_NULL_HANDLE;
      Allocation mem;
      VkDeviceSize size = 0, used = 0;
    };
    std::vector<Chunk> ring;
    size_t ringCurrent = 0;
  };
  struct State {
    std::array<int, 4> vp{0, 0, 1, 1}, sc{0, 0, 1, 1};
    bool scissorOn = false, blendOn = false, depthTest = false, depthWrite = true, cullOn = false, polyOn = false;
    Blend src = Blend::One, dst = Blend::Zero;
    Compare depthFunc = Compare::Less;
    Face cullFace = Face::Back;
    float polyFactor = 0, polyUnits = 0;
  };
  struct PipeKey {
    uint32_t program;
    int layout;
    uint8_t prim, blendOn, src, dst;
    VkFormat color, depth;
    int samples;
    bool operator==(const PipeKey& o) const { return std::memcmp(this, &o, sizeof *this) == 0; }
  };
  struct PipeHash {
    size_t operator()(const PipeKey& k) const {
      size_t h = 1469598103934665603ull;
      const auto* p = reinterpret_cast<const uint8_t*>(&k);
      for (size_t i = 0; i < sizeof k; ++i) h = (h ^ p[i]) * 1099511628211ull;
      return h;
    }
  };

  static int bufferSlotOf(const VertexAttrib* attribs, int count, const VertexAttrib& a) {
    std::vector<uint32_t> seen;
    for (int k = 0; k < count; ++k)
      if (std::find(seen.begin(), seen.end(), attribs[k].buffer) == seen.end()) seen.push_back(attribs[k].buffer);
    return int(std::find(seen.begin(), seen.end(), a.buffer) - seen.begin());
  }

  // device setup
  bool createSwapchain(std::string* error);
  void destroySwapchain();
  bool makeBackBuffer(int w, int h);
  void shutdown();

  // textures
  uint32_t newTexture() {
    for (uint32_t i = 1; i < textures_.size(); ++i)
      if (!textures_[i].used) {
        textures_[i] = Tex{};
        textures_[i].used = true;
        return i;
      }
    textures_.emplace_back();
    textures_.back().used = true;
    return uint32_t(textures_.size() - 1);
  }
  Tex* texture(uint32_t h) {
    h &= ~kRenderbufferBit;
    return h && h < textures_.size() && textures_[h].used ? &textures_[h] : nullptr;
  }
  void releaseTexture(uint32_t h) {
    Tex* t = texture(h);
    if (!t) return;
    freeImage(*t);
    *t = Tex{};
  }
  bool makeImage(Tex& t, Format f, int w, int h, int levels, int samples);
  void freeImage(Tex& t) {
    if (!t.image) return;
    VkImage img = t.image;
    VkImageView view = t.view;
    Allocation a = t.mem;
    later([this, img, view, a]() mutable {
      vkDestroyImageView(dev_, view, nullptr);
      vkDestroyImage(dev_, img, nullptr);
      alloc_.free(a);
    });
    t.image = VK_NULL_HANDLE;
    t.view = VK_NULL_HANDLE;
  }
  VkFormat vkFormatOf(Format f) const;
  VkImageAspectFlags aspectOf(const Tex& t) const { return isDepth(t.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }
  VkSampler samplerFor(Tex& t);
  void upload(Tex& t, int face, int level, int x, int y, int w, int h, Pixels p, const void* data, size_t bytes);

  // frames, commands, rings
  VkCommandBuffer cmd() { return frames_[frame_].cmd; }
  void later(std::function<void()> f) { frames_[frame_].garbage.push_back(std::move(f)); }
  bool ringAlloc(VkDeviceSize bytes, VkDeviceSize align, VkBuffer& buf, VkDeviceSize& off, uint8_t*& ptr);
  void beginCommands();
  void submitAndWait();  // ends rendering, submits what was recorded, waits, and starts a new command buffer

  // rendering
  void ensureRendering();
  void endRendering();
  void fbExtent(uint32_t fb, int& w, int& h, VkFormat& color, VkFormat& depth, int& samples);
  VkPipeline pipelineFor(Prim p);
  void draw(Prim p, bool indexed, int count, size_t first);
  int locIndex(int loc) const { return loc >> 10; }
  int locElement(int loc) const { return loc & 1023; }
  void setFloats(int loc, const float* v, int comps, int count, int columns);

  platform::Window& window_;
  std::string info_;
  float maxAniso_ = 1;
  bool vsync_ = true, swapchainDirty_ = false;
  HMODULE lib_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice dev_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkDeviceSize uboAlign_ = 256;
  VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
  Allocator alloc_;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat swapFormat_ = VK_FORMAT_B8G8R8A8_UNORM;
  VkExtent2D swapExtent_{0, 0};
  std::vector<VkImage> swapImages_;
  std::vector<VkSemaphore> presentReady_;  // per swapchain image
  std::array<Frame, kFrames> frames_;
  size_t frame_ = 0;
  uint64_t frameCount_ = 1;
  bool recording_ = false;

  std::vector<Tex> textures_ = std::vector<Tex>(1);
  std::vector<Fb> framebuffers_ = std::vector<Fb>(1);  // 0: the back buffer
  std::vector<Buf> buffers_ = std::vector<Buf>(1);
  std::vector<VertexInput> vertexInputs_ = std::vector<VertexInput>(1);
  std::vector<Program> programs_ = std::vector<Program>(1);
  std::map<std::string, int> layoutIds_;
  std::unordered_map<PipeKey, VkPipeline, PipeHash> pipelines_;
  std::map<std::string, VkSampler> samplers_;
  uint32_t backColor_ = 0, backDepth_ = 0;
  int backW_ = 0, backH_ = 0;
  uint32_t dummy2D_ = 0, dummyCube_ = 0, dummyShadow_ = 0;
  VkBuffer zeroAttrib_ = VK_NULL_HANDLE;  // (0, 0, 0, 1) for vertex inputs a layout does not supply
  Allocation zeroAttribMem_;

  State st_;
  std::array<std::pair<uint32_t, bool>, kUnits> units_{};
  uint32_t program_ = 0, boundVi_ = 0, boundFb_ = 0;
  int renderingFb_ = -1;
  bool nullDrawLogged_ = false;
};

// ---------------------------------------------------------------------------------------------------------------
// setup

bool VkDeviceImpl::init(bool vsync, std::string* error) {
  vsync_ = vsync;
  lib_ = LoadLibraryW(L"vulkan-1.dll");
  if (!lib_) {
    if (error) *error = "vulkan-1.dll not found (no Vulkan driver)";
    return false;
  }
  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib_, "vkGetInstanceProcAddr"));
  if (!vkGetInstanceProcAddr) {
    if (error) *error = "vkGetInstanceProcAddr missing";
    return false;
  }
#define REGS_VK_LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
  REGS_VK_GLOBAL(REGS_VK_LOAD_GLOBAL)
#undef REGS_VK_LOAD_GLOBAL
  if (!vkCreateInstance) {
    if (error) *error = "vkCreateInstance missing";
    return false;
  }
  // instance: Vulkan 1.3, the Win32 surface; the Khronos validation layer when asked for and installed
  std::vector<const char*> layers, exts = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
  if (const char* v = std::getenv("REGS_VK_VALIDATION"); v && *v == '1') {
    uint32_t n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    std::vector<VkLayerProperties> lp(n);
    vkEnumerateInstanceLayerProperties(&n, lp.data());
    for (const auto& l : lp)
      if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) layers.push_back("VK_LAYER_KHRONOS_validation");
    if (layers.empty()) LOG_WARN("vulkan: REGS_VK_VALIDATION set, but the validation layer is not installed (Vulkan SDK)");
  }
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "OpenCiM";
  app.pEngineName = "ReGS";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  ici.enabledLayerCount = uint32_t(layers.size());
  ici.ppEnabledLayerNames = layers.data();
  ici.enabledExtensionCount = uint32_t(exts.size());
  ici.ppEnabledExtensionNames = exts.data();
  if (VkResult r = vkCreateInstance(&ici, nullptr, &instance_); r != VK_SUCCESS) {
    if (error) *error = vkError("vkCreateInstance", r);
    return false;
  }
#define REGS_VK_LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance_, #name));
  REGS_VK_INSTANCE(REGS_VK_LOAD_INSTANCE)
#undef REGS_VK_LOAD_INSTANCE
  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = static_cast<HINSTANCE>(window_.nativeInstance());
  sci.hwnd = static_cast<HWND>(window_.nativeWindow());
  if (VkResult r = vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_); r != VK_SUCCESS) {
    if (error) *error = vkError("vkCreateWin32SurfaceKHR", r);
    return false;
  }
  // physical device: Vulkan 1.3 with dynamic rendering and synchronization2, BC textures, a queue that draws and
  // presents, VK_KHR_swapchain and VK_KHR_push_descriptor; discrete GPUs first
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance_, &n, nullptr);
  std::vector<VkPhysicalDevice> pds(n);
  vkEnumeratePhysicalDevices(instance_, &n, pds.data());
  std::string why = "no GPU with Vulkan";
  int best = -1;
  for (VkPhysicalDevice pd : pds) {
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(pd, &p);
    if (p.apiVersion < VK_API_VERSION_1_3) {
      why = std::string(p.deviceName) + ": Vulkan 1.3 missing";
      continue;
    }
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    if (!f13.dynamicRendering || !f13.synchronization2 || !f2.features.textureCompressionBC) {
      why = std::string(p.deviceName) + ": dynamic rendering, synchronization2 or BC textures missing";
      continue;
    }
    uint32_t en = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, nullptr);
    std::vector<VkExtensionProperties> ep(en);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &en, ep.data());
    auto has = [&](const char* e) {
      return std::any_of(ep.begin(), ep.end(), [&](const VkExtensionProperties& x) { return std::strcmp(x.extensionName, e) == 0; });
    };
    if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
      why = std::string(p.deviceName) + ": VK_KHR_swapchain or VK_KHR_push_descriptor missing";
      continue;
    }
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qp(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qp.data());
    int family = -1;
    for (uint32_t i = 0; i < qn && family < 0; ++i) {
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface_, &present);
      if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) family = int(i);
    }
    if (family < 0) {
      why = std::string(p.deviceName) + ": no queue that draws and presents";
      continue;
    }
    int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
    if (score > best) {
      best = score;
      phys_ = pd;
      queueFamily_ = uint32_t(family);
    }
  }
  if (!phys_) {
    if (error) *error = why;
    return false;
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys_, &props);
  maxAniso_ = props.limits.maxSamplerAnisotropy;
  uboAlign_ = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
  info_ = "Vulkan " + std::to_string(VK_API_VERSION_MAJOR(props.apiVersion)) + "." +
          std::to_string(VK_API_VERSION_MINOR(props.apiVersion)) + "." + std::to_string(VK_API_VERSION_PATCH(props.apiVersion)) +
          " / " + props.deviceName;
  // depth: 24-bit like GL's DEPTH_COMPONENT24 where it can be an attachment and sampled, else 32-bit float
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_X8_D24_UNORM_PACK32, &fp);
  VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
  depthFormat_ = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT;

  float prio = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queueFamily_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13};
  f2.features.textureCompressionBC = VK_TRUE;
  f2.features.samplerAnisotropy = props.limits.maxSamplerAnisotropy > 1 ? VK_TRUE : VK_FALSE;
  const char* dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 2;
  dci.ppEnabledExtensionNames = dexts;
  if (VkResult r = vkCreateDevice(phys_, &dci, nullptr, &dev_); r != VK_SUCCESS) {
    if (error) *error = vkError("vkCreateDevice", r);
    return false;
  }
#define REGS_VK_LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev_, #name));
  REGS_VK_DEVICE(REGS_VK_LOAD_DEVICE)
#undef REGS_VK_LOAD_DEVICE
  vkGetDeviceQueue(dev_, queueFamily_, 0, &queue_);
  alloc_.init(phys_, dev_);

  for (Frame& f : frames_) {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = queueFamily_;
    vkCreateCommandPool(dev_, &pci, nullptr, &f.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, f.pool,
                                    VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev_, &cai, &f.cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
    vkCreateFence(dev_, &fci, nullptr, &f.fence);
    VkSemaphoreCreateInfo sci2{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(dev_, &sci2, nullptr, &f.acquired);
  }
  vkResetFences(dev_, 1, &frames_[0].fence);
  beginCommands();

  if (!createSwapchain(error)) return false;
  if (!makeBackBuffer(std::max(1, window_.width()), std::max(1, window_.height()))) {
    if (error) *error = "the back buffer could not be created";
    return false;
  }
  // placeholders for units a program samples but nothing is bound to (GL reads black-ish there; white keeps
  // multiplied maps neutral): a white 2D texture, a white cube, a depth-1 shadow map
  const uint8_t white[4 * 6] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
  dummy2D_ = createTexture(TexKind::Tex2D, 1);
  texImage(dummy2D_, 0, 0, Format::RGBA8, 1, 1, Pixels::BGRA8, white);
  dummyCube_ = createTexture(TexKind::Cube, 1);
  for (int f = 0; f < 6; ++f) texImage(dummyCube_, f, 0, Format::RGBA8, 1, 1, Pixels::BGRA8, white);
  dummyShadow_ = createTexture(TexKind::Tex2D, 1);
  texImage(dummyShadow_, 0, 0, Format::Depth24, 1, 1, Pixels::None, nullptr);
  {
    Tex& t = textures_[dummyShadow_];
    VkClearDepthStencilValue one{1.0f, 0};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cmd(), t.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
    fullBarrier(cmd());
    SamplerDesc s;
    s.compareLEqual = true;
    t.sampler = s;
  }
  {  // the constant vertex attribute (0, 0, 0, 1)
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = 16;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vkCreateBuffer(dev_, &bci, nullptr, &zeroAttrib_);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev_, zeroAttrib_, &req);
    alloc_.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, zeroAttribMem_);
    vkBindBufferMemory(dev_, zeroAttrib_, zeroAttribMem_.memory, zeroAttribMem_.offset);
    const float v[4] = {0, 0, 0, 1};
    std::memcpy(zeroAttribMem_.mapped, v, 16);
  }
  if (!vk::available()) LOG_WARN("vulkan: this build has no glslang, so no shader compiles: only clears are drawn");
  return true;
}

bool VkDeviceImpl::createSwapchain(std::string* error) {
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, formats.data());
  VkSurfaceFormatKHR fmt = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
  for (const auto& f : formats)  // UNORM like the GL window (no sRGB conversion anywhere)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      fmt = f;
      break;
    }
  vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
  std::vector<VkPresentModeKHR> modes(n);
  vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, modes.data());
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync_) {
    for (VkPresentModeKHR m : modes)
      if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    for (VkPresentModeKHR m : modes)
      if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
  }
  VkExtent2D ext = caps.currentExtent;
  if (ext.width == 0xFFFFFFFFu) ext = {uint32_t(std::max(1, window_.width())), uint32_t(std::max(1, window_.height()))};
  if (ext.width == 0 || ext.height == 0) return true;  // minimised: no swapchain until it has a size again
  uint32_t count = std::max(caps.minImageCount + 1, 2u);
  if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = count;
  sci.imageFormat = fmt.format;
  sci.imageColorSpace = fmt.colorSpace;
  sci.imageExtent = ext;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = swapchain_;
  VkSwapchainKHR sc = VK_NULL_HANDLE;
  if (VkResult r = vkCreateSwapchainKHR(dev_, &sci, nullptr, &sc); r != VK_SUCCESS) {
    if (error) *error = vkError("vkCreateSwapchainKHR", r);
    return false;
  }
  destroySwapchain();
  swapchain_ = sc;
  swapFormat_ = fmt.format;
  swapExtent_ = ext;
  vkGetSwapchainImagesKHR(dev_, swapchain_, &n, nullptr);
  swapImages_.resize(n);
  vkGetSwapchainImagesKHR(dev_, swapchain_, &n, swapImages_.data());
  for (uint32_t i = 0; i < n; ++i) {
    VkSemaphoreCreateInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore sem;
    vkCreateSemaphore(dev_, &s, nullptr, &sem);
    presentReady_.push_back(sem);
  }
  swapchainDirty_ = false;
  return true;
}

void VkDeviceImpl::destroySwapchain() {
  if (!swapchain_) return;
  vkDeviceWaitIdle(dev_);
  for (VkSemaphore s : presentReady_) vkDestroySemaphore(dev_, s, nullptr);
  presentReady_.clear();
  vkDestroySwapchainKHR(dev_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  swapImages_.clear();
}

bool VkDeviceImpl::makeBackBuffer(int w, int h) {
  if (backColor_ && backW_ == w && backH_ == h) return true;
  if (renderingFb_ == 0) endRendering();
  if (backColor_) releaseTexture(backColor_), releaseTexture(backDepth_);
  backColor_ = newTexture();
  backDepth_ = newTexture();
  if (!makeImage(textures_[backColor_], Format::RGBA8, w, h, 1, 1) ||
      !makeImage(textures_[backDepth_], Format::Depth24, w, h, 1, 1))
    return false;
  framebuffers_[0] = {true, backColor_, backDepth_};
  backW_ = w;
  backH_ = h;
  return true;
}

void VkDeviceImpl::shutdown() {
  if (!instance_) {
    if (lib_) FreeLibrary(lib_), lib_ = nullptr;
    return;
  }
  if (dev_) {
    vkDeviceWaitIdle(dev_);
    for (Frame& f : frames_) {
      for (auto& g : f.garbage) g();
      f.garbage.clear();
    }
    for (auto& [k, p] : pipelines_) vkDestroyPipeline(dev_, p, nullptr);
    for (auto& [k, s] : samplers_) vkDestroySampler(dev_, s, nullptr);
    for (Program& p : programs_) {
      if (p.vs) vkDestroyShaderModule(dev_, p.vs, nullptr);
      if (p.fs) vkDestroyShaderModule(dev_, p.fs, nullptr);
      if (p.layout) vkDestroyPipelineLayout(dev_, p.layout, nullptr);
      if (p.setLayout) vkDestroyDescriptorSetLayout(dev_, p.setLayout, nullptr);
    }
    for (Tex& t : textures_)
      if (t.image) vkDestroyImageView(dev_, t.view, nullptr), vkDestroyImage(dev_, t.image, nullptr);
    for (Buf& b : buffers_)
      if (b.buffer && !b.stream) vkDestroyBuffer(dev_, b.buffer, nullptr);
    if (zeroAttrib_) vkDestroyBuffer(dev_, zeroAttrib_, nullptr);
    for (Frame& f : frames_) {
      for (auto& c : f.ring) vkDestroyBuffer(dev_, c.buffer, nullptr);
      vkDestroyFence(dev_, f.fence, nullptr);
      vkDestroySemaphore(dev_, f.acquired, nullptr);
      vkDestroyCommandPool(dev_, f.pool, nullptr);
    }
    destroySwapchain();
    alloc_.shutdown();
    vkDestroyDevice(dev_, nullptr);
    dev_ = VK_NULL_HANDLE;
  }
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  vkDestroyInstance(instance_, nullptr);
  instance_ = VK_NULL_HANDLE;
  if (lib_) FreeLibrary(lib_), lib_ = nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// commands and frames

void VkDeviceImpl::beginCommands() {
  Frame& f = frames_[frame_];
  vkResetCommandPool(dev_, f.pool, 0);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(f.cmd, &bi);
  recording_ = true;
}

void VkDeviceImpl::submitAndWait() {
  endRendering();
  Frame& f = frames_[frame_];
  vkEndCommandBuffer(f.cmd);
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, f.cmd, 0};
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  vkQueueSubmit2(queue_, 1, &si, f.fence);
  vkWaitForFences(dev_, 1, &f.fence, VK_TRUE, UINT64_MAX);
  vkResetFences(dev_, 1, &f.fence);
  // everything recorded so far has run: the ring can start over (stream buffers written this frame keep their data:
  // their regions are not handed out again before the next submit, which only happens after they were consumed)
  for (auto& g : f.garbage) g();
  f.garbage.clear();
  beginCommands();
}

bool VkDeviceImpl::ringAlloc(VkDeviceSize bytes, VkDeviceSize align, VkBuffer& buf, VkDeviceSize& off, uint8_t*& ptr) {
  Frame& f = frames_[frame_];
  for (; f.ringCurrent < f.ring.size(); ++f.ringCurrent) {
    Frame::Chunk& c = f.ring[f.ringCurrent];
    VkDeviceSize start = (c.used + align - 1) / align * align;
    if (start + bytes <= c.size) {
      c.used = start + bytes;
      buf = c.buffer;
      off = start;
      ptr = c.mem.mapped + start;
      return true;
    }
  }
  Frame::Chunk c;
  c.size = std::max<VkDeviceSize>(16ull << 20, bytes);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = c.size;
  bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(dev_, &bci, nullptr, &c.buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev_, c.buffer, &req);
  if (!alloc_.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, c.mem)) {
    vkDestroyBuffer(dev_, c.buffer, nullptr);
    return false;
  }
  vkBindBufferMemory(dev_, c.buffer, c.mem.memory, c.mem.offset);
  c.used = bytes;
  buf = c.buffer;
  off = 0;
  ptr = c.mem.mapped;
  f.ring.push_back(c);
  f.ringCurrent = f.ring.size() - 1;
  return true;
}

void VkDeviceImpl::beginFrame(int width, int height) {
  makeBackBuffer(std::max(1, width), std::max(1, height));
  if (swapchainDirty_ || int(swapExtent_.width) != window_.width() || int(swapExtent_.height) != window_.height()) {
    std::string err;
    if (!createSwapchain(&err)) LOG_ERROR("vulkan: %s", err.c_str());
  }
}

void VkDeviceImpl::present() {
  endRendering();
  Frame& f = frames_[frame_];
  uint32_t image = 0;
  bool presentable = swapchain_ != VK_NULL_HANDLE;
  if (presentable) {
    VkResult r = vkAcquireNextImageKHR(dev_, swapchain_, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) presentable = false, swapchainDirty_ = true;
    else if (r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
    else if (r != VK_SUCCESS) presentable = false;
  }
  if (presentable) {
    // the back buffer (GL rows: row 0 at the bottom) onto the swapchain image (row 0 at the top): mirrored in y
    VkImage dst = swapImages_[image];
    imageBarrier(cmd(), dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[0] = {0, 0, 0};
    b.srcOffsets[1] = {backW_, backH_, 1};
    b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstOffsets[0] = {0, int32_t(swapExtent_.height), 0};
    b.dstOffsets[1] = {int32_t(swapExtent_.width), 0, 1};
    vkCmdBlitImage(cmd(), textures_[backColor_].image, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &b, VK_FILTER_NEAREST);
    imageBarrier(cmd(), dst, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  }
  vkEndCommandBuffer(f.cmd);
  VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, nullptr, f.cmd, 0};
  VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr, f.acquired, 0,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
  VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, nullptr,
                               presentable ? presentReady_[image] : VK_NULL_HANDLE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
  VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  si.waitSemaphoreInfoCount = presentable ? 1 : 0;
  si.pWaitSemaphoreInfos = &wait;
  si.signalSemaphoreInfoCount = presentable ? 1 : 0;
  si.pSignalSemaphoreInfos = &signal;
  si.commandBufferInfoCount = 1;
  si.pCommandBufferInfos = &cbi;
  vkQueueSubmit2(queue_, 1, &si, f.fence);
  if (presentable) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &presentReady_[image];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &image;
    VkResult r = vkQueuePresentKHR(queue_, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) swapchainDirty_ = true;
  }
  // the next frame: wait until its previous use has finished, then reuse its pool, ring and garbage list
  frame_ = (frame_ + 1) % kFrames;
  ++frameCount_;
  Frame& n = frames_[frame_];
  vkWaitForFences(dev_, 1, &n.fence, VK_TRUE, UINT64_MAX);
  vkResetFences(dev_, 1, &n.fence);
  for (auto& g : n.garbage) g();
  n.garbage.clear();
  for (auto& c : n.ring) c.used = 0;
  n.ringCurrent = 0;
  beginCommands();
}

// ---------------------------------------------------------------------------------------------------------------
// textures

VkFormat VkDeviceImpl::vkFormatOf(Format f) const {
  switch (f) {
    case Format::RGBA8:
    case Format::RGB8: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::DXT1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::DXT3: return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::DXT5: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::RGBA16F: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::RGBA32F: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::R8: return VK_FORMAT_R8_UNORM;
    case Format::RG8: return VK_FORMAT_R8G8_UNORM;
    case Format::RG16: return VK_FORMAT_R16G16_UNORM;
    case Format::Depth24: return depthFormat_;
    case Format::None: break;
  }
  return VK_FORMAT_B8G8R8A8_UNORM;
}

bool VkDeviceImpl::makeImage(Tex& t, Format f, int w, int h, int levels, int samples) {
  freeImage(t);
  t.format = f;
  t.vkFormat = vkFormatOf(f);
  t.w = w;
  t.h = h;
  t.levels = levels;
  t.layers = t.cube ? 6 : 1;
  t.samples = samples;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.flags = t.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = t.vkFormat;
  ici.extent = {uint32_t(w), uint32_t(h), 1};
  ici.mipLevels = uint32_t(levels);
  ici.arrayLayers = uint32_t(t.layers);
  ici.samples = VkSampleCountFlagBits(samples);
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if (samples == 1) ici.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
  if (isDepth(f)) ici.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  else if (!isCompressed(f) && !t.cube) ici.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (VkResult r = vkCreateImage(dev_, &ici, nullptr, &t.image); r != VK_SUCCESS) {
    LOG_ERROR("vulkan: %s for a %dx%d image", vkError("vkCreateImage", r).c_str(), w, h);
    t.image = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(dev_, t.image, &req);
  if (!alloc_.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, t.mem)) {
    LOG_ERROR("vulkan: out of video memory for a %dx%d image", w, h);
    vkDestroyImage(dev_, t.image, nullptr);
    t.image = VK_NULL_HANDLE;
    return false;
  }
  vkBindImageMemory(dev_, t.image, t.mem.memory, t.mem.offset);
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.image = t.image;
  vci.viewType = t.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  vci.format = t.vkFormat;
  if (f == Format::RGB8) vci.components.a = VK_COMPONENT_SWIZZLE_ONE;  // GL_RGB8 reads alpha 1
  vci.subresourceRange = {aspectOf(t), 0, uint32_t(levels), 0, uint32_t(t.layers)};
  vkCreateImageView(dev_, &vci, nullptr, &t.view);
  // every image lives in GENERAL (see the top of this file)
  endRendering();
  imageBarrier(cmd(), t.image, aspectOf(t), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
  return true;
}

void VkDeviceImpl::texImage(uint32_t tex, int face, int level, Format f, int w, int h, Pixels p, const void* data,
                            size_t bytes) {
  Tex* t = texture(tex);
  if (!t) return;
  if (level == 0 && (!t->image || t->format != f || t->w != w || t->h != h)) {
    if (!makeImage(*t, f, w, h, std::max(1, t->levelsHint), 1)) return;
  }
  if (!t->image || level >= t->levels) return;
  if (data) upload(*t, face, level, 0, 0, w, h, p, data, bytes);
}

void VkDeviceImpl::texSubImage(uint32_t tex, int level, int x, int y, int w, int h, Pixels p, const void* data) {
  Tex* t = texture(tex);
  if (!t || !t->image || !data || w <= 0 || h <= 0) return;
  upload(*t, 0, level, x, y, w, h, p, data, 0);
}

void VkDeviceImpl::upload(Tex& t, int face, int level, int x, int y, int w, int h, Pixels p, const void* data, size_t bytes) {
  // the texels as the image stores them
  size_t texels = size_t(w) * size_t(h);
  size_t size = bytes;
  std::vector<uint8_t> converted;
  const void* src = data;
  if (p == Pixels::Compressed) {
    size = bytes;
  } else if (t.format == Format::RGBA16F && p == Pixels::BGRA8) {  // night lights: bytes into the half-float light map
    converted.resize(texels * 8);
    const auto* in = static_cast<const uint8_t*>(data);
    auto* out = reinterpret_cast<uint16_t*>(converted.data());
    for (size_t i = 0; i < texels; ++i) {
      out[i * 4 + 0] = floatToHalf(in[i * 4 + 2] / 255.0f);
      out[i * 4 + 1] = floatToHalf(in[i * 4 + 1] / 255.0f);
      out[i * 4 + 2] = floatToHalf(in[i * 4 + 0] / 255.0f);
      out[i * 4 + 3] = floatToHalf(in[i * 4 + 3] / 255.0f);
    }
    src = converted.data();
    size = converted.size();
  } else if (p == Pixels::BGR8) {  // BGR → BGRA
    converted.resize(texels * 4);
    const auto* in = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < texels; ++i) {
      converted[i * 4] = in[i * 3];
      converted[i * 4 + 1] = in[i * 3 + 1];
      converted[i * 4 + 2] = in[i * 3 + 2];
      converted[i * 4 + 3] = 255;
    }
    src = converted.data();
    size = converted.size();
  } else {
    size_t bpp = p == Pixels::BGRA8 ? 4 : p == Pixels::RGBA32F ? 16 : p == Pixels::R8 ? 1 : p == Pixels::RG8 ? 2 : 4;
    size = texels * bpp;
  }
  if (!size) return;
  VkBuffer buf;
  VkDeviceSize off;
  uint8_t* ptr;
  if (size > (64ull << 20)) submitAndWait();  // very large uploads start a fresh ring
  if (!ringAlloc(size, 16, buf, off, ptr)) return;
  std::memcpy(ptr, src, size);
  endRendering();
  VkBufferImageCopy c{};
  c.bufferOffset = off;
  c.imageSubresource = {aspectOf(t), uint32_t(level), uint32_t(face), 1};
  c.imageOffset = {x, y, 0};
  c.imageExtent = {uint32_t(w), uint32_t(h), 1};
  vkCmdCopyBufferToImage(cmd(), buf, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
  fullBarrier(cmd());
  // loading a city uploads gigabytes between two frames: flush now and then so the ring does not grow without end
  size_t ringBytes = 0;
  for (const auto& ch : frames_[frame_].ring) ringBytes += ch.used;
  if (ringBytes > (256ull << 20)) submitAndWait();
}

VkSampler VkDeviceImpl::samplerFor(Tex& t) {
  if (t.vkSampler) return t.vkSampler;
  const SamplerDesc& s = t.sampler;
  float aniso = std::min(t.aniso, maxAniso_);
  std::string key = std::to_string(int(s.minFilter)) + "," + std::to_string(int(s.magFilter)) + "," + std::to_string(int(s.wrapS)) +
                    "," + std::to_string(int(s.wrapT)) + "," + std::to_string(int(s.wrapR)) + "," + std::to_string(s.maxLevel) + "," +
                    std::to_string(s.borderWhite) + "," + std::to_string(s.compareLEqual) + "," + std::to_string(aniso);
  auto it = samplers_.find(key);
  if (it != samplers_.end()) return t.vkSampler = it->second;
  VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sci.magFilter = s.magFilter == Filter::Nearest || s.magFilter == Filter::NearestMipNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  sci.minFilter = s.minFilter == Filter::Nearest || s.minFilter == Filter::NearestMipNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  bool mip = s.minFilter == Filter::LinearMipLinear || s.minFilter == Filter::NearestMipNearest;
  sci.mipmapMode = s.minFilter == Filter::LinearMipLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sci.addressModeU = vkWrap(s.wrapS);
  sci.addressModeV = vkWrap(s.wrapT);
  sci.addressModeW = vkWrap(s.wrapR);
  sci.anisotropyEnable = aniso > 1 ? VK_TRUE : VK_FALSE;
  sci.maxAnisotropy = std::max(1.0f, aniso);
  sci.compareEnable = s.compareLEqual ? VK_TRUE : VK_FALSE;
  sci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
  sci.minLod = 0;
  // GL's non-mipmap filters read only the base level: the usual 0.25 clamp (Vulkan spec, "Texel filtering")
  sci.maxLod = !mip ? 0.25f : s.maxLevel >= 0 ? float(s.maxLevel) : VK_LOD_CLAMP_NONE;
  sci.borderColor = s.borderWhite ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  VkSampler smp = VK_NULL_HANDLE;
  vkCreateSampler(dev_, &sci, nullptr, &smp);
  samplers_[key] = smp;
  return t.vkSampler = smp;
}

void VkDeviceImpl::readTextureRed(uint32_t tex, int w, int h, float* out) {
  Tex* t = texture(tex);
  if (!t || !t->image) return;
  size_t bpp = t->format == Format::RGBA16F ? 8 : t->format == Format::RGBA32F ? 16 : t->format == Format::R8 ? 1 : 4;
  VkBuffer buf;
  VkDeviceSize off;
  uint8_t* ptr;
  w = std::min(w, t->w), h = std::min(h, t->h);
  if (!ringAlloc(size_t(w) * size_t(h) * bpp, 16, buf, off, ptr)) return;
  endRendering();
  VkBufferImageCopy c{};
  c.bufferOffset = off;
  c.imageSubresource = {aspectOf(*t), 0, 0, 1};
  c.imageExtent = {uint32_t(w), uint32_t(h), 1};
  vkCmdCopyImageToBuffer(cmd(), t->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
  submitAndWait();
  for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
    switch (t->format) {
      case Format::RGBA16F: out[i] = halfToFloat(reinterpret_cast<const uint16_t*>(ptr)[i * 4]); break;
      case Format::RGBA32F: out[i] = reinterpret_cast<const float*>(ptr)[i * 4]; break;
      case Format::R8: out[i] = ptr[i] / 255.0f; break;
      default: out[i] = ptr[i * 4 + 2] / 255.0f; break;  // B8G8R8A8: red is the third byte
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// buffers

void VkDeviceImpl::bufferData(uint32_t buf, size_t bytes, const void* data, bool stream) {
  if (!buf || buf >= buffers_.size() || !buffers_[buf].used) return;
  Buf& b = buffers_[buf];
  b.size = bytes;
  if (stream) {  // GL's orphaning: fresh space in this frame's ring; draws recorded before keep the old region
    if (b.buffer && !b.stream) destroyBuffer(buf), buffers_[buf].used = true;
    Buf& nb = buffers_[buf];
    nb.stream = true;
    nb.size = bytes;
    if (!ringAlloc(std::max<size_t>(bytes, 4), 256, nb.buffer, nb.offset, nb.ptr)) return;
    nb.frame = frameCount_;
    if (data) std::memcpy(nb.ptr, data, bytes);
    return;
  }
  // static: device-local memory, filled through the ring
  if (b.buffer && !b.stream) {
    VkBuffer vb = b.buffer;
    Allocation a = b.mem;
    later([this, vb, a]() mutable {
      vkDestroyBuffer(dev_, vb, nullptr);
      alloc_.free(a);
    });
  }
  b.stream = false;
  b.buffer = VK_NULL_HANDLE;
  b.offset = 0;
  if (!bytes) return;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(dev_, &bci, nullptr, &b.buffer) != VK_SUCCESS) return;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(dev_, b.buffer, &req);
  if (!alloc_.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true, b.mem)) {
    LOG_ERROR("vulkan: out of video memory for a %zu-byte buffer", bytes);
    vkDestroyBuffer(dev_, b.buffer, nullptr);
    b.buffer = VK_NULL_HANDLE;
    return;
  }
  vkBindBufferMemory(dev_, b.buffer, b.mem.memory, b.mem.offset);
  if (data) bufferSubData(buf, 0, bytes, data);
}

void VkDeviceImpl::bufferSubData(uint32_t buf, size_t offset, size_t bytes, const void* data) {
  if (!buf || buf >= buffers_.size() || !buffers_[buf].buffer || !bytes) return;
  Buf& b = buffers_[buf];
  if (offset + bytes > b.size) return;
  if (b.stream) {
    if (b.frame != frameCount_) {  // a region from an earlier frame may be reused already: take a new one
      if (!ringAlloc(std::max<size_t>(b.size, 4), 256, b.buffer, b.offset, b.ptr)) return;
      b.frame = frameCount_;
    }
    std::memcpy(b.ptr + offset, data, bytes);
    return;
  }
  VkBuffer src;
  VkDeviceSize off;
  uint8_t* ptr;
  if (!ringAlloc(bytes, 16, src, off, ptr)) return;
  std::memcpy(ptr, data, bytes);
  endRendering();
  VkBufferCopy c{off, VkDeviceSize(offset), bytes};
  vkCmdCopyBuffer(cmd(), src, b.buffer, 1, &c);
  fullBarrier(cmd());
}

// ---------------------------------------------------------------------------------------------------------------
// programs and uniforms

uint32_t VkDeviceImpl::createProgram(const std::string& vs, const std::string& fs,
                                     const std::vector<std::pair<int, const char*>>& attribs, std::string* error) {
  Program p;
  p.used = true;
  vk::ProgramSpirv sp;
  if (!vk::compileProgram(vs, fs, attribs, sp, error)) {
    if (vk::available()) return 0;
    p.null = true;  // bring-up without glslang: a program that draws nothing
  } else {
    auto module = [&](const std::vector<uint32_t>& code) {
      VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      mci.codeSize = code.size() * 4;
      mci.pCode = code.data();
      VkShaderModule m = VK_NULL_HANDLE;
      vkCreateShaderModule(dev_, &mci, nullptr, &m);
      return m;
    };
    p.vs = module(sp.vs);
    p.fs = module(sp.fs);
    p.uniforms = std::move(sp.uniforms);
    p.samplerUnit.assign(p.uniforms.size(), 0);
    p.block.assign(size_t(std::max(sp.blockSize, 0)), 0);
    p.blockBinding = sp.blockBinding;
    p.inputs = std::move(sp.inputs);
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    if (p.blockBinding >= 0)
      bindings.push_back({uint32_t(p.blockBinding), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (const vk::UniformInfo& u : p.uniforms)
      if (u.sampler)
        bindings.push_back({uint32_t(u.binding), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    lci.bindingCount = uint32_t(bindings.size());
    lci.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(dev_, &lci, nullptr, &p.setLayout);
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &p.setLayout;
    vkCreatePipelineLayout(dev_, &plci, nullptr, &p.layout);
    for (size_t i = 0; i < p.uniforms.size(); ++i) p.locations[p.uniforms[i].name] = int(i);
  }
  for (uint32_t i = 1; i < programs_.size(); ++i)
    if (!programs_[i].used) {
      programs_[i] = std::move(p);
      return i;
    }
  programs_.push_back(std::move(p));
  return uint32_t(programs_.size() - 1);
}

int VkDeviceImpl::uniformLocation(uint32_t prog, const char* name) {
  if (!prog || prog >= programs_.size() || !programs_[prog].used) return -1;
  Program& p = programs_[prog];
  if (p.null) return 0;
  std::string n(name);
  auto it = p.locations.find(n);
  if (it != p.locations.end()) return it->second << 10;
  // "array[i]" of a uniform reflected as "array" or "array[0]"
  if (size_t lb = n.rfind('['); lb != std::string::npos && n.back() == ']') {
    int element = std::atoi(n.c_str() + lb + 1);
    std::string base = n.substr(0, lb);
    for (const std::string& key : {base, base + "[0]"}) {
      auto b = p.locations.find(key);
      if (b != p.locations.end() && element < p.uniforms[size_t(b->second)].arraySize) return b->second << 10 | element;
    }
  } else if (auto b = p.locations.find(n + "[0]"); b != p.locations.end()) {
    return b->second << 10;
  }
  return -1;
}

void VkDeviceImpl::uniform1i(int loc, int v) {
  if (loc < 0 || !program_) return;
  Program& p = programs_[program_];
  size_t i = size_t(locIndex(loc));
  if (p.null || i >= p.uniforms.size()) return;
  const vk::UniformInfo& u = p.uniforms[i];
  if (u.sampler) {
    p.samplerUnit[i] = v;
    return;
  }
  size_t off = size_t(u.offset + locElement(loc) * u.arrayStride);
  if (off + 4 > p.block.size()) return;
  if (u.isInt) std::memcpy(&p.block[off], &v, 4);
  else {
    float f = float(v);
    std::memcpy(&p.block[off], &f, 4);
  }
}

void VkDeviceImpl::setFloats(int loc, const float* v, int comps, int count, int columns) {
  if (loc < 0 || !program_) return;
  Program& p = programs_[program_];
  size_t i = size_t(locIndex(loc));
  if (p.null || i >= p.uniforms.size()) return;
  const vk::UniformInfo& u = p.uniforms[i];
  if (u.sampler) return;
  int first = locElement(loc);
  count = std::min(count, u.arraySize - first);
  for (int e = 0; e < count; ++e) {
    size_t base = size_t(u.offset + (first + e) * u.arrayStride);
    if (columns) {  // a matrix: column c at base + c · matrixStride, comps rows each
      for (int c = 0; c < columns && c < std::max(u.columns, 1); ++c) {
        size_t o = base + size_t(c * u.matrixStride);
        if (o + size_t(comps) * 4 > p.block.size()) return;
        std::memcpy(&p.block[o], v + (e * columns + c) * comps, size_t(comps) * 4);
      }
    } else {
      int n = std::min(comps, u.components);
      if (base + size_t(n) * 4 > p.block.size()) return;
      if (u.isInt) {
        for (int k = 0; k < n; ++k) {
          int iv = int(v[e * comps + k]);
          std::memcpy(&p.block[base + size_t(k) * 4], &iv, 4);
        }
      } else {
        std::memcpy(&p.block[base], v + e * comps, size_t(n) * 4);
      }
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// rendering

void VkDeviceImpl::fbExtent(uint32_t fb, int& w, int& h, VkFormat& color, VkFormat& depth, int& samples) {
  const Fb& f = framebuffers_[fb];
  const Tex* c = f.color ? &textures_[f.color] : nullptr;
  const Tex* d = f.depth ? &textures_[f.depth] : nullptr;
  const Tex* any = c ? c : d;
  w = any ? any->w : 1;
  h = any ? any->h : 1;
  samples = any ? any->samples : 1;
  color = c ? c->vkFormat : VK_FORMAT_UNDEFINED;
  depth = d ? d->vkFormat : VK_FORMAT_UNDEFINED;
}

void VkDeviceImpl::ensureRendering() {
  if (renderingFb_ == int(boundFb_)) return;
  endRendering();
  const Fb& f = framebuffers_[boundFb_];
  int w, h, samples;
  VkFormat cf, df;
  fbExtent(boundFb_, w, h, cf, df, samples);
  VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {uint32_t(w), uint32_t(h)}};
  ri.layerCount = 1;
  if (f.color) {
    ca.imageView = textures_[f.color].view;
    ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
  }
  if (f.depth) {
    da.imageView = textures_[f.depth].view;
    da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.pDepthAttachment = &da;
  }
  vkCmdBeginRendering(cmd(), &ri);
  renderingFb_ = int(boundFb_);
}

void VkDeviceImpl::endRendering() {
  if (renderingFb_ < 0) return;
  vkCmdEndRendering(cmd());
  fullBarrier(cmd());
  renderingFb_ = -1;
}

void VkDeviceImpl::clear(bool color, bool depth, float r, float g, float b, float a, float depthValue) {
  const Fb& f = framebuffers_[boundFb_];
  color = color && f.color;
  depth = depth && f.depth && st_.depthWrite;  // GL's depth clear obeys the depth mask
  if (!color && !depth) return;
  ensureRendering();
  int w, h, samples;
  VkFormat cf, df;
  fbExtent(boundFb_, w, h, cf, df, samples);
  int x0 = 0, y0 = 0, x1 = w, y1 = h;
  if (st_.scissorOn) {
    x0 = std::clamp(st_.sc[0], 0, w), y0 = std::clamp(st_.sc[1], 0, h);
    x1 = std::clamp(st_.sc[0] + st_.sc[2], 0, w), y1 = std::clamp(st_.sc[1] + st_.sc[3], 0, h);
  }
  if (x1 <= x0 || y1 <= y0) return;
  VkClearAttachment att[2];
  uint32_t n = 0;
  if (color) {
    att[n] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
    att[n].clearValue.color = {{r, g, b, a}};
    ++n;
  }
  if (depth) {
    att[n] = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, {}};
    att[n].clearValue.depthStencil = {depthValue, 0};
    ++n;
  }
  VkClearRect rect{{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}}, 0, 1};
  vkCmdClearAttachments(cmd(), n, att, 1, &rect);
}

VkPipeline VkDeviceImpl::pipelineFor(Prim prim) {
  const Program& p = programs_[program_];
  const VertexInput& vi = vertexInputs_[boundVi_];
  int w, h, samples;
  PipeKey key{};
  fbExtent(boundFb_, w, h, key.color, key.depth, samples);
  key.program = program_;
  key.layout = vi.layout;
  key.prim = uint8_t(prim);
  key.blendOn = st_.blendOn;
  key.src = st_.blendOn ? uint8_t(st_.src) : 0;
  key.dst = st_.blendOn ? uint8_t(st_.dst) : 0;
  key.samples = samples;
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;

  // vertex input: one binding per buffer of the layout; inputs the program reads but the layout lacks come from the
  // constant (0, 0, 0, 1) binding with stride 0 (GL's default attribute value)
  std::vector<VkVertexInputBindingDescription> binds;
  std::vector<VkVertexInputAttributeDescription> attrs;
  std::vector<uint32_t> bufs;
  for (const VertexAttrib& a : vi.attribs) {
    if (std::find(p.inputs.begin(), p.inputs.end(), a.slot) == p.inputs.end()) continue;
    auto bi = std::find(bufs.begin(), bufs.end(), a.buffer);
    uint32_t binding = uint32_t(bi - bufs.begin());
    if (bi == bufs.end()) {
      bufs.push_back(a.buffer);
      binds.push_back({binding, uint32_t(a.stride), VK_VERTEX_INPUT_RATE_VERTEX});
    }
    VkFormat f = a.type == AttribType::UByteNorm ? VK_FORMAT_R8G8B8A8_UNORM
                 : a.components == 1 ? VK_FORMAT_R32_SFLOAT
                 : a.components == 2 ? VK_FORMAT_R32G32_SFLOAT
                 : a.components == 3 ? VK_FORMAT_R32G32B32_SFLOAT
                                     : VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs.push_back({uint32_t(a.slot), binding, f, uint32_t(a.offset)});
  }
  for (int loc : p.inputs) {
    if (std::any_of(attrs.begin(), attrs.end(), [&](const auto& a) { return int(a.location) == loc; })) continue;
    uint32_t binding = uint32_t(binds.size());
    if (binds.empty() || binds.back().stride != 0) binds.push_back({binding, 0, VK_VERTEX_INPUT_RATE_VERTEX});
    else binding = binds.back().binding;
    attrs.push_back({uint32_t(loc), binding, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
  }
  VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vis.vertexBindingDescriptionCount = uint32_t(binds.size());
  vis.pVertexBindingDescriptions = binds.data();
  vis.vertexAttributeDescriptionCount = uint32_t(attrs.size());
  vis.pVertexAttributeDescriptions = attrs.data();
  VkPipelineInputAssemblyStateCreateInfo ias{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ias.topology = prim == Prim::Lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vps.viewportCount = 1;
  vps.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VkSampleCountFlagBits(samples);
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState cba{};
  cba.blendEnable = key.blendOn;
  cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = vkBlend(st_.src);
  cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = vkBlend(st_.dst);
  cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
  cba.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cbs.attachmentCount = key.color != VK_FORMAT_UNDEFINED ? 1 : 0;
  cbs.pAttachments = &cba;
  const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                                VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dys{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dys.dynamicStateCount = uint32_t(std::size(dyn));
  dys.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  prci.colorAttachmentCount = cbs.attachmentCount;
  prci.pColorAttachmentFormats = &key.color;
  prci.depthAttachmentFormat = key.depth;
  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, p.vs, "main", nullptr},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, p.fs, "main", nullptr}};
  VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prci};
  gci.stageCount = 2;
  gci.pStages = stages;
  gci.pVertexInputState = &vis;
  gci.pInputAssemblyState = &ias;
  gci.pViewportState = &vps;
  gci.pRasterizationState = &rs;
  gci.pMultisampleState = &ms;
  gci.pDepthStencilState = &ds;
  gci.pColorBlendState = &cbs;
  gci.pDynamicState = &dys;
  gci.layout = p.layout;
  VkPipeline pipe = VK_NULL_HANDLE;
  if (VkResult r = vkCreateGraphicsPipelines(dev_, VK_NULL_HANDLE, 1, &gci, nullptr, &pipe); r != VK_SUCCESS)
    LOG_ERROR("vulkan: %s", vkError("vkCreateGraphicsPipelines", r).c_str());
  pipelines_[key] = pipe;
  return pipe;
}

void VkDeviceImpl::draw(Prim prim, bool indexed, int count, size_t first) {
  if (count <= 0 || !program_ || !boundVi_) return;
  Program& p = programs_[program_];
  if (p.null) {
    if (!nullDrawLogged_) LOG_WARN("vulkan: draws skipped (no shaders in this build)");
    nullDrawLogged_ = true;
    return;
  }
  const VertexInput& vi = vertexInputs_[boundVi_];
  VkPipeline pipe = pipelineFor(prim);
  if (!pipe) return;
  ensureRendering();
  VkCommandBuffer c = cmd();
  vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  int fw, fh, samples;
  VkFormat cf, df;
  fbExtent(boundFb_, fw, fh, cf, df, samples);
  // GL's viewport and scissor, same numbers (rows are stored as GL stores them)
  VkViewport vp{float(st_.vp[0]), float(st_.vp[1]), float(std::max(1, st_.vp[2])), float(std::max(1, st_.vp[3])), 0, 1};
  vkCmdSetViewport(c, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {uint32_t(fw), uint32_t(fh)}};
  if (st_.scissorOn) {
    int x0 = std::clamp(st_.sc[0], 0, fw), y0 = std::clamp(st_.sc[1], 0, fh);
    int x1 = std::clamp(st_.sc[0] + st_.sc[2], 0, fw), y1 = std::clamp(st_.sc[1] + st_.sc[3], 0, fh);
    if (x1 <= x0 || y1 <= y0) return;
    sc = {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
  }
  vkCmdSetScissor(c, 0, 1, &sc);
  vkCmdSetCullMode(c, !st_.cullOn ? VK_CULL_MODE_NONE : st_.cullFace == Face::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT);
  vkCmdSetFrontFace(c, VK_FRONT_FACE_CLOCKWISE);  // GL's counter-clockwise in GL row order (top of this file)
  bool hasDepth = df != VK_FORMAT_UNDEFINED;
  vkCmdSetDepthTestEnable(c, hasDepth && st_.depthTest);
  vkCmdSetDepthWriteEnable(c, hasDepth && st_.depthTest && st_.depthWrite);
  vkCmdSetDepthCompareOp(c, st_.depthFunc == Compare::Less ? VK_COMPARE_OP_LESS : VK_COMPARE_OP_LESS_OR_EQUAL);
  vkCmdSetDepthBiasEnable(c, st_.polyOn);
  vkCmdSetDepthBias(c, st_.polyOn ? st_.polyUnits : 0, 0, st_.polyOn ? st_.polyFactor : 0);
  // vertex buffers in the pipeline's binding order
  std::vector<VkBuffer> vbs;
  std::vector<VkDeviceSize> offs;
  std::vector<uint32_t> seen;
  for (const VertexAttrib& a : vi.attribs) {
    if (std::find(p.inputs.begin(), p.inputs.end(), a.slot) == p.inputs.end()) continue;
    if (std::find(seen.begin(), seen.end(), a.buffer) != seen.end()) continue;
    seen.push_back(a.buffer);
    const Buf* b = a.buffer < buffers_.size() ? &buffers_[a.buffer] : nullptr;
    if (!b || !b->buffer) return;
    vbs.push_back(b->buffer);
    offs.push_back(b->offset);
  }
  size_t supplied = seen.size();
  for (int loc : p.inputs) {
    bool have = false;
    for (const VertexAttrib& a : vi.attribs) have |= a.slot == loc;
    if (!have) {
      if (vbs.size() == supplied) vbs.push_back(zeroAttrib_), offs.push_back(0);
    }
  }
  if (!vbs.empty()) vkCmdBindVertexBuffers(c, 0, uint32_t(vbs.size()), vbs.data(), offs.data());
  // descriptors: the uniform block (copied into the ring) and the images of the units the samplers point at
  std::vector<VkWriteDescriptorSet> writes;
  std::vector<VkDescriptorImageInfo> images;
  images.reserve(p.uniforms.size());
  VkDescriptorBufferInfo ubo{};
  if (p.blockBinding >= 0 && !p.block.empty()) {
    VkBuffer ub;
    VkDeviceSize uo;
    uint8_t* uptr;
    if (!ringAlloc(p.block.size(), uboAlign_, ub, uo, uptr)) return;
    std::memcpy(uptr, p.block.data(), p.block.size());
    ubo = {ub, uo, p.block.size()};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = uint32_t(p.blockBinding);
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &ubo;
    writes.push_back(w);
  }
  for (size_t i = 0; i < p.uniforms.size(); ++i) {
    const vk::UniformInfo& u = p.uniforms[i];
    if (!u.sampler) continue;
    int unit = p.samplerUnit[i];
    uint32_t th = unit >= 0 && unit < kUnits ? units_[size_t(unit)].first : 0;
    Tex* t = texture(th);
    bool fits = t && t->image && t->cube == u.cube && isDepth(t->format) == u.shadow && t->samples == 1;
    if (!fits) t = &textures_[u.cube ? dummyCube_ : u.shadow ? dummyShadow_ : dummy2D_];
    images.push_back({samplerFor(*t), t->view, VK_IMAGE_LAYOUT_GENERAL});
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = uint32_t(u.binding);
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &images.back();
    writes.push_back(w);
  }
  if (!writes.empty())
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, uint32_t(writes.size()), writes.data());
  if (indexed) {
    const Buf* ib = vi.index < buffers_.size() ? &buffers_[vi.index] : nullptr;
    if (!ib || !ib->buffer) return;
    vkCmdBindIndexBuffer(c, ib->buffer, ib->offset, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(c, uint32_t(count), 1, uint32_t(first), 0, 0);
  } else {
    vkCmdDraw(c, uint32_t(count), 1, uint32_t(first), 0);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// copies

void VkDeviceImpl::blit(uint32_t src, uint32_t dst, int sx0, int sy0, int sx1, int sy1, int dx0, int dy0, int dx1, int dy1,
                        bool color, bool depth) {
  if (src >= framebuffers_.size() || dst >= framebuffers_.size()) return;
  endRendering();
  const Fb& s = framebuffers_[src];
  const Fb& d = framebuffers_[dst];
  auto one = [&](uint32_t sh, uint32_t dh, VkImageAspectFlags aspect) {
    if (!sh || !dh) return;
    Tex& a = textures_[sh];
    Tex& b = textures_[dh];
    int w = sx1 - sx0, h = sy1 - sy0;
    bool sameSize = w == dx1 - dx0 && h == dy1 - dy0 && w > 0 && h > 0;
    if (a.samples > 1 && b.samples == 1) {
      if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageResolve r{{aspect, 0, 0, 1}, {sx0, sy0, 0}, {aspect, 0, 0, 1}, {dx0, dy0, 0}, {uint32_t(w), uint32_t(h), 1}};
        vkCmdResolveImage(cmd(), a.image, VK_IMAGE_LAYOUT_GENERAL, b.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
      } else {  // depth: no resolve command; an empty rendering pass resolves sample 0 into the single-sample image
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = a.view;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        da.resolveImageView = b.view;
        da.resolveImageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {uint32_t(std::min(a.w, b.w)), uint32_t(std::min(a.h, b.h))}};
        ri.layerCount = 1;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd(), &ri);
        vkCmdEndRendering(cmd());
      }
    } else if (sameSize && (a.vkFormat == b.vkFormat)) {
      VkImageCopy r{{aspect, 0, 0, 1}, {sx0, sy0, 0}, {aspect, 0, 0, 1}, {dx0, dy0, 0}, {uint32_t(w), uint32_t(h), 1}};
      vkCmdCopyImage(cmd(), a.image, VK_IMAGE_LAYOUT_GENERAL, b.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
      VkImageBlit r{};
      r.srcSubresource = r.dstSubresource = {aspect, 0, 0, 1};
      r.srcOffsets[0] = {sx0, sy0, 0};
      r.srcOffsets[1] = {sx1, sy1, 1};
      r.dstOffsets[0] = {dx0, dy0, 0};
      r.dstOffsets[1] = {dx1, dy1, 1};
      vkCmdBlitImage(cmd(), a.image, VK_IMAGE_LAYOUT_GENERAL, b.image, VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_NEAREST);
    }
  };
  if (color) one(s.color, d.color, VK_IMAGE_ASPECT_COLOR_BIT);
  if (depth) one(s.depth, d.depth, VK_IMAGE_ASPECT_DEPTH_BIT);
  fullBarrier(cmd());
  boundFb_ = 0;  // as GL's blit path leaves the framebuffer binding
}

void VkDeviceImpl::readPixels(uint32_t fb, int x, int y, int w, int h, uint8_t* rgba) {
  if (fb >= framebuffers_.size() || !framebuffers_[fb].color || w <= 0 || h <= 0) return;
  Tex& t = textures_[framebuffers_[fb].color];
  if (t.samples > 1) return;
  size_t bpp = t.format == Format::RGBA16F ? 8 : t.format == Format::RGBA32F ? 16 : 4;
  VkBuffer buf;
  VkDeviceSize off;
  uint8_t* ptr;
  if (!ringAlloc(size_t(w) * size_t(h) * bpp, 16, buf, off, ptr)) return;
  endRendering();
  VkBufferImageCopy c{};
  c.bufferOffset = off;
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  c.imageOffset = {x, y, 0};
  c.imageExtent = {uint32_t(w), uint32_t(h), 1};
  vkCmdCopyImageToBuffer(cmd(), t.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
  submitAndWait();
  size_t n = size_t(w) * size_t(h);
  auto toByte = [](float v) { return uint8_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
  for (size_t i = 0; i < n; ++i) {
    if (t.format == Format::RGBA16F) {
      const auto* hp = reinterpret_cast<const uint16_t*>(ptr) + i * 4;
      for (int k = 0; k < 4; ++k) rgba[i * 4 + size_t(k)] = toByte(halfToFloat(hp[k]));
    } else if (t.format == Format::RGBA32F) {
      const auto* fp = reinterpret_cast<const float*>(ptr) + i * 4;
      for (int k = 0; k < 4; ++k) rgba[i * 4 + size_t(k)] = toByte(fp[k]);
    } else if (t.vkFormat == VK_FORMAT_B8G8R8A8_UNORM) {
      rgba[i * 4] = ptr[i * 4 + 2];
      rgba[i * 4 + 1] = ptr[i * 4 + 1];
      rgba[i * 4 + 2] = ptr[i * 4];
      rgba[i * 4 + 3] = ptr[i * 4 + 3];
    } else {
      std::memcpy(rgba + i * 4, ptr + i * 4, 4);
    }
  }
}

}  // namespace

std::unique_ptr<Device> createVulkanDevice(platform::Window& window, bool vsync, std::string* error) {
  auto d = std::make_unique<VkDeviceImpl>(window);
  if (!d->init(vsync, error)) return nullptr;
  LOG_INFO("vulkan: %s", d->info().c_str());
  return d;
}

}  // namespace regs::render::gpu
