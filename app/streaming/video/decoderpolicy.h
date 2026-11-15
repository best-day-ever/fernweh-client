#pragma once

#include <Limelight.h>

// Prefer exact-profile hardware decoding. Preserve an exact-format software
// fallback for supported H.264 and HEVC profiles; never substitute their format.
// The Host encoder (x264, NVENC or VideoToolbox) does not define decoder support.
inline constexpr bool plankSoftwareDecodingAllowed(int videoFormat)
{
    switch (videoFormat) {
    case VIDEO_FORMAT_H264:
    case VIDEO_FORMAT_H264_HIGH8_422:
    case VIDEO_FORMAT_H264_HIGH10_422:
    case VIDEO_FORMAT_H264_HIGH8_444:
    case VIDEO_FORMAT_H264_HIGH10_444:
    case VIDEO_FORMAT_H265:
    case VIDEO_FORMAT_H265_MAIN10:
    case VIDEO_FORMAT_H265_REXT8_444:
    case VIDEO_FORMAT_H265_REXT10_444:
        return true;
    default:
        return false;
    }
}
