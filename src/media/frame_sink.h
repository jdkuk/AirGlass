// Interface between the video decoder and the renderer.
#pragma once

#include "common.h"

struct ID3D11Device;
struct ID3D11Texture2D;

struct VideoColor {
    int matrix = 709;  // 601, 709 or 2020
    bool fullRange = false;
};

class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual ID3D11Device* GpuDevice() = 0;
    virtual std::recursive_mutex& GpuLock() = 0;
    // A decoded hardware frame: slice `index` of a (texture-array) NV12 texture.
    virtual void PresentHwFrame(ID3D11Texture2D* tex, unsigned index, int width, int height,
                                const VideoColor& color) = 0;
    // A decoded software frame: YUV 4:2:0 planar (3 planes) or NV12 (2 planes).
    virtual void PresentSwFrame(const uint8_t* const planes[3], const int strides[3], bool nv12, int width,
                                int height, const VideoColor& color) = 0;
};
