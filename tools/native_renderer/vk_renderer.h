// Native renderer for Rayman Origins: draws the game's D3D draw calls with
// Vulkan and the game's own shaders (XenosRecomp SPIR-V), without emulating
// the Xenos GPU. Used offline (tools/native_renderer/vkrender, on a frame dump)
// and in-game (rex/src/native_renderer.cpp).
//
// Input per draw: the D3D device state block (device + 0x480 .. + 0x3700, big
// endian, see docs/D3D_MAP.md), the draw arguments, and guest physical memory.
// Pipeline interface: XenosRecomp HLSL rewritten by hlsl_ubo.py (no 64-bit
// addresses, so it runs on stock Adreno drivers without shaderInt64):
//   set 0 Texture2D heap, set 1 Texture3D heap, set 2 TextureCube heap, set 3 samplers,
//   set 4 uniform buffers: 0 VS constants, 1 PS constants, 2 shared constants.
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <volk.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "spirv_inputs.h"
#include "xenos_texture.h"

namespace native {

constexpr uint32_t kStateBegin = 0x480, kStateEnd = 0x3700;  // device range the renderer reads

struct DrawCall {
  const uint8_t* state;  // device + kStateBegin .. kStateEnd, big-endian
  uint32_t entry;        // 0 DrawIndexed, 1 Draw, 2 DrawUP
  uint32_t primitive, baseVertex, startIndex, indexCount;
  uint64_t vs, ps;       // XXH3 hashes of the shader containers
  uint32_t ibWord0, ibAddress;
  const uint8_t* upData = nullptr;  // DrawVerticesUP: the vertices (host pointer, big-endian)
};

class Renderer {
 public:
  struct Layout {
    uint32_t stride;
    std::vector<VkVertexInputAttributeDescription> attributes;  // binding 0
    std::vector<uint32_t> byteOrderedOffsets;  // 4-byte attributes kept in memory byte order
  };
  struct Recorded {
    VkPipeline pipeline;
    uint32_t vsConst, psConst, shared;  // offsets into the constant buffer
    size_t vertexOffset, indexOffset;
    uint32_t indexCount;
    VkIndexType indexType;
    uint32_t firstVertex, vertexCount;  // non-indexed draws
  };
  using ShaderSource = std::function<std::vector<uint32_t>(uint64_t hash, bool vertex)>;
  using SurfaceFactory = std::function<VkSurfaceKHR(VkInstance)>;

  // loader: vkGetInstanceProcAddr to use (null: volk finds the system loader).
  // surface: creates the window surface (null: offscreen, EndFrame reads back).
  // extraInstanceExtensions: what the window system needs (e.g. from SDL).
  // Optional progress log (initialization stages), e.g. for device bring-up.
  std::function<void(const char*)> log;

