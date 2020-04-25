////////////////////////////////////////////////////////////////////////////////
//
// Demo framework for the Vookoo for the Vookoo high level C++ Vulkan interface.
//
// (C) Andy Thomason 2017 MIT License
//
// This is an optional demo framework for the Vookoo high level C++ Vulkan interface.
//
////////////////////////////////////////////////////////////////////////////////

#ifndef VKU_FRAMEWORK_HPP
#define VKU_FRAMEWORK_HPP

#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#define GLFW_EXPOSE_NATIVE_WIN32
#define VKU_SURFACE "VK_KHR_win32_surface"
#pragma warning(disable : 4005)
#elif defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#else // X11
#define VK_USE_PLATFORM_XLIB_KHR
#define GLFW_EXPOSE_NATIVE_X11
#define VKU_SURFACE "VK_KHR_xlib_surface"
#endif

#ifndef VKU_NO_GLFW
#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

// Undo damage done by windows.h
#undef APIENTRY
#undef None
#undef max
#undef min

#include <array>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <thread>
#include <chrono>
#include <functional>
#include <cstddef>
#include <mutex>
#include <map>
#include <tuple>

#include <set>
#include <vku/vku.hpp>
#include <vulkan/vulkan.hpp>
#define RDTSC
#ifdef RDTSC
//  Windows
#ifdef _WIN32

#include <intrin.h>
uint64_t rdtsc(){
   return __rdtsc();
}

//  Linux/GCC
#else
static inline unsigned long long rdtsc() {
  unsigned hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((unsigned long long)lo) | (((unsigned long long)hi) << 32);
}
#endif // _WIN32
#else
static inline unsigned long long rdtsc() { return 0; }
#endif

namespace vku {


template <class F>
class final_act
{
public:
  explicit final_act(F f) noexcept
      : f_(std::move(f)), invoke_(true) {}

  final_act(final_act&& other) noexcept
      : f_(std::move(other.f_)),
        invoke_(other.invoke_)
  {
    other.invoke_ = false;
  }

  final_act(const final_act&) = delete;
  final_act& operator=(const final_act&) = delete;

  ~final_act() noexcept
  {
    if (invoke_) f_();
  }

private:
  F f_;
  bool invoke_;
};

template <typename F>
final_act<F> on_death(F &&f) { return final_act<F>{std::forward<F>(f)}; }

template <typename T>
class locked_access {
public:
  locked_access(T t, std::mutex &mtx) : t(t), lg(mtx) {}
  const T *operator->() const { return &t; }
private:
  T t;
  std::lock_guard<std::mutex> lg;
};

template <typename T>
class locked_deref {
public:
  locked_deref(T t, std::mutex &mtx) : t(t), lg(mtx) {}
  operator const T &() { return t; }
private:
  T t;
  std::lock_guard<std::mutex> lg;
};

template <typename T>
class synchronized_ref {
public:
  synchronized_ref(T t, std::mutex *mtx) : t(t), mtx{mtx} {}
  synchronized_ref() : t{}, mtx{nullptr} {}

  locked_access<T> operator->() const { return {t, *mtx}; }
  locked_deref<T> operator*() const { return {t, *mtx}; }
private:
  T t;
  std::mutex *mtx;
};

class SynchronizedQueue : public synchronized_ref<vk::Queue> {
public:
  using synchronized_ref<vk::Queue>::synchronized_ref;
  template <typename Dispatch = VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>
  void submit(uint32_t submitCount, const vk::SubmitInfo *submits,
              vk::Fence fence, Dispatch const &d = Dispatch{}) const {
    this->operator->()->submit(submitCount, submits, fence, d);
  }
  template <typename Dispatch = VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>
  void submit(vk::ArrayProxy<const vk::SubmitInfo> submits, vk::Fence fence,
              Dispatch const &d = Dispatch{}) const {
    this->operator->()->submit(submits, fence, d);
  }

  template <typename Dispatch = VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>
  vk::Result presentKHR(const vk::PresentInfoKHR &presentInfo,
                        Dispatch const &d = Dispatch{}) const {
    return this->operator->()->presentKHR(presentInfo, d);
  }
};

/// This class provides an optional interface to the vulkan instance, devices and queues.
/// It is not used by any of the other classes directly and so can be safely ignored if Vookoo
/// is embedded in an engine.
/// See https://vulkan-tutorial.com for details of many operations here.
class Framework {
public:
  Framework() {
  }

