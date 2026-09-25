// vkrender: renders a frame dump (rex/src/native_capture.cpp) on the GPU with
// the game's own shaders, recompiled by XenosRecomp to SPIR-V. No Xenos
// emulation: textures, vertices and constants come straight from the D3D state.
//
// Usage: vkrender <native_frame.bin> <spirv dir> <out.tga>
//   <spirv dir> holds <HASH>_vs.spv / <HASH>_ps.spv and the matching .hlsl
//   (the .hlsl is read for the vertex inputs).
//
// Pipeline interface (XenosRecomp shader_common.h, SPIR-V flavour):
//   push constants: 3 x uint64 buffer addresses (VS constants, PS constants, shared)
//   set 0: Texture2D heap, set 1: Texture3D heap, set 2: TextureCube heap, set 3: samplers
//   VS inputs by semantic: POSITION 0, TEXCOORD0-3 4-7, COLOR0 8, ...
#define DUMPVIEW_NO_MAIN
#include "dumpview.cpp"

#define VK_NO_PROTOTYPES
#include <volk.h>

#include <array>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace dv;

#define VKCHECK(x)                                                                   \
  do {                                                                               \
    VkResult r_ = (x);                                                               \
    if (r_ != VK_SUCCESS) {                                                          \
      std::fprintf(stderr, "%s failed: %d (line %d)\n", #x, int(r_), __LINE__);      \
      std::exit(1);                                                                  \
    }                                                                                \
  } while (0)

namespace {

constexpr uint32_t kWidth = 1280, kHeight = 720;
constexpr uint32_t kMaxTextures = 256, kMaxSamplers = 16;

VkInstance g_instance;
VkPhysicalDevice g_phys;
VkDevice g_device;
VkQueue g_queue;
uint32_t g_queueFamily;
VkPhysicalDeviceMemoryProperties g_memProps;
VkCommandPool g_cmdPool;

uint32_t FindMemory(uint32_t bits, VkMemoryPropertyFlags props) {
  for (uint32_t i = 0; i < g_memProps.memoryTypeCount; ++i)
    if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & props) == props) return i;
  std::fprintf(stderr, "no memory type\n");
  std::exit(1);
}

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* map = nullptr;
  VkDeviceAddress address = 0;
  size_t size = 0, used = 0;

  // Bump allocation inside a host-visible buffer.
  size_t Alloc(size_t bytes, size_t align) {
    used = (used + align - 1) & ~(align - 1);
    size_t at = used;
    used += bytes;
    if (used > size) {
      std::fprintf(stderr, "buffer overflow\n");
      std::exit(1);
    }
    return at;
  }
};