  bool Init(PFN_vkGetInstanceProcAddr loader, const std::vector<const char*>& extraInstanceExtensions,
            SurfaceFactory surface, uint32_t width, uint32_t height) {
    auto stage = [this](const char* s) { if (log) log(s); };
    width_ = width;
    height_ = height;
    stage("loader");
    if (loader) volkInitializeCustom(loader);
    else if (volkInitialize() != VK_SUCCESS) return Fail("no Vulkan loader");
    stage("instance");
    if (!CreateInstance(extraInstanceExtensions)) return false;
    if (surface) {
      stage("surface");
      surface_ = surface(instance_);
      if (!surface_) return Fail("surface creation failed");
    }
    stage("device");
    if (!CreateDevice()) return false;
    stage(surface_ ? "swapchain" : "offscreen target");
    if (surface_ && !CreateSwapchain()) return false;
    if (!surface_ && !CreateOffscreen()) return false;
    stage("descriptors");
    if (!CreateDescriptors()) return false;
    constants_ = CreateBuffer(64u << 20, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    vertices_ = CreateBuffer(64u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    indices_ = CreateBuffer(16u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    staging_ = CreateBuffer(128u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    {
      // Set 4: the three constant blocks, bound with per-draw dynamic offsets.
      VkDescriptorBufferInfo infos[3] = {{constants_.buffer, 0, 4096}, {constants_.buffer, 0, 4096},
                                         {constants_.buffer, 0, 512}};
      VkWriteDescriptorSet w[3]{};
      for (uint32_t i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = sets_[4];
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[i].pBufferInfo = &infos[i];
      }
      vkUpdateDescriptorSets(device_, 3, w, 0, nullptr);
    }
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(device_, &fi, nullptr, &fence_);
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkCreateSemaphore(device_, &si, nullptr, &acquired_);
    vkCreateSemaphore(device_, &si, nullptr, &rendered_);
    ready_ = true;
    return true;
  }

  void SetShaderSource(ShaderSource source) { shaders_ = std::move(source); }
  void SetMemory(xenos::MemoryReader memory) { memory_ = std::move(memory); }
  bool ready() const { return ready_; }
  const std::string& error() const { return error_; }

  struct Stats { uint32_t draws = 0, skipped = 0, pipelines = 0, textures = 0; };
  // Why draws were skipped since the last call, e.g. "layout vs=...(0,4,8,9,12)" -> count.
  std::map<std::string, uint32_t> TakeSkipReasons() { return std::move(skipReasons_); }
  const Stats& stats() const { return stats_; }

  // Starts recording a frame. Per-frame buffers are reused (one frame in flight).
  void BeginFrame() {
    constants_.used = vertices_.used = indices_.used = staging_.used = 0;
    stats_.draws = stats_.skipped = 0;
    cmd_ = AllocCommandBuffer();
    uploads_ = AllocCommandBuffer();
    frameDraws_.clear();
  }

  // Captures one draw: copies its vertices, indices and constants right away,
  // since the game reuses the memory after the call. Every primitive type is
  // turned into an indexed triangle list (32-bit indices).
  void Draw(const DrawCall& d) {
    uint32_t prim = d.primitive;
    bool known = prim == 4 || prim == 5 || prim == 6 || prim == 13;  // list, fan, strip, quad list
    if (d.entry > 2 || !known || (d.entry == 0 && !d.ibAddress) || (d.entry == 2 && !d.upData)) {
      Skip("draw entry " + std::to_string(d.entry) + " primitive " + std::to_string(prim));
      return;
    }
    VkShaderModule vsm = Module(d.vs, true), psm = Module(d.ps, false);
    if (!vsm || !psm) {
      Skip(std::string("missing SPIR-V ") + Hex(!vsm ? d.vs : d.ps) + (!vsm ? "_vs" : "_ps"));
      return;
    }
    // Stream 0's stride: SetStreamSource keeps stride / 4 in a byte at device + 0x3268 + stream.
    // DrawVerticesUP passes its own.
    uint32_t stride = d.entry == 2 ? d.indexCount : uint32_t(d.state[0x3268 - kStateBegin]) * 4;
    std::string missing;
    const Layout* layout = LayoutFor(d.vs, stride, &missing);
    if (!layout) {
      std::string in;
      for (uint32_t l : inputs_[d.vs]) in += (in.empty() ? "" : ",") + std::to_string(l);
      Skip("vertex layout " + Hex(d.vs) + " inputs (" + in + "): " + missing);
      return;
    }
    // The vertices (source and size) and the vertex indices the primitive uses.
    const uint8_t* vb = nullptr;
    uint32_t vsize = 0;
    int64_t vertexBias = 0;  // indexed draws: base vertex
    std::vector<uint32_t> src;
    bool resettable = false;
    if (d.entry == 2) {
      // DrawVerticesUP(prim, vertexCount, data, stride): vertices inline.
      uint32_t count = d.baseVertex;
      vb = d.upData;
      vsize = count * stride;
      src.resize(count);
      for (uint32_t i = 0; i < count; ++i) src[i] = i;
    } else {
      const uint8_t* vf = d.state + 95 * 8;  // stream 0 = vertex fetch 95
      uint32_t vbase = xenos::BE32(vf) & 0x1FFFFFFC;
      vsize = ((xenos::BE32(vf + 4) >> 2) & 0xFFFFFF) * 4;
      vb = memory_(vbase, vsize);
      if (d.entry == 1) {
        // DrawVertices(prim, start, count).
        src.resize(d.startIndex);
        for (uint32_t i = 0; i < d.startIndex; ++i) src[i] = d.baseVertex + i;
      } else {
        uint32_t isize = (d.ibWord0 & 0x80000000u) ? 4 : 2;
        uint32_t ibPhys = (d.ibAddress & 0x1FFFFFFF) + (d.ibAddress >= 0xE0000000u ? 0x1000 : 0);
        const uint8_t* idx = memory_(ibPhys + d.startIndex * isize, d.indexCount * isize);
        if (!idx) { ++stats_.skipped; return; }
        src.resize(d.indexCount);
        for (uint32_t i = 0; i < d.indexCount; ++i)
          src[i] = isize == 2 ? uint32_t(idx[i * 2] << 8 | idx[i * 2 + 1]) : xenos::BE32(idx + i * 4);
        vertexBias = int32_t(d.baseVertex);
        resettable = true;
      }
    }
    std::vector<uint32_t> tris = Triangles(prim, src, resettable);
    if (!vb || !vsize || tris.empty() || vertices_.used + vsize > vertices_.size ||
        indices_.used + tris.size() * 4 > indices_.size || constants_.used + 16384 > constants_.size) {
      ++stats_.skipped;
      return;
    }
    Recorded r{};
    r.vertexOffset = CopyVertices(vb, vsize, *layout) + size_t(vertexBias * layout->stride);
    r.indexOffset = indices_.Alloc(tris.size() * 4, 4);
    std::memcpy(indices_.map + r.indexOffset, tris.data(), tris.size() * 4);
    r.indexCount = uint32_t(tris.size());
    r.indexType = VK_INDEX_TYPE_UINT32;
    FillConstants(d, r);
    uint32_t blend = xenos::BE32(d.state + (0x2934 - kStateBegin) + 4);  // RB_BLENDCONTROL0
    r.pipeline = Pipeline(d.vs, d.ps, vsm, psm, *layout, blend);
    if (!r.pipeline) { ++stats_.skipped; return; }
    frameDraws_.push_back(r);
    ++stats_.draws;
  }

  // Triangle-list indices for a Xenos primitive over the vertex indices `v`.
  // Strips and fans restart at the reset index (0xFFFF / 0xFFFFFFFF) when indexed.
  static std::vector<uint32_t> Triangles(uint32_t prim, const std::vector<uint32_t>& v, bool resettable) {
    std::vector<uint32_t> out;
    auto reset = [&](uint32_t i) { return resettable && (i == 0xFFFFu || i == 0xFFFFFFFFu); };
    if (prim == 4) {
      out.assign(v.begin(), v.begin() + v.size() / 3 * 3);
    } else if (prim == 13) {
      for (size_t q = 0; q + 3 < v.size(); q += 4) out.insert(out.end(), {v[q], v[q + 1], v[q + 2], v[q], v[q + 2], v[q + 3]});
    } else {
      size_t begin = 0;
      for (size_t i = 0; i <= v.size(); ++i) {
        if (i < v.size() && !reset(v[i])) continue;
        // Run [begin, i) without reset indices.
        for (size_t k = begin + 2; k < i; ++k) {
          if (prim == 6) {
            // Strip: keep a consistent winding (culling is off, but be exact).
            bool odd = (k - begin) % 2 == 1;
            out.insert(out.end(), {v[k - 2], odd ? v[k] : v[k - 1], odd ? v[k - 1] : v[k]});
          } else {
            out.insert(out.end(), {v[begin], v[k - 1], v[k]});  // fan
          }
        }
        begin = i + 1;
      }
    }
    return out;
  }

  // Vertex data: 8in32 swap of the whole range, then back to memory order for
  // 8-bit integer attributes (e.g. blend indices), which Vulkan reads byte-wise.
  size_t CopyVertices(const uint8_t* vb, uint32_t vsize, const Layout& layout) {
    size_t at = vertices_.Alloc(vsize, 16);
    for (uint32_t i = 0; i + 4 <= vsize; i += 4) {
      uint32_t w = xenos::BE32(vb + i);
      std::memcpy(vertices_.map + at + i, &w, 4);
    }
    for (uint32_t offset : layout.byteOrderedOffsets)
      for (uint32_t v = 0; v + layout.stride <= vsize; v += layout.stride)
        std::memcpy(vertices_.map + at + v + offset, vb + v + offset, 4);
    return at;
  }

  void FillConstants(const DrawCall& d, Recorded& r) {
    r.vsConst = CopyConstants(d.state + (0x780 - kStateBegin));
    r.psConst = CopyConstants(d.state + (0x1780 - kStateBegin));
    size_t sharedAt = constants_.Alloc(512, 256);
    uint8_t* shared = constants_.map + sharedAt;
    std::memset(shared, 0, 512);
    for (uint32_t s = 0; s < 16; ++s) {
      const uint8_t* tp = d.state + s * 24;
      if ((xenos::BE32(tp) & 3) != 2) continue;
      xenos::TextureFetch t = xenos::DecodeFetch(tp);
      int index = Texture(t, tp);
      if (index < 0) continue;
      uint32_t u = uint32_t(index), samp = std::min(t.clampX, 2u) * 3 + std::min(t.clampY, 2u);
      std::memcpy(shared + s * 4, &u, 4);
      std::memcpy(shared + 192 + s * 4, &samp, 4);
    }
    r.shared = uint32_t(sharedAt);
  }

  // Submits the frame. Presents to the window, or reads back RGBA8 into `readback`.
  bool EndFrame(std::vector<uint8_t>* readback) {
    EndCommandBuffer(uploads_);
    uint32_t imageIndex = 0;
    VkFramebuffer fb = offscreenFramebuffer_;
    if (surface_) {
      VkResult r = swapchain_ ? vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, acquired_, VK_NULL_HANDLE, &imageIndex)
                              : VK_ERROR_OUT_OF_DATE_KHR;
      if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        swapchainStale_ = true;
        SubmitUploadsOnly();
        return false;
      }
      fb = framebuffers_[imageIndex];
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);
    VkClearValue clear{};
    clear.color = {{0, 0, 0, 1}};
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = renderPass_;
    rb.framebuffer = fb;
    rb.renderArea = {{0, 0}, {extent_.width, extent_.height}};
    rb.clearValueCount = 1;
    rb.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_, &rb, VK_SUBPASS_CONTENTS_INLINE);
    // The game renders 1280x720: letterbox it into the target.
    // Fit the game's frame (16:9, or wider with the widescreen patch) into the target.
    float scale = std::min(extent_.width / (720.0f * aspect_), extent_.height / 720.0f);
    float vw = 720.0f * aspect_ * scale, vh = 720.0f * scale;
    VkViewport viewport{(extent_.width - vw) / 2, (extent_.height - vh) / 2, vw, vh, 0, 1};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 4, sets_, 0, nullptr);
    for (const Recorded& r : frameDraws_) {
      vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
      uint32_t offsets[3] = {r.vsConst, r.psConst, r.shared};
      vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 4, 1, &sets_[4], 3, offsets);
      VkDeviceSize vo = r.vertexOffset;
      vkCmdBindVertexBuffers(cmd_, 0, 1, &vertices_.buffer, &vo);
      if (r.indexCount) {
        vkCmdBindIndexBuffer(cmd_, indices_.buffer, r.indexOffset, r.indexType);
        vkCmdDrawIndexed(cmd_, r.indexCount, 1, 0, 0, 0);
      } else {
        vkCmdDraw(cmd_, r.vertexCount, 1, r.firstVertex, 0);
      }
    }
    vkCmdEndRenderPass(cmd_);
    if (readback) {
      VkBufferImageCopy copy{};
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {extent_.width, extent_.height, 1};
      if (surface_) {
        VkImage image = swapchainImages_[imageIndex];
        Barrier(cmd_, image, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &copy);
        Barrier(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
      } else {
        vkCmdCopyImageToBuffer(cmd_, offscreen_.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_.buffer, 1, &copy);
      }
    }
    vkEndCommandBuffer(cmd_);
    VkCommandBuffer cmds[2] = {uploads_, cmd_};
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 2;
    submit.pCommandBuffers = cmds;
    if (surface_) {
      submit.waitSemaphoreCount = 1;
      submit.pWaitSemaphores = &acquired_;
      submit.pWaitDstStageMask = &waitStage;
      submit.signalSemaphoreCount = 1;
      submit.pSignalSemaphores = &rendered_;
    }
    vkResetFences(device_, 1, &fence_);
    vkQueueSubmit(queue_, 1, &submit, fence_);
    if (surface_) {
      VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
      present.waitSemaphoreCount = 1;
      present.pWaitSemaphores = &rendered_;
      present.swapchainCount = 1;
      present.pSwapchains = &swapchain_;
      present.pImageIndices = &imageIndex;
      VkResult pr = vkQueuePresentKHR(queue_, &present);
      if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_SURFACE_LOST_KHR)
        swapchainStale_ = true;
    }
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    FreeFrame();
    if (readback) {
      readback->assign(readback_.map, readback_.map + size_t(extent_.width) * extent_.height * 4);
      if (surface_ && swapchainBgra_)
        for (size_t i = 0; i + 3 < readback->size(); i += 4) std::swap((*readback)[i], (*readback)[i + 2]);
    }
    return true;
  }