  // Construct a framework containing the instance, a device and one or more queues.
  Framework(const std::string &name) {
    livePools_ = std::make_shared<SyncDescriptors>();
    vku::InstanceMaker im{};
    im.defaultLayers();
    instance_ = im.createUnique();

    callback_ = DebugCallback(*instance_);

    auto pds = instance_->enumeratePhysicalDevices();
    physical_device_ = pds[0];
    auto qprops = physical_device_.getQueueFamilyProperties();
    const auto badQueue = ~(uint32_t)0;
    graphicsQueueFamilyIndex_ = badQueue;
    computeQueueFamilyIndex_ = badQueue;
    vk::QueueFlags search = vk::QueueFlagBits::eGraphics|vk::QueueFlagBits::eCompute;

    // Look for an omnipurpose queue family first
    // It is better if we can schedule operations without barriers and semaphores.
    // The Spec says: "If an implementation exposes any queue family that supports graphics operations,
    // at least one queue family of at least one physical device exposed by the implementation
    // must support both graphics and compute operations."
    // Also: All commands that are allowed on a queue that supports transfer operations are
    // also allowed on a queue that supports either graphics or compute operations...
    // As a result we can expect a queue family with at least all three and maybe all four modes.
    for (uint32_t qi = 0; qi != qprops.size(); ++qi) {
      auto &qprop = qprops[qi];
      std::cout << vk::to_string(qprop.queueFlags) << "\n";
      if ((qprop.queueFlags & search) == search) {
        graphicsQueueFamilyIndex_ = qi;
        computeQueueFamilyIndex_ = qi;
        break;
      }
    }

    if (graphicsQueueFamilyIndex_ == badQueue || computeQueueFamilyIndex_ == badQueue) {
      std::cerr << "oops, missing a queue\n";
      return;
    }

    memprops_ = physical_device_.getMemoryProperties();

    // todo: find optimal texture format
    // auto rgbaprops = physical_device_.getFormatProperties(vk::Format::eR8G8B8A8Unorm);

    vku::DeviceMaker dm{};
    dm.defaultLayers();
    dm.queue(graphicsQueueFamilyIndex_);
    if (computeQueueFamilyIndex_ != graphicsQueueFamilyIndex_) dm.queue(computeQueueFamilyIndex_);
    device_ = dm.createUnique(physical_device_);
    
    vk::PipelineCacheCreateInfo pipelineCacheInfo{};
    pipelineCache_ = device_->createPipelineCacheUnique(pipelineCacheInfo);

    std::vector<vk::DescriptorPoolSize> poolSizes;
    poolSizes.emplace_back(vk::DescriptorType::eUniformBuffer, 128);
    poolSizes.emplace_back(vk::DescriptorType::eCombinedImageSampler, 128);
    poolSizes.emplace_back(vk::DescriptorType::eStorageBuffer, 128);

    graphicsQueue_ = Framework::getQueue(*device_, graphicsQueueFamilyIndex_, 0);
    computeQueue_  = Framework::getQueue(*device_, computeQueueFamilyIndex_, 0);;
    ok_ = true;
  }

  void dumpCaps(std::ostream &os) const {
    os << "Memory Types\n";
    for (uint32_t i = 0; i != memprops_.memoryTypeCount; ++i) {
      os << "  type" << i << " heap" << memprops_.memoryTypes[i].heapIndex << " " << vk::to_string(memprops_.memoryTypes[i].propertyFlags) << "\n";
    }
    os << "Heaps\n";
    for (uint32_t i = 0; i != memprops_.memoryHeapCount; ++i) {
      os << "  heap" << vk::to_string(memprops_.memoryHeaps[i].flags) << " " << memprops_.memoryHeaps[i].size << "\n";
    }
  }

  /// Get the Vulkan instance.
  const vk::Instance instance() const { return *instance_; }

  /// Get the Vulkan device.
  const vk::Device device() const { return *device_; }

  /// Get the queue used to submit graphics jobs
  SynchronizedQueue graphicsQueue() const {
    return graphicsQueue_;
  }

  /// Get the queue used to submit compute jobs
  const vk::Queue computeQueue() const { return device_->getQueue(computeQueueFamilyIndex_, 0); }