Buffer CreateBuffer(size_t size, VkBufferUsageFlags usage) {
  Buffer b;
  b.size = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VKCHECK(vkCreateBuffer(g_device, &info, nullptr, &b.buffer));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(g_device, b.buffer, &req);
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &flags};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VKCHECK(vkAllocateMemory(g_device, &alloc, nullptr, &b.memory));
  VKCHECK(vkBindBufferMemory(g_device, b.buffer, b.memory, 0));
  VKCHECK(vkMapMemory(g_device, b.memory, 0, size, 0, reinterpret_cast<void**>(&b.map)));
  VkBufferDeviceAddressInfo addr{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  addr.buffer = b.buffer;
  b.address = vkGetBufferDeviceAddress(g_device, &addr);
  return b;
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
};

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
  VKCHECK(vkCreateImage(g_device, &info, nullptr, &img.image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(g_device, img.image, &req);
  VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = FindMemory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VKCHECK(vkAllocateMemory(g_device, &alloc, nullptr, &img.memory));
  VKCHECK(vkBindImageMemory(g_device, img.image, img.memory, 0));
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = img.image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VKCHECK(vkCreateImageView(g_device, &view, nullptr, &img.view));
  return img;
}

VkCommandBuffer BeginOneShot() {
  VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  alloc.commandPool = g_cmdPool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd;
  VKCHECK(vkAllocateCommandBuffers(g_device, &alloc, &cmd));
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VKCHECK(vkBeginCommandBuffer(cmd, &begin));
  return cmd;
}

void SubmitAndWait(VkCommandBuffer cmd) {
  VKCHECK(vkEndCommandBuffer(cmd));
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  VKCHECK(vkQueueSubmit(g_queue, 1, &submit, VK_NULL_HANDLE));
  VKCHECK(vkQueueWaitIdle(g_queue));
  vkFreeCommandBuffers(g_device, g_cmdPool, 1, &cmd);
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
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
}

void InitVulkan() {
  VKCHECK(volkInitialize());
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_2;
  const char* instanceExts[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                                VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = 2;
  info.ppEnabledExtensionNames = instanceExts;
  VKCHECK(vkCreateInstance(&info, nullptr, &g_instance));
  volkLoadInstance(g_instance);

  uint32_t count = 1;
  VkResult r = vkEnumeratePhysicalDevices(g_instance, &count, &g_phys);
  if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !count) {
    std::fprintf(stderr, "no Vulkan device\n");
    std::exit(1);
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(g_phys, &props);
  std::printf("GPU: %s\n", props.deviceName);
  vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memProps);

  uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &families, nullptr);
  std::vector<VkQueueFamilyProperties> fam(families);
  vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &families, fam.data());
  for (uint32_t i = 0; i < families; ++i)
    if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g_queueFamily = i; break; }

  // Features the XenosRecomp shaders need.
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
  vkGetPhysicalDeviceFeatures2(g_phys, &f2);
  std::printf("features: int64 %d, bufferDeviceAddress %d, runtimeDescriptorArray %d, partiallyBound %d\n",
              f2.features.shaderInt64, f12.bufferDeviceAddress, f12.runtimeDescriptorArray,
              f12.descriptorBindingPartiallyBound);
  VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  want12.bufferDeviceAddress = VK_TRUE;
  want12.descriptorIndexing = f12.descriptorIndexing;
  want12.runtimeDescriptorArray = VK_TRUE;
  want12.descriptorBindingPartiallyBound = VK_TRUE;
  want12.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
  VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &want12};
  want.features.shaderInt64 = VK_TRUE;

  float priority = 1.0f;
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueFamilyIndex = g_queueFamily;
  queue.queueCount = 1;
  queue.pQueuePriorities = &priority;
  const char* deviceExts[] = {"VK_KHR_portability_subset"};
  uint32_t extCount = 0;
  vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &extCount, nullptr);
  std::vector<VkExtensionProperties> exts(extCount);
  vkEnumerateDeviceExtensionProperties(g_phys, nullptr, &extCount, exts.data());
  bool portability = false;
  for (auto& e : exts) portability |= !std::strcmp(e.extensionName, "VK_KHR_portability_subset");
  VkDeviceCreateInfo dinfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &want};
  dinfo.queueCreateInfoCount = 1;
  dinfo.pQueueCreateInfos = &queue;
  dinfo.enabledExtensionCount = portability ? 1 : 0;
  dinfo.ppEnabledExtensionNames = deviceExts;
  VKCHECK(vkCreateDevice(g_phys, &dinfo, nullptr, &g_device));
  volkLoadDevice(g_device);
  vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);

  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.queueFamilyIndex = g_queueFamily;
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  VKCHECK(vkCreateCommandPool(g_device, &pool, nullptr, &g_cmdPool));
}

std::vector<uint32_t> ReadSpirv(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)), {});
  std::vector<uint32_t> words(bytes.size() / 4);
  std::memcpy(words.data(), bytes.data(), words.size() * 4);
  return words;
}