  // Drops the recorded frame without drawing it (no surface: the app is in
  // the background). Texture uploads are still submitted, since the cache
  // already counts them as done.
  void DiscardFrame() {
    EndCommandBuffer(uploads_);
    SubmitUploadsOnly();
  }

  // True after the swapchain stopped matching its surface (resize, rotation,
  // surface lost): call Recreate().
  bool stale() const { return swapchainStale_; }

  // New swapchain for the current surface, or for a new surface from the
  // factory (Android hands out a new window when the app comes back).
  bool Recreate(SurfaceFactory surface = nullptr) {
    vkDeviceWaitIdle(device_);
    for (VkFramebuffer f : framebuffers_) vkDestroyFramebuffer(device_, f, nullptr);
    for (VkImageView v : swapchainViews_) vkDestroyImageView(device_, v, nullptr);
    framebuffers_.clear();
    swapchainViews_.clear();
    swapchainImages_.clear();
    if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    if (surface) {
      if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
      surface_ = surface(instance_);
      if (!surface_) return Fail("surface creation failed");
    }
    swapchainStale_ = false;
    return CreateSwapchain();
  }

  // Aspect ratio of the frames the game draws (width / height).
  void SetAspect(float aspect) { aspect_ = aspect; }

  uint32_t width() const { return extent_.width; }
  uint32_t height() const { return extent_.height; }