  /// Get the physical device.
  const vk::PhysicalDevice &physicalDevice() const { return physical_device_; }

  /// Get the default pipeline cache (you can use your own if you like).
  const vk::PipelineCache pipelineCache() const { return *pipelineCache_; }

  /// Get the default descriptor pool (you can use your own if you like).
  const vk::DescriptorPool descriptorPool() const {
    auto makeDescPool = [this] {
      std::vector<vk::DescriptorPoolSize> poolSizes;
      poolSizes.emplace_back(vk::DescriptorType::eUniformBuffer, 128);
      poolSizes.emplace_back(vk::DescriptorType::eCombinedImageSampler, 128);
      poolSizes.emplace_back(vk::DescriptorType::eStorageBuffer, 128);

      // Create an arbitrary number of descriptors in a pool.
      // Allow the descriptors to be freed, possibly not optimal behaviour.
      vk::DescriptorPoolCreateInfo descriptorPoolInfo{};
      descriptorPoolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
      descriptorPoolInfo.maxSets = 256;
      descriptorPoolInfo.poolSizeCount = (uint32_t)poolSizes.size();
      descriptorPoolInfo.pPoolSizes = poolSizes.data();
      auto descPool = device_->createDescriptorPoolUnique(descriptorPoolInfo);
      auto result = *descPool;
      auto threadID = std::this_thread::get_id();
      {
        std::lock_guard<std::mutex> lockGuard(livePools_->first);
        livePools_->second.insert({threadID, std::move(descPool)});
      }
      std::weak_ptr<SyncDescriptors> weakLivePools{livePools_};
      thread_local auto cleanup = on_death([threadID, weakLivePools] {
        auto livePools = weakLivePools.lock();
        if (livePools)
          livePools->second.erase(threadID);
      });
      return result;
    };

    thread_local vk::DescriptorPool desc_pool = makeDescPool();
    return desc_pool;
  }

  /// Get the family index for the graphics queues.
  uint32_t graphicsQueueFamilyIndex() const { return graphicsQueueFamilyIndex_; }

  /// Get the family index for the compute queues.
  uint32_t computeQueueFamilyIndex() const { return computeQueueFamilyIndex_; }

  const vk::PhysicalDeviceMemoryProperties &memprops() const { return memprops_; }

  /// Clean up the framework satisfying the Vulkan verification layers.
  ~Framework() {
    if (device_) {
      device_->waitIdle();
      if (pipelineCache_) {
        pipelineCache_.reset();
      }
      std::lock_guard<std::mutex> lockGuard(livePools_->first);
      livePools_->second.clear();
      device_.reset();
    }

    if (instance_) {
      callback_.reset();
      instance_.reset();
    }
  }

  Framework &operator=(Framework &&rhs) = default;

  /// Returns true if the Framework has been built correctly.
  bool ok() const { return ok_; }

  static SynchronizedQueue getQueue(vk::Device device, uint32_t queueFamily, uint32_t queueIndex)
  {
    using Request = std::tuple<vk::Device, uint32_t, uint32_t>;
    static std::map<Request, SynchronizedQueue> allQueues;
    static std::mutex mtx;
    std::lock_guard<std::mutex> lg(mtx);
    auto it = allQueues.find({device, queueFamily, queueIndex});
    if (it == allQueues.end()) {
      it = allQueues
               .insert({{device, queueFamily, queueIndex},
                        {device.getQueue(queueFamily, queueIndex),
                         new std::mutex{}}})
               .first;
    }
    return it->second;
  }

private:
  vk::UniqueInstance instance_;
  vku::DebugCallback callback_;
  vk::UniqueDevice device_;
  //vk::DebugReportCallbackEXT callback_;
  vk::PhysicalDevice physical_device_;
  vk::UniquePipelineCache pipelineCache_;
  using SyncDescriptors = std::pair<std::mutex,std::map<std::thread::id, vk::UniqueDescriptorPool>>;
  std::shared_ptr<SyncDescriptors> livePools_;
  uint32_t graphicsQueueFamilyIndex_;
  uint32_t computeQueueFamilyIndex_;
  SynchronizedQueue graphicsQueue_;
  SynchronizedQueue computeQueue_;
  vk::PhysicalDeviceMemoryProperties memprops_;
  bool ok_ = false;
};

/// This class wraps a window, a surface and a swap chain for that surface.
class Window {
public:
  Window() {
  }

#ifndef VKU_NO_GLFW
  /// Construct a window, surface and swapchain using a GLFW window.
  Window(const vk::Instance &instance, const vk::Device &device, const vk::PhysicalDevice &physicalDevice, uint32_t graphicsQueueFamilyIndex, GLFWwindow *window) {
#ifdef VK_USE_PLATFORM_WIN32_KHR
    auto module = GetModuleHandle(nullptr);
    auto handle = glfwGetWin32Window(window);
    auto ci = vk::Win32SurfaceCreateInfoKHR{{}, module, handle};
    auto surface = instance.createWin32SurfaceKHR(ci);
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
    auto display = glfwGetX11Display();
    auto x11window = glfwGetX11Window(window);
    auto ci = vk::XlibSurfaceCreateInfoKHR{{}, display, x11window};
    auto surface = instance.createXlibSurfaceKHR(ci);
#endif
#ifdef VK_EXT_METAL_SURFACE_EXTENSION_NAME
    vk::SurfaceKHR surface;
    glfwCreateWindowSurface(instance, window,
	                        nullptr,
	                        reinterpret_cast<VkSurfaceKHR *>(&surface));
#endif
    init(instance, device, physicalDevice, graphicsQueueFamilyIndex, surface);
  }
#endif