// Vertex input locations the VS declares ([[vk::location(N)]] in ...).
std::vector<uint32_t> VertexInputs(const std::string& hlslPath) {
  std::ifstream f(hlslPath);
  std::string line;
  std::vector<uint32_t> locs;
  bool inMain = false;
  while (std::getline(f, line)) {
    if (line.rfind("void main(", 0) == 0) inMain = true;
    if (!inMain) continue;
    auto p = line.find("[[vk::location(");
    if (p != std::string::npos && line.find(" in ") != std::string::npos)
      locs.push_back(uint32_t(std::stoul(line.substr(p + 15))));
    if (line.find('{') == 0) break;
  }
  std::sort(locs.begin(), locs.end());
  return locs;
}

struct Attribute { uint32_t location; VkFormat format; uint32_t offset; };
struct VertexLayout { uint32_t stride; std::vector<Attribute> attributes; };

// UbiArt vertex formats, measured from captured vertex data (docs/D3D_MAP.md).
bool LayoutFor(const std::vector<uint32_t>& inputs, VertexLayout& out) {
  const VkFormat f2 = VK_FORMAT_R32G32_SFLOAT, f3 = VK_FORMAT_R32G32B32_SFLOAT,
                 f4 = VK_FORMAT_R32G32B32A32_SFLOAT, c = VK_FORMAT_B8G8R8A8_UNORM;
  if (inputs == std::vector<uint32_t>{0, 4, 8}) {  // renderpct: position, color, uv
    out = {24, {{0, f3, 0}, {8, c, 12}, {4, f2, 16}}};
    return true;
  }
  if (inputs == std::vector<uint32_t>{0, 4, 5, 6, 7, 8}) {  // animated patch
    out = {64, {{0, f3, 0}, {8, c, 12}, {4, f2, 16}, {5, f4, 24}, {6, f4, 40}, {7, f2, 56}}};
    return true;
  }
  return false;
}

// Xenos blend factor / op -> Vulkan.
VkBlendFactor BlendFactor(uint32_t x) {
  switch (x) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}