 private:
  struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* map = nullptr;
    VkDeviceAddress address = 0;
    size_t size = 0, used = 0;
    size_t Alloc(size_t bytes, size_t align) {
      used = (used + align - 1) & ~(align - 1);
      size_t at = used;
      used += bytes;
      return at;
    }
  };
  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  void Skip(const std::string& reason) {
    ++stats_.skipped;
    ++skipReasons_[reason];
  }
  static std::string Hex(uint64_t v) {
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llX", (unsigned long long)v);
    return buf;
  }

  bool Fail(const std::string& message) {
    error_ = message;
    return false;
  }

  bool CreateInstance(const std::vector<const char*>& extra) {
    std::vector<const char*> exts = extra;
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> avail(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, avail.data());
    bool portability = false;
    for (auto& e : avail) portability |= !std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    if (portability) exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    exts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Rayman Origins native renderer";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (portability) info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(exts.size());
    info.ppEnabledExtensionNames = exts.data();
    if (vkCreateInstance(&info, nullptr, &instance_) != VK_SUCCESS) return Fail("vkCreateInstance failed");
    volkLoadInstance(instance_);
    return true;
  }

  bool CreateDevice() {
    auto stage = [this](const char* st) { if (log) log(st); };
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (!count) return Fail("no Vulkan device");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    phys_ = devices[0];
    stage("device: physical device found");
    vkGetPhysicalDeviceProperties(phys_, &props_);
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties(phys_, &families, fam.data());
    for (uint32_t i = 0; i < families; ++i) {
      VkBool32 present = VK_TRUE;
      if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(phys_, i, surface_, &present);
      if ((fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { queueFamily_ = i; break; }
    }
    stage("device: queue family chosen");
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 have{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &have12};
    if (log) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "device: features2 fn %p, api %u.%u", reinterpret_cast<void*>(vkGetPhysicalDeviceFeatures2),
                    VK_API_VERSION_MAJOR(props_.apiVersion), VK_API_VERSION_MINOR(props_.apiVersion));
      log(buf);
    }
    vkGetPhysicalDeviceFeatures2(phys_, &have);
    stage("device: features queried");
    if (!have12.runtimeDescriptorArray || !have12.descriptorBindingPartiallyBound)
      return Fail("GPU lacks descriptor indexing (runtimeDescriptorArray / partiallyBound)");
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    want12.descriptorIndexing = have12.descriptorIndexing;
    want12.runtimeDescriptorArray = VK_TRUE;
    want12.descriptorBindingPartiallyBound = VK_TRUE;
    want12.shaderSampledImageArrayNonUniformIndexing = have12.shaderSampledImageArrayNonUniformIndexing;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &want12};
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = queueFamily_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    stage("device: features ok");
    uint32_t extCount = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, exts.data());
    std::vector<const char*> enable;
    for (auto& e : exts)
      if (!std::strcmp(e.extensionName, "VK_KHR_portability_subset")) enable.push_back("VK_KHR_portability_subset");
    if (surface_) enable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = uint32_t(enable.size());
    info.ppEnabledExtensionNames = enable.data();
    stage("device: vkCreateDevice");
    if (vkCreateDevice(phys_, &info, nullptr, &device_) != VK_SUCCESS) return Fail("vkCreateDevice failed");
    stage("device: created");
    volkLoadDevice(device_);
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = queueFamily_;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    vkCreateCommandPool(device_, &pool, nullptr, &cmdPool_);
    return true;
  }

  bool CreateRenderPass(VkFormat format, VkImageLayout finalLayout) {
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = finalLayout;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    return vkCreateRenderPass(device_, &rp, nullptr, &renderPass_) == VK_SUCCESS;
  }

  bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps);
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &count, formats.data());
    VkSurfaceFormatKHR format = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (auto& f : formats)
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) { format = f; break; }
    extent_ = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{width_, height_};
    // Android reports a rotation from the panel's natural (portrait)
    // orientation; the extent is already the window's (landscape). Render
    // upright and let the compositor rotate: identity transform.
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    VkSwapchainCreateInfoKHR sc{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sc.surface = surface_;
    sc.minImageCount = std::max(2u, caps.minImageCount);
    sc.imageFormat = format.format;
    sc.imageColorSpace = format.colorSpace;
    sc.imageExtent = extent_;
    sc.imageArrayLayers = 1;
    sc.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sc.preTransform = transform;
    sc.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sc.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sc.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(device_, &sc, nullptr, &swapchain_) != VK_SUCCESS) return Fail("swapchain failed");
    // The render pass (and the pipelines built against it) outlives swapchain
    // re-creation; a window keeps its surface format.
    if (!renderPass_ && !CreateRenderPass(format.format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR)) return Fail("render pass failed");
    uint32_t images = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &images, nullptr);
    std::vector<VkImage> list(images);
    vkGetSwapchainImagesKHR(device_, swapchain_, &images, list.data());
    swapchainImages_ = list;
    swapchainBgra_ = format.format == VK_FORMAT_B8G8R8A8_UNORM;
    if (readback_.size < size_t(extent_.width) * extent_.height * 4)
      readback_ = CreateBuffer(size_t(extent_.width) * extent_.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    for (VkImage img : list) {
      VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      v.image = img;
      v.viewType = VK_IMAGE_VIEW_TYPE_2D;
      v.format = format.format;
      v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view;
      vkCreateImageView(device_, &v, nullptr, &view);
      swapchainViews_.push_back(view);
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = renderPass_;
      fb.attachmentCount = 1;
      fb.pAttachments = &view;
      fb.width = extent_.width;
      fb.height = extent_.height;
      fb.layers = 1;
      VkFramebuffer f;
      vkCreateFramebuffer(device_, &fb, nullptr, &f);
      framebuffers_.push_back(f);
    }
    return true;
  }

  bool CreateOffscreen() {
    extent_ = {width_, height_};
    if (!CreateRenderPass(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)) return Fail("render pass failed");
    offscreen_ = CreateImage(width_, height_, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = renderPass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &offscreen_.view;
    fb.width = width_;
    fb.height = height_;
    fb.layers = 1;
    vkCreateFramebuffer(device_, &fb, nullptr, &offscreenFramebuffer_);
    readback_ = CreateBuffer(size_t(width_) * height_ * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    return true;
  }

  static constexpr uint32_t kMaxTextures = 1024, kMaxSamplers = 16;

  bool CreateDescriptors() {
    {
      VkDescriptorSetLayoutBinding ubo[3]{};
      for (uint32_t i = 0; i < 3; ++i) {
        ubo[i].binding = i;
        ubo[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ubo[i].descriptorCount = 1;
        ubo[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      }
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      li.bindingCount = 3;
      li.pBindings = ubo;
      if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayouts_[4]) != VK_SUCCESS) return Fail("set layout");
    }
    for (int s = 0; s < 4; ++s) {
      VkDescriptorSetLayoutBinding b{};
      b.descriptorType = s == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b.descriptorCount = s == 3 ? kMaxSamplers : s == 0 ? kMaxTextures : 1;
      b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
      VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
      bf.bindingCount = 1;
      bf.pBindingFlags = &flags;
      VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
      li.bindingCount = 1;
      li.pBindings = &b;
      if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &setLayouts_[s]) != VK_SUCCESS) return Fail("set layout");
    }
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 5;
    pl.pSetLayouts = setLayouts_;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipelineLayout_) != VK_SUCCESS) return Fail("pipeline layout");
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures + 2},
                                    {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 3}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 5;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    vkCreateDescriptorPool(device_, &dp, nullptr, &pool_);
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = pool_;
    da.descriptorSetCount = 5;
    da.pSetLayouts = setLayouts_;
    if (vkAllocateDescriptorSets(device_, &da, sets_) != VK_SUCCESS) return Fail("descriptor sets");
    // Samplers: index = clampX * 3 + clampY over {wrap, mirror, clamp}.
    std::vector<VkDescriptorImageInfo> infos;
    for (uint32_t x = 0; x < 3; ++x)
      for (uint32_t y = 0; y < 3; ++y) {
        auto mode = [](uint32_t m) {
          return m == 0 ? VK_SAMPLER_ADDRESS_MODE_REPEAT
                 : m == 1 ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        };
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = mode(x);
        si.addressModeV = mode(y);
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkSampler s;
        vkCreateSampler(device_, &si, nullptr, &s);
        infos.push_back({s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
      }
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[3];
    w.descriptorCount = uint32_t(infos.size());
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = infos.data();
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    return true;
  }

  uint32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
  }

  Buffer CreateBuffer(size_t size, VkBufferUsageFlags usage) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = usage;
    vkCreateBuffer(device_, &info, nullptr, &b.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &alloc, nullptr, &b.memory);
    vkBindBufferMemory(device_, b.buffer, b.memory, 0);
    vkMapMemory(device_, b.memory, 0, size, 0, reinterpret_cast<void**>(&b.map));
    return b;
  }

  Image CreateImage(uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    Image img;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    vkCreateImage(device_, &info, nullptr, &img.image);
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(device_, &alloc, nullptr, &img.memory);
    vkBindImageMemory(device_, img.image, img.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = img.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(device_, &view, nullptr, &img.view);
    return img;
  }

  VkCommandBuffer AllocCommandBuffer() {
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = cmdPool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &alloc, &cmd);
    return cmd;
  }

  void EndCommandBuffer(VkCommandBuffer cmd) {
    if (!uploadsBegun_) {
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(cmd, &begin);
    }
    vkEndCommandBuffer(cmd);
    uploadsBegun_ = false;
  }

  // Submits only the frame's texture uploads (uploads_ already ended), then frees it.
  void SubmitUploadsOnly() {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &uploads_;
    vkResetFences(device_, 1, &fence_);
    vkQueueSubmit(queue_, 1, &submit, fence_);
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    FreeFrame();
  }

  void FreeFrame() {
    VkCommandBuffer cmds[2] = {uploads_, cmd_};
    vkFreeCommandBuffers(device_, cmdPool_, 2, cmds);
    uploads_ = cmd_ = VK_NULL_HANDLE;
  }

  void Barrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
  }

  uint32_t CopyConstants(const uint8_t* src) {
    size_t at = constants_.Alloc(4096, 256);
    for (uint32_t i = 0; i < 4096; i += 4) {
      uint32_t w = xenos::BE32(src + i);
      std::memcpy(constants_.map + at + i, &w, 4);
    }
    return uint32_t(at);
  }

  // Texture cache keyed by the fetch constant (address, format, size). The
  // game reuses addresses for new content (movie frames are rewritten in place
  // every frame, streaming reuses memory), so each entry keeps a cheap content
  // signature and is re-decoded into the same image when it changes.
  struct TextureEntry {
    int index = -1;
    Image image;
    uint32_t span = 0;
    uint64_t signature = 0;
  };

  uint64_t Signature(uint32_t base, uint32_t span) {
    uint64_t h = 1469598103934665603ull;
    for (uint32_t i = 0; i < 32; ++i) {
      uint32_t off = uint32_t(uint64_t(span > 8 ? span - 8 : 0) * i / 31);
      const uint8_t* p = memory_(base + off, 8);
      uint64_t v = 0;
      if (p) std::memcpy(&v, p, 8);
      h = (h ^ v) * 1099511628211ull;
    }
    return h;
  }

  void Upload(const Image& img, const xenos::TextureFetch& t, const std::vector<uint8_t>& rgba, bool fresh) {
    if (!uploadsBegun_) {
      VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      vkBeginCommandBuffer(uploads_, &begin);
      uploadsBegun_ = true;
    }
    size_t at = staging_.Alloc(rgba.size(), 16);
    std::memcpy(staging_.map + at, rgba.data(), rgba.size());
    Barrier(uploads_, img.image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t.width, t.height, 1};
    vkCmdCopyBufferToImage(uploads_, staging_.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(uploads_, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  }

  int Texture(const xenos::TextureFetch& t, const uint8_t* fetch) {
    uint64_t key = uint64_t(xenos::BE32(fetch + 4)) << 32 | xenos::BE32(fetch + 8);
    auto it = textures_.find(key);
    if (it != textures_.end()) {
      TextureEntry& e = it->second;
      if (e.index < 0) return -1;
      uint64_t signature = Signature(t.base, e.span);
      if (signature != e.signature) {
        std::vector<uint8_t> rgba;
        if (xenos::DecodeTexture(memory_, t, rgba) && staging_.used + rgba.size() <= staging_.size) {
          Upload(e.image, t, rgba, false);
          e.signature = signature;
        }
      }
      return e.index;
    }
    TextureEntry& e = textures_[key];
    if (!xenos::Supported(t.format) || textureCount_ >= kMaxTextures) {
      ++skipReasons_["texture format 0x" + std::to_string(t.format) + " (" + std::to_string(t.width) + "x" +
                     std::to_string(t.height) + ") not supported"];
      return -1;
    }
    std::vector<uint8_t> rgba;
    if (!xenos::DecodeTexture(memory_, t, rgba) || staging_.used + rgba.size() > staging_.size) {
      textures_.erase(key);
      return -1;
    }
    e.image = CreateImage(t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Upload(e.image, t, rgba, true);
    // Bytes the base level spans (an upper bound over its tiled layout).
    bool block = t.format == 0x12 || t.format == 0x13 || t.format == 0x14;
    uint32_t blockBytes = t.format == 0x12 ? 8 : block ? 16 : t.format == 0x06 ? 4 : 1;
    uint32_t bw = block ? (t.width + 3) / 4 : t.width, bh = block ? (t.height + 3) / 4 : t.height;
    e.span = ((std::max(t.pitch / (block ? 4 : 1), bw) + 31) & ~31u) * ((bh + 31) & ~31u) * blockBytes;
    e.signature = Signature(t.base, e.span);
    e.index = int(textureCount_++);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, e.image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[0];
    w.dstArrayElement = uint32_t(e.index);
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
    stats_.textures = textureCount_;
    return e.index;
  }

  VkShaderModule Module(uint64_t hash, bool vertex) {
    uint64_t key = hash ^ (vertex ? 0x8000000000000000ull : 0);
    auto it = modules_.find(key);
    if (it != modules_.end()) return it->second;
    std::vector<uint32_t> code = shaders_ ? shaders_(hash, vertex) : std::vector<uint32_t>{};
    VkShaderModule m = VK_NULL_HANDLE;
    if (!code.empty()) {
      VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = code.size() * 4;
      ci.pCode = code.data();
      vkCreateShaderModule(device_, &ci, nullptr, &m);
      if (vertex) inputs_[hash] = spirv::VertexInputLocations(code);
    }
    return modules_[key] = m;
  }

  // UbiArt vertex structs by stride (measured from captured vertex data,
  // docs/D3D_MAP.md). Locations follow XenosRecomp: 0 position, 1 normal,
  // 4..7 texcoord 0..3, 8 color, 9 blend indices. D3D patches the vertex
  // fetches at draw time from the stream stride, so the stride picks the
  // struct and the shader's inputs pick the attributes from it.
  struct Attribute {
    uint32_t location;
    VkFormat format;
    uint32_t offset;
    bool byteOrdered;  // 8-bit integer attribute, kept in memory byte order
  };
  static const std::vector<Attribute>* Struct(uint32_t stride) {
    const VkFormat f2 = VK_FORMAT_R32G32_SFLOAT, f3 = VK_FORMAT_R32G32B32_SFLOAT, f4 = VK_FORMAT_R32G32B32A32_SFLOAT,
                   c = VK_FORMAT_B8G8R8A8_UNORM, u4 = VK_FORMAT_R8G8B8A8_UINT;
    static const std::vector<Attribute> pc{{0, f3, 0}, {8, c, 12}};                              // 16
    static const std::vector<Attribute> pt{{0, f3, 0}, {4, f2, 12}};                             // 20
    static const std::vector<Attribute> pct{{0, f3, 0}, {8, c, 12}, {4, f2, 16}};                // 24
    static const std::vector<Attribute> font{{0, f3, 0}, {8, c, 12}, {9, u4, 16, true}, {4, f2, 20}};  // 28
    static const std::vector<Attribute> pnct{{0, f3, 0}, {1, f3, 12}, {8, c, 24}, {4, f2, 28}};  // 36
    static const std::vector<Attribute> patch{{0, f3, 0}, {8, c, 12}, {4, f2, 16}, {5, f4, 24}, {6, f4, 40}, {7, f2, 56}};  // 64
    switch (stride) {
      case 16: return &pc;
      case 20: return &pt;
      case 24: return &pct;
      case 28: return &font;
      case 36: return &pnct;
      case 64: return &patch;
      default: return nullptr;
    }
  }

  const Layout* LayoutFor(uint64_t vs, uint32_t stride, std::string* missing) {
    auto it = inputs_.find(vs);
    if (it == inputs_.end()) return nullptr;
    std::string key = std::to_string(vs) + ":" + std::to_string(stride);
    auto cached = layouts_.find(key);
    if (cached != layouts_.end()) return &cached->second;
    const std::vector<Attribute>* fields = Struct(stride);
    if (!fields) {
      *missing = "stride " + std::to_string(stride);
      return nullptr;
    }
    Layout layout{stride, {}, {}};
    for (uint32_t location : it->second) {
      const Attribute* a = nullptr;
      for (const Attribute& f : *fields)
        if (f.location == location) a = &f;
      if (!a) {
        *missing = "location " + std::to_string(location) + " in stride " + std::to_string(stride);
        return nullptr;
      }
      layout.attributes.push_back({location, 0, a->format, a->offset});
      if (a->byteOrdered) layout.byteOrderedOffsets.push_back(a->offset);
    }
    return &(layouts_[key] = layout);
  }

  static VkBlendFactor BlendFactor(uint32_t x) {
    static const VkBlendFactor map[] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_CONSTANT_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, VK_BLEND_FACTOR_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return x < 17 ? map[x] : VK_BLEND_FACTOR_ONE;
  }
  static VkBlendOp BlendOp(uint32_t x) {
    switch (x) {
      case 1: return VK_BLEND_OP_SUBTRACT;
      case 2: return VK_BLEND_OP_MIN;
      case 3: return VK_BLEND_OP_MAX;
      case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
      default: return VK_BLEND_OP_ADD;
    }
  }

  VkPipeline Pipeline(uint64_t vs, uint64_t ps, VkShaderModule vsm, VkShaderModule psm, const Layout& layout,
                      uint32_t blend) {
    std::string key = std::to_string(vs) + ":" + std::to_string(ps) + ":" + std::to_string(blend) + ":" +
                      std::to_string(layout.stride);
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main"};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psm, "main"};
    VkVertexInputBindingDescription bind{0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = uint32_t(layout.attributes.size());
    vi.pVertexAttributeDescriptions = layout.attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cb{};
    uint32_t cs = blend & 31, cop = (blend >> 5) & 7, cd = (blend >> 8) & 31;
    uint32_t as = (blend >> 16) & 31, aop = (blend >> 21) & 7, ad = (blend >> 24) & 31;
    cb.blendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
    cb.srcColorBlendFactor = BlendFactor(cs);
    cb.dstColorBlendFactor = BlendFactor(cd);
    cb.colorBlendOp = BlendOp(cop);
    cb.srcAlphaBlendFactor = BlendFactor(as);
    cb.dstAlphaBlendFactor = BlendFactor(ad);
    cb.alphaBlendOp = BlendOp(aop);
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cbs;
    gp.pDynamicState = &ds;
    gp.layout = pipelineLayout_;
    gp.renderPass = renderPass_;
    VkPipeline pipe = VK_NULL_HANDLE;
    vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe);
    stats_.pipelines = uint32_t(pipelines_.size() + 1);
    return pipelines_[key] = pipe;
  }

  uint32_t width_ = 0, height_ = 0;
  float aspect_ = 16.0f / 9.0f;
  VkExtent2D extent_{};
  bool ready_ = false;
  std::string error_;
  ShaderSource shaders_;
  xenos::MemoryReader memory_;
  Stats stats_;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties props_{};
  VkPhysicalDeviceMemoryProperties memProps_{};
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  uint32_t queueFamily_ = 0;
  VkCommandPool cmdPool_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> framebuffers_;
  std::vector<VkImage> swapchainImages_;
  std::vector<VkImageView> swapchainViews_;
  bool swapchainStale_ = false;
  bool swapchainBgra_ = false;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;
  Image offscreen_;
  VkFramebuffer offscreenFramebuffer_ = VK_NULL_HANDLE;
  Buffer readback_, constants_, vertices_, indices_, staging_;
  VkDescriptorSetLayout setLayouts_[5]{};
  VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  VkDescriptorSet sets_[5]{};
  VkFence fence_ = VK_NULL_HANDLE;
  VkSemaphore acquired_ = VK_NULL_HANDLE, rendered_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE, uploads_ = VK_NULL_HANDLE;
  bool uploadsBegun_ = false;
  std::vector<Recorded> frameDraws_;
  std::unordered_map<uint64_t, TextureEntry> textures_;
  uint32_t textureCount_ = 0;
  std::unordered_map<uint64_t, VkShaderModule> modules_;
  std::unordered_map<uint64_t, std::vector<uint32_t>> inputs_;
  std::unordered_map<std::string, VkPipeline> pipelines_;
  std::unordered_map<std::string, Layout> layouts_;
  std::map<std::string, uint32_t> skipReasons_;
};

}  // namespace native
