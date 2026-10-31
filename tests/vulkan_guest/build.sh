#!/bin/sh
# Builds vkclear.exe with clang + lld-link (no Windows SDK needed: the program is freestanding).
set -e
LLVM=$(ls -d /usr/lib/llvm-*/bin | tail -1)
PATH="$LLVM:$PATH"
cd "$(dirname "$0")"
mkdir -p inc && ln -sfn /usr/include/vulkan inc/vulkan && ln -sfn /usr/include/vk_video inc/vk_video
mk() { name=$1; dll=$2; shift 2; printf 'LIBRARY %s\nEXPORTS\n' "$dll" > "$name.def"; for f in "$@"; do echo "$f" >> "$name.def"; done
       llvm-dlltool -m i386:x86-64 -d "$name.def" -l "$name.lib"; }
mk kernel32 KERNEL32.dll GetModuleHandleA GetStdHandle WriteFile GetCommandLineA ExitProcess
mk user32 USER32.dll RegisterClassA CreateWindowExA ShowWindow PeekMessageA TranslateMessage DispatchMessageA DefWindowProcA
mk vulkan-1 vulkan-1.dll vkCreateInstance vkEnumeratePhysicalDevices vkGetPhysicalDeviceProperties vkGetPhysicalDeviceQueueFamilyProperties \
   vkCreateWin32SurfaceKHR vkGetPhysicalDeviceSurfaceSupportKHR vkGetPhysicalDeviceSurfaceFormatsKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR \
   vkCreateDevice vkGetDeviceQueue vkCreateSwapchainKHR vkGetSwapchainImagesKHR vkCreateCommandPool vkAllocateCommandBuffers vkCreateSemaphore \
   vkCreateFence vkWaitForFences vkResetFences vkAcquireNextImageKHR vkBeginCommandBuffer vkEndCommandBuffer vkResetCommandBuffer \
   vkCmdPipelineBarrier vkCmdClearColorImage vkQueueSubmit vkQueuePresentKHR vkDeviceWaitIdle
clang --target=x86_64-pc-windows-msvc -ffreestanding -fno-builtin -Iinc -fno-stack-protector -mno-stack-arg-probe -O0 -c vkclear.c -o vkclear.obj
lld-link /subsystem:console /entry:start /nodefaultlib /fixed:no /out:vkclear.exe vkclear.obj kernel32.lib user32.lib vulkan-1.lib
rm -rf *.def *.lib *.obj inc
echo built vkclear.exe