  Window(const vk::Instance &instance, const vk::Device &device, const vk::PhysicalDevice &physicalDevice, uint32_t graphicsQueueFamilyIndex, vk::SurfaceKHR surface) {
    init(instance, device, physicalDevice, graphicsQueueFamilyIndex, surface);
  }

  Window(const Window &) = delete;
  Window(Window &&) = default;


  void init(const vk::Instance &instance, const vk::Device &device, const vk::PhysicalDevice &physicalDevice, uint32_t graphicsQueueFamilyIndex, vk::SurfaceKHR surface) {
    surface_ = vk::UniqueSurfaceKHR(surface, { instance });
    graphicsQueueFamilyIndex_ = graphicsQueueFamilyIndex;
    physicalDevice_ = physicalDevice;
    instance_ = instance;
    device_ = device;
    presentQueueFamily_ = 0;
    auto &pd = physicalDevice;
    auto qprops = pd.getQueueFamilyProperties();
    bool found = false;
    for (uint32_t qi = 0; qi != qprops.size(); ++qi) {
      auto &qprop = qprops[qi];
      if (pd.getSurfaceSupportKHR(qi, *surface_) && (qprop.queueFlags & vk::QueueFlagBits::eGraphics) == vk::QueueFlagBits::eGraphics) {
        presentQueueFamily_ = qi;
        found = true;
        break;
      }
    }

    if (!found) {
      std::cerr << "No Vulkan present queues found\n";
      return;
    }

    presentQueue_ = Framework::getQueue(device_, presentQueueFamily_, 0);

    auto fmts = pd.getSurfaceFormatsKHR(*surface_);
    swapchainImageFormat_ = fmts[0].format;
    swapchainColorSpace_ = fmts[0].colorSpace;
    if (fmts.size() == 1 && swapchainImageFormat_ == vk::Format::eUndefined) {
      swapchainImageFormat_ = vk::Format::eB8G8R8A8Unorm;
      swapchainColorSpace_ = vk::ColorSpaceKHR::eSrgbNonlinear;
    } else {
      for (auto &fmt : fmts) {
        if (fmt.format == vk::Format::eB8G8R8A8Unorm) {
          swapchainImageFormat_ = fmt.format;
          swapchainColorSpace_ = fmt.colorSpace;
        }
      }
    }

    createSwapchain();

    createImages();

    createDepthStencil();

    createRenderPass();

    createFrameBuffers();

    vk::SemaphoreCreateInfo sci;
    imageAcquireSemaphore_ = device.createSemaphoreUnique(sci);
    commandCompleteSemaphore_ = device.createSemaphoreUnique(sci);
    dynamicSemaphore_ = device.createSemaphoreUnique(sci);

    typedef vk::CommandPoolCreateFlagBits ccbits;

    vk::CommandPoolCreateInfo cpci{ccbits::eTransient |
                                       ccbits::eResetCommandBuffer,
                                   graphicsQueueFamilyIndex};
    commandPool_ = device.createCommandPoolUnique(cpci);

    // Create static draw buffers
    vk::CommandBufferAllocateInfo cbai{*commandPool_,
                                       vk::CommandBufferLevel::ePrimary,
                                       (uint32_t)framebuffers_.size()};
    staticDrawBuffers_ = device.allocateCommandBuffersUnique(cbai);
    dynamicDrawBuffers_ = device.allocateCommandBuffersUnique(cbai);

    // Create a set of fences to protect the command buffers from re-writing.
    for (int i = 0; i != staticDrawBuffers_.size(); ++i) {
      vk::FenceCreateInfo fci;
      fci.flags = vk::FenceCreateFlagBits::eSignaled;
      commandBufferFences_.emplace_back(device.createFence(fci));
    }

    for (int i = 0; i != staticDrawBuffers_.size(); ++i) {
      vk::CommandBuffer cb = *staticDrawBuffers_[i];
      vk::CommandBufferBeginInfo bi{};
      cb.begin(bi);
      cb.end();
    }

    ok_ = true;
  }