VkBlendOp BlendOp(uint32_t x) {
  switch (x) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

struct PipelineKey {
  uint64_t vs, ps;
  uint32_t blend;
  bool operator==(const PipelineKey& o) const { return vs == o.vs && ps == o.ps && blend == o.blend; }
};
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const { return size_t(k.vs * 31 + k.ps * 17 + k.blend); }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s native_frame.bin spirv_dir out.tga\n", argv[0]);
    return 1;
  }
  Dump dump;
  if (!Load(argv[1], dump)) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 1;
  }
  std::string spirvDir = argv[2];
  InitVulkan();

  // ---- Render target ----
  Image target = CreateImage(kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  VkAttachmentDescription color{};
  color.format = VK_FORMAT_R8G8B8A8_UNORM;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1;
  rp.pAttachments = &color;
  rp.subpassCount = 1;
  rp.pSubpasses = &subpass;
  VkRenderPass renderPass;
  VKCHECK(vkCreateRenderPass(g_device, &rp, nullptr, &renderPass));
  VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
  fb.renderPass = renderPass;
  fb.attachmentCount = 1;
  fb.pAttachments = &target.view;
  fb.width = kWidth;
  fb.height = kHeight;
  fb.layers = 1;
  VkFramebuffer framebuffer;
  VKCHECK(vkCreateFramebuffer(g_device, &fb, nullptr, &framebuffer));

  // ---- Descriptor heaps (sets 0-3) ----
  VkDescriptorSetLayout setLayouts[4];
  for (int s = 0; s < 4; ++s) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = s == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = s == 3 ? kMaxSamplers : s == 0 ? kMaxTextures : 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bf.bindingCount = 1;
    bf.pBindingFlags = &flags;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bf};
    li.bindingCount = 1;
    li.pBindings = &binding;
    VKCHECK(vkCreateDescriptorSetLayout(g_device, &li, nullptr, &setLayouts[s]));
  }
  VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24};
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 4;
  pl.pSetLayouts = setLayouts;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &push;
  VkPipelineLayout pipelineLayout;
  VKCHECK(vkCreatePipelineLayout(g_device, &pl, nullptr, &pipelineLayout));
  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures + 2},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers}};
  VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp.maxSets = 4;
  dp.poolSizeCount = 2;
  dp.pPoolSizes = sizes;
  VkDescriptorPool pool;
  VKCHECK(vkCreateDescriptorPool(g_device, &dp, nullptr, &pool));
  VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  da.descriptorPool = pool;
  da.descriptorSetCount = 4;
  da.pSetLayouts = setLayouts;
  VkDescriptorSet sets[4];
  VKCHECK(vkAllocateDescriptorSets(g_device, &da, sets));

  // Samplers: index = clampX * 3 + clampY over {wrap, mirror, clamp}.
  std::vector<VkSampler> samplers;
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
      si.maxLod = 0;
      VkSampler s;
      VKCHECK(vkCreateSampler(g_device, &si, nullptr, &s));
      samplers.push_back(s);
    }
  {
    std::vector<VkDescriptorImageInfo> infos;
    for (auto s : samplers) infos.push_back({s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets[3];
    w.descriptorCount = uint32_t(infos.size());
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = infos.data();
    vkUpdateDescriptorSets(g_device, 1, &w, 0, nullptr);
  }

  // ---- Textures ----
  std::unordered_map<uint32_t, uint32_t> textureIndex;  // guest base -> heap index
  std::vector<Image> textures;
  Buffer staging = CreateBuffer(256u << 20, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  VkCommandBuffer upload = BeginOneShot();
  auto textureFor = [&](const TextureFetch& t) -> int {
    auto it = textureIndex.find(t.base);
    if (it != textureIndex.end()) return int(it->second);
    std::vector<uint8_t> rgba;
    if (textures.size() >= kMaxTextures || !DecodeTexture(dump, t, rgba)) return -1;
    Image img = CreateImage(t.width, t.height, VK_FORMAT_R8G8B8A8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    size_t at = staging.Alloc(rgba.size(), 16);
    std::memcpy(staging.map + at, rgba.data(), rgba.size());
    Barrier(upload, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t.width, t.height, 1};
    vkCmdCopyBufferToImage(upload, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Barrier(upload, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    uint32_t index = uint32_t(textures.size());
    textures.push_back(img);
    textureIndex[t.base] = index;
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets[0];
    w.dstArrayElement = index;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_device, 1, &w, 0, nullptr);
    return int(index);
  };

  // ---- Per-draw data ----
  Buffer constants = CreateBuffer(64u << 20, 0);
  Buffer vertices = CreateBuffer(64u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
  Buffer indices = CreateBuffer(16u << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

  struct Prepared {
    VkPipeline pipeline;
    VkDeviceAddress vsConst, psConst, shared;
    size_t vertexOffset, indexOffset;
    uint32_t indexCount;
    VkIndexType indexType;
  };
  std::vector<Prepared> prepared;
  std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> pipelines;
  std::unordered_map<uint64_t, VkShaderModule> modules;
  std::map<uint32_t, int> targets;  // RB_COLOR_INFO -> draw count
  int skippedLayout = 0, skippedOther = 0;

  auto moduleFor = [&](uint64_t hash, bool vertex) -> VkShaderModule {
    auto it = modules.find(hash);
    if (it != modules.end()) return it->second;
    char name[64];
    std::snprintf(name, sizeof(name), "/%016llX_%s.spv", (unsigned long long)hash, vertex ? "vs" : "ps");
    auto code = ReadSpirv(spirvDir + name);
    VkShaderModule m = VK_NULL_HANDLE;
    if (!code.empty()) {
      VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = code.size() * 4;
      ci.pCode = code.data();
      VKCHECK(vkCreateShaderModule(g_device, &ci, nullptr, &m));
    }
    modules[hash] = m;
    return m;
  };

  for (const Draw& d : dump.draws) {
    uint32_t colorInfo = BE32(d.device.data() + (0x2880 - kDumpBegin) + 4);
    targets[colorInfo]++;
  }
  // The main render target: the one the last draws of the frame use.
  uint32_t mainTarget = dump.draws.empty() ? 0 : BE32(dump.draws.back().device.data() + (0x2880 - kDumpBegin) + 4);

  for (const Draw& d : dump.draws) {
    uint32_t colorInfo = BE32(d.device.data() + (0x2880 - kDumpBegin) + 4);
    if (d.entry != 0 || d.prim != 4 || !d.ibAddress || colorInfo != mainTarget) { ++skippedOther; continue; }
    char hlsl[64];
    std::snprintf(hlsl, sizeof(hlsl), "/%016llX_vs.hlsl", (unsigned long long)d.vs);
    VertexLayout layout;
    if (!LayoutFor(VertexInputs(spirvDir + hlsl), layout)) { ++skippedLayout; continue; }
    VkShaderModule vsm = moduleFor(d.vs, true), psm = moduleFor(d.ps, false);
    if (!vsm || !psm) { ++skippedOther; continue; }

    // Geometry: indices (big-endian -> little-endian) and the vertex range (8in32 swap).
    uint32_t isize = (d.ibWord0 & 0x80000000u) ? 4 : 2;
    uint32_t ibPhys = (d.ibAddress & 0x1FFFFFFF) + (d.ibAddress >= 0xE0000000u ? 0x1000 : 0);
    const uint8_t* idx = Memory(dump, ibPhys + d.a6 * isize, d.a7 * isize);
    const uint8_t* vf = d.device.data() + 95 * 8;
    uint32_t vbase = BE32(vf) & 0x1FFFFFFC, vsize = ((BE32(vf + 4) >> 2) & 0xFFFFFF) * 4;
    const uint8_t* vb = Memory(dump, vbase, vsize);
    if (!idx || !vb) { ++skippedOther; continue; }
    Prepared p{};
    p.vertexOffset = vertices.Alloc(vsize, 16);
    for (uint32_t i = 0; i + 4 <= vsize; i += 4) {
      uint32_t w = BE32(vb + i);
      std::memcpy(vertices.map + p.vertexOffset + i, &w, 4);
    }
    // Rebase so vertex 0 of the draw is at the buffer start (baseVertex = a5).
    p.vertexOffset += size_t(d.a5) * layout.stride;
    p.indexOffset = indices.Alloc(d.a7 * isize, 4);
    for (uint32_t i = 0; i < d.a7; ++i) {
      if (isize == 2) {
        uint16_t v = uint16_t(idx[i * 2] << 8 | idx[i * 2 + 1]);
        std::memcpy(indices.map + p.indexOffset + i * 2, &v, 2);
      } else {
        uint32_t v = BE32(idx + i * 4);
        std::memcpy(indices.map + p.indexOffset + i * 4, &v, 4);
      }
    }
    p.indexCount = d.a7;
    p.indexType = isize == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    // Constants: 256 float4 each for VS (device+0x780) and PS (device+0x1780), little-endian.
    auto copyConstants = [&](uint32_t deviceOffset) {
      size_t at = constants.Alloc(4096, 256);
      const uint8_t* src = d.device.data() + (deviceOffset - kDumpBegin);
      for (uint32_t i = 0; i < 4096; i += 4) {
        uint32_t w = BE32(src + i);
        std::memcpy(constants.map + at + i, &w, 4);
      }
      return constants.address + at;
    };
    p.vsConst = copyConstants(0x780);
    p.psConst = copyConstants(0x1780);
    // Shared constants: texture/sampler heap indices per fetch slot, booleans, etc.
    size_t sharedAt = constants.Alloc(512, 256);
    uint8_t* shared = constants.map + sharedAt;
    std::memset(shared, 0, 512);
    for (uint32_t s = 0; s < 16; ++s) {
      const uint8_t* tp = d.device.data() + s * 24;
      if ((BE32(tp) & 3) != 2) continue;
      TextureFetch t = DecodeFetch(tp);
      int index = textureFor(t);
      if (index < 0) continue;
      uint32_t u = uint32_t(index), samp = std::min(t.clampX, 2u) * 3 + std::min(t.clampY, 2u);
      std::memcpy(shared + s * 4, &u, 4);          // Texture2D index
      std::memcpy(shared + 192 + s * 4, &samp, 4);  // sampler index
    }
    p.shared = constants.address + sharedAt;

    // Pipeline: shaders + blend state (RB_BLENDCONTROL0, register 0x2201).
    uint32_t blend = BE32(d.device.data() + (0x2934 - kDumpBegin) + 4);
    PipelineKey key{d.vs, d.ps, blend};
    auto pit = pipelines.find(key);
    if (pit == pipelines.end()) {
      VkPipelineShaderStageCreateInfo stages[2]{};
      stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vsm, "main"};
      stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, psm, "main"};
      VkVertexInputBindingDescription bind{0, layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
      std::vector<VkVertexInputAttributeDescription> attrs;
      for (auto& a : layout.attributes) attrs.push_back({a.location, 0, a.format, a.offset});
      VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
      vi.vertexBindingDescriptionCount = 1;
      vi.pVertexBindingDescriptions = &bind;
      vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
      vi.pVertexAttributeDescriptions = attrs.data();
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
      gp.layout = pipelineLayout;
      gp.renderPass = renderPass;
      VkPipeline pipe;
      VKCHECK(vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipe));
      pit = pipelines.emplace(key, pipe).first;
    }
    p.pipeline = pit->second;
    prepared.push_back(p);
  }
  SubmitAndWait(upload);
  std::printf("render targets in frame:");
  for (auto& [info, n] : targets) std::printf(" %08X x%d%s", info, n, info == mainTarget ? " (main)" : "");
  std::printf("\n%zu draws prepared, %zu pipelines, %zu textures; skipped %d (vertex layout) + %d (other)\n",
              prepared.size(), pipelines.size(), textures.size(), skippedLayout, skippedOther);

  // ---- Record and submit ----
  VkCommandBuffer cmd = BeginOneShot();
  VkClearValue clear{};
  clear.color = {{0, 0, 0, 1}};
  VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rb.renderPass = renderPass;
  rb.framebuffer = framebuffer;
  rb.renderArea = {{0, 0}, {kWidth, kHeight}};
  rb.clearValueCount = 1;
  rb.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rb, VK_SUBPASS_CONTENTS_INLINE);
  VkViewport viewport{0, 0, float(kWidth), float(kHeight), 0, 1};
  VkRect2D scissor{{0, 0}, {kWidth, kHeight}};
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 4, sets, 0, nullptr);
  for (const Prepared& p : prepared) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
    uint64_t pc[3] = {p.vsConst, p.psConst, p.shared};
    vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 24, pc);
    VkDeviceSize vo = p.vertexOffset;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices.buffer, &vo);
    vkCmdBindIndexBuffer(cmd, indices.buffer, p.indexOffset, p.indexType);
    vkCmdDrawIndexed(cmd, p.indexCount, 1, 0, 0, 0);
  }
  vkCmdEndRenderPass(cmd);
  Buffer readback = CreateBuffer(size_t(kWidth) * kHeight * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {kWidth, kHeight, 1};
  vkCmdCopyImageToBuffer(cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
  SubmitAndWait(cmd);

  std::vector<uint8_t> rgba(readback.map, readback.map + size_t(kWidth) * kHeight * 4);
  for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;
  WriteTga(argv[3], kWidth, kHeight, rgba);
  std::printf("wrote %s\n", argv[3]);
  return 0;
}
