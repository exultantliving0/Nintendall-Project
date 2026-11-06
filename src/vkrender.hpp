#pragma once
#include <SDL.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#define VK_NO_PROTOTYPES
#include "libretro.h"
#include "libretro_vulkan.h"

// Vulkan rendering for libretro cores that draw with Vulkan (N64 ParaLLEl, Dolphin, Azahar, Cemu),
// the Vulkan counterpart of HwRender (OpenGL).
//
// Nintendall creates the Vulkan instance and device (letting the core choose what it needs through
// libretro's negotiation interface), and the core draws into its own images. Each frame the core
// hands over its image (set_image); we shrink it on the GPU to the size it's shown at (a blit into
// our own BGRA8 image), copy that into memory, and pass it to the normal video path, like OpenGL.
// The copy finishes in the background, so what's returned is the previous frame.
class VkRender {
public:
    ~VkRender() { destroy(); }

    // Creates the instance and device. `negotiation` may be null (the core didn't send one).
    bool create(const retro_hw_render_context_negotiation_interface_vulkan* negotiation, std::string& error);
    void destroy();
    bool active() const { return device_ != VK_NULL_HANDLE; }
    // What the core gets from GET_HW_RENDER_INTERFACE.
    const retro_hw_render_interface_vulkan* interface() const { return &iface_; }
    // GPU name, for the log.
    const std::string& deviceName() const { return deviceName_; }
    // The size of the (hidden) surface window: Dolphin draws its frames at that size.
    void setSurfaceSize(unsigned w, unsigned h) {
        if (window_ && w && h) SDL_SetWindowSize(window_, int(w), int(h));
    }

    // Copies the image the core set this frame (w x h) into memory as XRGB8888, shrunk to fit
    // within maxW x maxH (0 = no limit). Returns the previous frame (or nullptr if none is ready).
    const uint32_t* readFrame(unsigned w, unsigned h, unsigned maxW, unsigned maxH, unsigned& outW, unsigned& outH);
    // A duplicate frame: the copy still in flight, if any.
    const uint32_t* takePending(unsigned& outW, unsigned& outH);

private:
    static constexpr int kSlots = 2;  // frames in flight (= sync indices the core sees)
    struct Slot {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;  // the shrunk copy
        VkDeviceMemory imageMem = VK_NULL_HANDLE;
        VkFormat imageFormat = VK_FORMAT_UNDEFINED;
        unsigned imageW = 0, imageH = 0;
        VkBuffer buffer = VK_NULL_HANDLE;  // where the GPU writes the pixels for us to read
        VkDeviceMemory bufferMem = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize bufferSize = 0;
        bool coherent = true;
        bool pending = false;  // submitted, not read yet
        unsigned w = 0, h = 0;
    };

    bool loadInstanceFunctions();
    bool loadDeviceFunctions();
    bool pickGpu(VkPhysicalDevice& gpu, uint32_t& family);
    bool createDefaultDevice(VkPhysicalDevice gpu, uint32_t family);
    bool createResources();
    void freeSlotImage(Slot& s);
    void freeSlotBuffer(Slot& s);
    bool ensureSlot(Slot& s, unsigned w, unsigned h, VkFormat format);
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice, bool* got = nullptr) const;
    const uint32_t* collect(Slot& s, unsigned& outW, unsigned& outH);
    void waitSlot(Slot& s);

    // The interface's callbacks (handle = this).
    static void cbSetImage(void* h, const retro_vulkan_image* image, uint32_t numSemaphores, const VkSemaphore* semaphores,
                           uint32_t srcQueueFamily);
    static uint32_t cbGetSyncIndex(void* h);
    static uint32_t cbGetSyncIndexMask(void* h);
    static void cbSetCommandBuffers(void* h, uint32_t num, const VkCommandBuffer* cmd);
    static void cbWaitSyncIndex(void* h);
    static void cbLockQueue(void* h);
    static void cbUnlockQueue(void* h);
    static void cbSetSignalSemaphore(void* h, VkSemaphore semaphore);
    static VkInstance createInstanceWrapper(void* opaque, const VkInstanceCreateInfo* info);
    static VkDevice createDeviceWrapper(VkPhysicalDevice gpu, void* opaque, const VkDeviceCreateInfo* info);

    // Vulkan functions (loaded at run time: there's no link-time dependency on Vulkan).
    struct Fn {
        PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
        PFN_vkCreateInstance CreateInstance = nullptr;
        PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
        PFN_vkDestroyInstance DestroyInstance = nullptr;
        PFN_vkDestroySurfaceKHR DestroySurfaceKHR = nullptr;
        PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
        PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties = nullptr;
        PFN_vkCreateDevice CreateDevice = nullptr;
        PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
        PFN_vkDestroyDevice DestroyDevice = nullptr;
        PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
        PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
        PFN_vkCreateCommandPool CreateCommandPool = nullptr;
        PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
        PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
        PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
        PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
        PFN_vkCmdBlitImage CmdBlitImage = nullptr;
        PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
        PFN_vkQueueSubmit QueueSubmit = nullptr;
        PFN_vkCreateFence CreateFence = nullptr;
        PFN_vkDestroyFence DestroyFence = nullptr;
        PFN_vkWaitForFences WaitForFences = nullptr;
        PFN_vkResetFences ResetFences = nullptr;
        PFN_vkCreateImage CreateImage = nullptr;
        PFN_vkDestroyImage DestroyImage = nullptr;
        PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
        PFN_vkBindImageMemory BindImageMemory = nullptr;
        PFN_vkCreateBuffer CreateBuffer = nullptr;
        PFN_vkDestroyBuffer DestroyBuffer = nullptr;
        PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
        PFN_vkBindBufferMemory BindBufferMemory = nullptr;
        PFN_vkAllocateMemory AllocateMemory = nullptr;
        PFN_vkFreeMemory FreeMemory = nullptr;
        PFN_vkMapMemory MapMemory = nullptr;
        PFN_vkUnmapMemory UnmapMemory = nullptr;
        PFN_vkInvalidateMappedMemoryRanges InvalidateMappedMemoryRanges = nullptr;
    } vk_;

    const retro_hw_render_context_negotiation_interface_vulkan* negotiation_ = nullptr;
    bool coreMadeDevice_ = false;  // the core's create_device ran (so destroy_device must be called)
    // A hidden window and its Vulkan surface: cores like Dolphin only present frames when they
    // get a real surface (as in RetroArch), even though we never show it.
    SDL_Window* window_ = nullptr;
    std::vector<const char*> windowExtensions_;  // instance extensions the surface needs
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice gpu_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    retro_hw_render_interface_vulkan iface_{};
    std::string deviceName_;
    std::mutex queueLock_;

    Slot slots_[kSlots];
    uint32_t frame_ = 0;  // sync index = frame_ % kSlots

    // What the core set for this frame.
    retro_vulkan_image image_{};
    bool haveImage_ = false;
    uint32_t srcQueueFamily_ = VK_QUEUE_FAMILY_IGNORED;
    std::vector<VkSemaphore> waitSemaphores_;
    std::vector<VkCommandBuffer> coreCommands_;
    VkSemaphore signalSemaphore_ = VK_NULL_HANDLE;

    std::vector<uint32_t> pixels_;
};