	/// Dump the capabilities of the physical device used by this window.
  void dumpCaps(std::ostream &os, vk::PhysicalDevice pd) const {
    os << "Surface formats\n";
    auto fmts = pd.getSurfaceFormatsKHR(*surface_);
    for (auto &fmt : fmts) {
      auto fmtstr = vk::to_string(fmt.format);
      auto cstr = vk::to_string(fmt.colorSpace);
      os << "format=" << fmtstr << " colorSpace=" << cstr << "\n";
    }

    os << "Present Modes\n";
    auto presentModes = pd.getSurfacePresentModesKHR(*surface_);
    for (auto pm : presentModes) {
      std::cout << vk::to_string(pm) << "\n";
    }
  }

  static void defaultRenderFunc(vk::CommandBuffer cb, int imageIndex, vk::RenderPassBeginInfo &rpbi) {
    vk::CommandBufferBeginInfo bi{};
    cb.begin(bi);
    cb.end();
  }

  typedef void (renderFunc_t)(vk::CommandBuffer cb, int imageIndex, vk::RenderPassBeginInfo &rpbi);

  /// Build a static draw buffer. This will be rendered after any dynamic
  /// content generated in draw()
  void setStaticCommands(const std::function<renderFunc_t> &func) {
    this->func = func;
    buildStaticCBs();
  }

  void buildStaticCBs() {
    if(func) {
      for (int i = 0; i != staticDrawBuffers_.size(); ++i) {
        vk::CommandBuffer cb = *staticDrawBuffers_[i];

        std::array<float, 4> clearColorValue{0.75f, 0.75f, 0.75f, 1};
        vk::ClearDepthStencilValue clearDepthValue{1.0f, 0};
        std::array<vk::ClearValue, 2> clearColours{
            vk::ClearValue{clearColorValue}, clearDepthValue};
        vk::RenderPassBeginInfo rpbi;
        rpbi.renderPass = *renderPass_;
        rpbi.framebuffer = *framebuffers_[i];
        rpbi.renderArea = vk::Rect2D{{0, 0}, {width_, height_}};
        rpbi.clearValueCount = (uint32_t)clearColours.size();
        rpbi.pClearValues = clearColours.data();

        func(cb, i, rpbi);
      }
    }
  }

