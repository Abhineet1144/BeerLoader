#define _GNU_SOURCE
#include "bd12_formats.h"

VkFormat bd12_dxgi_to_vk(uint32_t f)
{
    switch (f) {
    case 1: case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 3: return VK_FORMAT_R32G32B32A32_UINT;
    case 4: return VK_FORMAT_R32G32B32A32_SINT;
    case 5: case 6: return VK_FORMAT_R32G32B32_SFLOAT;
    case 7: return VK_FORMAT_R32G32B32_UINT;
    case 8: return VK_FORMAT_R32G32B32_SINT;
    case 9: case 10: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 11: return VK_FORMAT_R16G16B16A16_UNORM;
    case 12: return VK_FORMAT_R16G16B16A16_UINT;
    case 13: return VK_FORMAT_R16G16B16A16_SNORM;
    case 14: return VK_FORMAT_R16G16B16A16_SINT;
    case 15: case 16: return VK_FORMAT_R32G32_SFLOAT;
    case 17: return VK_FORMAT_R32G32_UINT;
    case 18: return VK_FORMAT_R32G32_SINT;
    case 19: case 20: case 21: case 22: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 23: case 24: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 25: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 26: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 27: case 28: return VK_FORMAT_R8G8B8A8_UNORM;
    case 29: return VK_FORMAT_R8G8B8A8_SRGB;
    case 30: return VK_FORMAT_R8G8B8A8_UINT;
    case 31: return VK_FORMAT_R8G8B8A8_SNORM;
    case 32: return VK_FORMAT_R8G8B8A8_SINT;
    case 33: case 34: return VK_FORMAT_R16G16_SFLOAT;
    case 35: return VK_FORMAT_R16G16_UNORM;
    case 36: return VK_FORMAT_R16G16_UINT;
    case 37: return VK_FORMAT_R16G16_SNORM;
    case 38: return VK_FORMAT_R16G16_SINT;
    case 39: case 41: return VK_FORMAT_R32_SFLOAT;
    case 40: return VK_FORMAT_D32_SFLOAT;
    case 42: return VK_FORMAT_R32_UINT;
    case 43: return VK_FORMAT_R32_SINT;
    case 44: case 45: case 46: case 47: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 48: case 49: return VK_FORMAT_R8G8_UNORM;
    case 50: return VK_FORMAT_R8G8_UINT;
    case 51: return VK_FORMAT_R8G8_SNORM;
    case 52: return VK_FORMAT_R8G8_SINT;
    case 53: case 54: return VK_FORMAT_R16_SFLOAT;
    case 55: return VK_FORMAT_D16_UNORM;
    case 56: return VK_FORMAT_R16_UNORM;
    case 57: return VK_FORMAT_R16_UINT;
    case 58: return VK_FORMAT_R16_SNORM;
    case 59: return VK_FORMAT_R16_SINT;
    case 60: case 61: return VK_FORMAT_R8_UNORM;
    case 62: return VK_FORMAT_R8_UINT;
    case 63: return VK_FORMAT_R8_SNORM;
    case 64: return VK_FORMAT_R8_SINT;
    case 65: return VK_FORMAT_R8_UNORM;
    case 67: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 70: case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 72: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 73: case 74: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 75: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 76: case 77: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 78: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 79: case 80: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 81: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 82: case 83: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 84: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 85: return VK_FORMAT_R5G6B5_UNORM_PACK16;
    case 86: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
    case 87: case 88: case 90: case 92: return VK_FORMAT_B8G8R8A8_UNORM;
    case 91: case 93: return VK_FORMAT_B8G8R8A8_SRGB;
    case 94: case 95: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 96: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case 97: case 98: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 99: return VK_FORMAT_BC7_SRGB_BLOCK;
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat bd12_dxgi_resource_to_vk(uint32_t f, int allow_depth_stencil)
{
    if (allow_depth_stencil) {
        switch (f) {
        case 19: case 20: case 21: case 22: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case 39: case 40: case 41: return VK_FORMAT_D32_SFLOAT;
        case 44: case 45: case 46: case 47: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        case 53: case 55: case 56: return VK_FORMAT_D16_UNORM;
        default: break;
        }
    }
    return bd12_dxgi_to_vk(f);
}

uint32_t bd12_dxgi_bytes(uint32_t f)
{
    switch (f) {
    case 1: case 2: case 3: case 4: return 16;
    case 5: case 6: case 7: case 8: return 12;
    case 9: case 10: case 11: case 12: case 13: case 14:
    case 15: case 16: case 17: case 18:
    case 19: case 20: case 21: case 22: return 8;
    case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30:
    case 31: case 32: case 33: case 34: case 35: case 36: case 37: case 38:
    case 39: case 40: case 41: case 42: case 43: case 44: case 45: case 46:
    case 47: case 67: case 87: case 88: case 89: case 90: case 91: case 92:
    case 93: return 4;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    case 56: case 57: case 58: case 59: case 85: case 86: return 2;
    case 60: case 61: case 62: case 63: case 64: case 65: return 1;
    case 70: case 71: case 72: case 79: case 80: case 81: return 8;
    case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83:
    case 84: case 94: case 95: case 96: case 97: case 98: case 99: return 16;
    default: return 0;
    }
}

int bd12_dxgi_is_compressed(uint32_t f)
{
    return (f >= 70 && f <= 84) || (f >= 94 && f <= 99);
}

int bd12_dxgi_reads_stencil(uint32_t f)
{
    return f == 22 || f == 47;
}