  /// Queue the static command buffer for the next image in the swap chain. Optionally call a function to create a dynamic command buffer
  /// for uploading textures, changing uniforms etc.
  void draw(const vk::Device &device, SynchronizedQueue graphicsQueue, const std::function<void (vk::CommandBuffer cb, int imageIndex, vk::RenderPassBeginInfo &rpbi)> &dynamic = defaultRenderFunc) {
    static auto start = std::chrono::high_resolution_clock::now();
    auto time = std::chrono::high_resolution_clock::now();
    auto delta = time - start;
    start = time;
    // uncomment to get frame time.
    //std::cout << std::chrono::duration_cast<std::chrono::microseconds>(delta).count() << "us frame time\n";

    auto umax = std::numeric_limits<uint64_t>::max();
    uint32_t imageIndex = 0;
    auto acquired = device.acquireNextImageKHR(*swapchain_, umax, *imageAcquireSemaphore_, vk::Fence(), &imageIndex);
    if (acquired != vk::Result::eSuccess) {
      recreate();
      return;
    }
    vk::PipelineStageFlags waitStages = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    vk::Semaphore ccSema = *commandCompleteSemaphore_;
    vk::Semaphore iaSema = *imageAcquireSemaphore_;
    vk::Semaphore psSema = *dynamicSemaphore_;
    vk::CommandBuffer cb = *staticDrawBuffers_[imageIndex];
    vk::CommandBuffer pscb = *dynamicDrawBuffers_[imageIndex];

    vk::Fence cbFence = commandBufferFences_[imageIndex];
    device.waitForFences(cbFence, 1, umax);
    device.resetFences(cbFence);

    std::array<float, 4> clearColorValue{0.75f, 0.75f, 0.75f, 1};
    vk::ClearDepthStencilValue clearDepthValue{ 1.0f, 0 };
    std::array<vk::ClearValue, 2> clearColours{vk::ClearValue{clearColorValue}, clearDepthValue};
    vk::RenderPassBeginInfo rpbi;
    rpbi.renderPass = *renderPass_;
    rpbi.framebuffer = *framebuffers_[imageIndex];
    rpbi.renderArea = vk::Rect2D{{0, 0}, {width_, height_}};
    rpbi.clearValueCount = (uint32_t)clearColours.size();
    rpbi.pClearValues = clearColours.data();
    dynamic(pscb, imageIndex, rpbi);

    vk::SubmitInfo dynamicSubmit;
    dynamicSubmit.waitSemaphoreCount = 1;
    dynamicSubmit.pWaitSemaphores = &iaSema;
    dynamicSubmit.pWaitDstStageMask = &waitStages;
    dynamicSubmit.commandBufferCount = 1;
    dynamicSubmit.pCommandBuffers = &pscb;
    dynamicSubmit.signalSemaphoreCount = 1;
    dynamicSubmit.pSignalSemaphores = &psSema;

    vk::SubmitInfo staticSubmit{};
    staticSubmit.waitSemaphoreCount = 1;
    staticSubmit.pWaitSemaphores = &psSema;
    staticSubmit.pWaitDstStageMask = &waitStages;
    staticSubmit.commandBufferCount = 1;
    staticSubmit.pCommandBuffers = &cb;
    staticSubmit.signalSemaphoreCount = 1;
    staticSubmit.pSignalSemaphores = &ccSema;
    thread_local unsigned long long t = 0;
    thread_local int cnt = 0;
    auto t0 = rdtsc();
    graphicsQueue.submit({dynamicSubmit, staticSubmit}, cbFence);
    auto t1 = rdtsc();
    t += t1 - t0;
    if (++cnt % 100 == 0) {
      static std::mutex dmtx;
      std::lock_guard<std::mutex> lg(dmtx);
      std::cout << "Time: " << (t / cnt) << std::endl;
      cnt = 0;
      t = 0;
    }
    vk::PresentInfoKHR presentInfo;
    vk::SwapchainKHR swapchain = *swapchain_;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.swapchainCount = 1;
    presentInfo.pImageIndices = &imageIndex;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &ccSema;
    try {
      presentQueue_.presentKHR(presentInfo);
    } catch (const vk::OutOfDateKHRError &e) {
      recreate();
    }
  }

  /// Return the queue family index used to present the surface to the display.
  uint32_t presentQueueFamily() const { return presentQueueFamily_; }

  /// Get the queue used to submit graphics jobs
  SynchronizedQueue presentQueue() const {
    return presentQueue_;
  }

  /// Return true if this window was created sucessfully.
  bool ok() const { return ok_; }

  /// Return the renderpass used by this window.
  vk::RenderPass renderPass() const { return *renderPass_; }

  /// Return the frame buffers used by this window
  const std::vector<vk::UniqueFramebuffer> &framebuffers() const { return framebuffers_; }

  /// Destroy resources when shutting down.
  ~Window() {
    for (auto &iv : imageViews_) {
      device_.destroyImageView(iv);
    }
    for (auto &f : commandBufferFences_) {
      device_.destroyFence(f);
    }
    swapchain_ = vk::UniqueSwapchainKHR{};
  }
  Window &operator=(const Window &) = delete;
  Window &operator=(Window &&rhs) = default;

  /// Return the width of the display.
  uint32_t width() const { return width_; }

  /// Return the height of the display.
  uint32_t height() const { return height_; }

  /// Return the format of the back buffer.
  vk::Format swapchainImageFormat() const { return swapchainImageFormat_; }

  /// Return the colour space of the back buffer (Usually sRGB)
  vk::ColorSpaceKHR swapchainColorSpace() const { return swapchainColorSpace_; }

  /// Return the swapchain object
  const vk::SwapchainKHR swapchain() const { return *swapchain_; }

  /// Return the views of the swap chain images
  const std::vector<vk::ImageView> &imageViews() const { return imageViews_; }

  /// Return the swap chain images
  const std::vector<vk::Image> &images() const { return images_; }

  /// Return the static command buffers.
  const std::vector<vk::UniqueCommandBuffer> &commandBuffers() const { return staticDrawBuffers_; }

  /// Return the fences used to control the static buffers.
  const std::vector<vk::Fence> &commandBufferFences() const { return commandBufferFences_; }

  /// Return the semaphore signalled when an image is acquired.
  vk::Semaphore imageAcquireSemaphore() const { return *imageAcquireSemaphore_; }

  /// Return the semaphore signalled when the command buffers are finished.
  vk::Semaphore commandCompleteSemaphore() const { return *commandCompleteSemaphore_; }

  /// Return a defult command Pool to use to create new command buffers.
  vk::CommandPool commandPool() const { return *commandPool_; }

  /// Return the number of swap chain images.
  int numImageIndices() const { return (int)images_.size(); }

  /// Create a new swapchain and destroy the previous one if any.
  void createSwapchain() {
    auto pms = physicalDevice_.getSurfacePresentModesKHR(*surface_);
    vk::PresentModeKHR presentMode = pms[0];
    if (std::find(pms.begin(), pms.end(), vk::PresentModeKHR::eFifo) !=
        pms.end()) {
      presentMode = vk::PresentModeKHR::eFifo;
    } else {
      std::cerr << "No fifo mode available\n";
      return;
    }

    auto surfaceCaps = physicalDevice_.getSurfaceCapabilitiesKHR(*surface_);
    width_ = surfaceCaps.currentExtent.width;
    height_ = surfaceCaps.currentExtent.height;
    vk::SwapchainCreateInfoKHR swapinfo{};
    std::array<uint32_t, 2> queueFamilyIndices = {graphicsQueueFamilyIndex_,
                                                  presentQueueFamily_};
    bool sameQueues = queueFamilyIndices[0] == queueFamilyIndices[1];
    vk::SharingMode sharingMode = !sameQueues ? vk::SharingMode::eConcurrent
                                              : vk::SharingMode::eExclusive;
    swapinfo.imageExtent = surfaceCaps.currentExtent;
    swapinfo.surface = *surface_;
    swapinfo.minImageCount = surfaceCaps.minImageCount + 1;
    swapinfo.imageFormat = swapchainImageFormat_;
    swapinfo.imageColorSpace = swapchainColorSpace_;
    swapinfo.imageExtent = surfaceCaps.currentExtent;
    swapinfo.imageArrayLayers = 1;
    swapinfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
    swapinfo.imageSharingMode = sharingMode;
    swapinfo.queueFamilyIndexCount = !sameQueues ? 2 : 0;
    swapinfo.pQueueFamilyIndices = queueFamilyIndices.data();
    swapinfo.preTransform = surfaceCaps.currentTransform;
    ;
    swapinfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    swapinfo.presentMode = presentMode;
    swapinfo.clipped = 1;
    swapinfo.oldSwapchain = vk::SwapchainKHR{};
    swapchain_ = device_.createSwapchainKHRUnique(swapinfo);
  }

  void createImages() {
    images_ = device_.getSwapchainImagesKHR(*swapchain_);
    for (auto &iv : imageViews_) {
      device_.destroyImageView(iv);
    }
    imageViews_.clear();
    for (auto &img : images_) {
      vk::ImageViewCreateInfo ci{};
      ci.image = img;
      ci.viewType = vk::ImageViewType::e2D;
      ci.format = swapchainImageFormat_;
      ci.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
      imageViews_.emplace_back(device_.createImageView(ci));
    }
  }

  void createFrameBuffers() {
    framebuffers_.clear();
    for (int i = 0; i != imageViews_.size(); ++i) {
      vk::ImageView attachments[2] = {imageViews_[i],
                                      depthStencilImage_.imageView()};
      vk::FramebufferCreateInfo fbci{{},     *renderPass_, 2, attachments,
                                     width_, height_,      1};
      framebuffers_.push_back(device_.createFramebufferUnique(fbci));
    }
  }

  void createDepthStencil() {
    auto memprops = physicalDevice_.getMemoryProperties();
    depthStencilImage_ =
        vku::DepthStencilImage(device_, memprops, width_, height_);
  }

  void createRenderPass() { // Build the renderpass using two attachments,
                            // colour and depth/stencil.
    RenderpassMaker rpm;

    // The only colour attachment.
    rpm.attachmentBegin(swapchainImageFormat_);
    rpm.attachmentLoadOp(vk::AttachmentLoadOp::eClear);
    rpm.attachmentStoreOp(vk::AttachmentStoreOp::eStore);
    rpm.attachmentFinalLayout(vk::ImageLayout::ePresentSrcKHR);

    // The depth/stencil attachment.
    rpm.attachmentBegin(depthStencilImage_.format());
    rpm.attachmentLoadOp(vk::AttachmentLoadOp::eClear);
    rpm.attachmentStencilLoadOp(vk::AttachmentLoadOp::eDontCare);
    rpm.attachmentFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);

    // A subpass to render using the above two attachments.
    rpm.subpassBegin(vk::PipelineBindPoint::eGraphics);
    rpm.subpassColorAttachment(vk::ImageLayout::eColorAttachmentOptimal, 0);
    rpm.subpassDepthStencilAttachment(
        vk::ImageLayout::eDepthStencilAttachmentOptimal, 1);

    // A dependency to reset the layout of both attachments.
    rpm.dependencyBegin(VK_SUBPASS_EXTERNAL, 0);
    rpm.dependencySrcStageMask(
        vk::PipelineStageFlagBits::eColorAttachmentOutput);
    rpm.dependencyDstStageMask(
        vk::PipelineStageFlagBits::eColorAttachmentOutput);
    rpm.dependencyDstAccessMask(vk::AccessFlagBits::eColorAttachmentRead |
                                vk::AccessFlagBits::eColorAttachmentWrite);

    // Use the maker object to construct the vulkan object
    renderPass_ = rpm.createUnique(device_);
  }

  void recreate() {
    device_.waitForFences(commandBufferFences_, VK_TRUE,
                          std::numeric_limits<uint64_t>::max());
    createSwapchain();

    createImages();

    createDepthStencil();

    createFrameBuffers();

    buildStaticCBs();
  }

  vk::Device device() const { return device_; }

private:
  vk::Instance instance_;
  vk::PhysicalDevice physicalDevice_;
  uint32_t graphicsQueueFamilyIndex_;
  vk::UniqueSurfaceKHR surface_;
  vk::UniqueSwapchainKHR swapchain_;
  vk::UniqueRenderPass renderPass_;
  vk::UniqueSemaphore imageAcquireSemaphore_;
  vk::UniqueSemaphore commandCompleteSemaphore_;
  vk::UniqueSemaphore dynamicSemaphore_;
  vk::UniqueCommandPool commandPool_;

  std::vector<vk::ImageView> imageViews_;
  std::vector<vk::Image> images_;
  std::vector<vk::Fence> commandBufferFences_;
  std::vector<vk::UniqueFramebuffer> framebuffers_;
  std::vector<vk::UniqueCommandBuffer> staticDrawBuffers_;
  std::vector<vk::UniqueCommandBuffer> dynamicDrawBuffers_;
  /// \brief Function called to recreate the static buffers on window size
  /// change.
  std::function<renderFunc_t> func;

  vku::DepthStencilImage depthStencilImage_;

  uint32_t presentQueueFamily_ = 0;
  SynchronizedQueue presentQueue_;
  uint32_t width_;
  uint32_t height_;
  vk::Format swapchainImageFormat_ = vk::Format::eB8G8R8A8Unorm;
  vk::ColorSpaceKHR swapchainColorSpace_ = vk::ColorSpaceKHR::eSrgbNonlinear;
  vk::Device device_;
  bool ok_ = false;
};

} // namespace vku

#endif // VKU_FRAMEWORK_HPP
