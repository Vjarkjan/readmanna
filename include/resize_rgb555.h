#pragma once

#include <stdint.h>
#include <string.h>

namespace readmanna {

inline void rotated_row_layout(
    int source_width,
    int source_height,
    int source_y,
    int rotation,
    int* destination_index,
    int* destination_step
) {
    switch (rotation & 3) {
        case 1: // 90 degrees clockwise: rotated size is height x width.
            *destination_index = source_height - 1 - source_y;
            *destination_step = source_height;
            break;
        case 2:
            *destination_index =
                (source_height - 1 - source_y) * source_width +
                source_width - 1;
            *destination_step = -1;
            break;
        case 3: // 90 degrees counterclockwise: rotated size is height x width.
            *destination_index =
                (source_width - 1) * source_height + source_y;
            *destination_step = -source_height;
            break;
        default:
            *destination_index = source_y * source_width;
            *destination_step = 1;
            break;
    }
}

inline uint16_t bilinear_rgb555_fast(
    uint16_t top_left,
    uint16_t top_right,
    uint16_t bottom_left,
    uint16_t bottom_right,
    uint32_t fraction_x,
    uint32_t fraction_y
) {
    // Do the interpolation as two exact 1-D lerps without rounding the
    // horizontal intermediate.  This is algebraically identical to the old
    // four 2-D weights but needs only three multiplies per color channel.
    const int r00 = top_left & 31;
    const int r10 = top_right & 31;
    const int r01 = bottom_left & 31;
    const int r11 = bottom_right & 31;
    const int g00 = (top_left >> 5) & 31;
    const int g10 = (top_right >> 5) & 31;
    const int g01 = (bottom_left >> 5) & 31;
    const int g11 = (bottom_right >> 5) & 31;
    const int b00 = (top_left >> 10) & 31;
    const int b10 = (top_right >> 10) & 31;
    const int b01 = (bottom_left >> 10) & 31;
    const int b11 = (bottom_right >> 10) & 31;

    const int red_top = (r00 << 8) + (r10 - r00) * static_cast<int>(fraction_x);
    const int red_bottom = (r01 << 8) + (r11 - r01) * static_cast<int>(fraction_x);
    const int green_top = (g00 << 8) + (g10 - g00) * static_cast<int>(fraction_x);
    const int green_bottom = (g01 << 8) + (g11 - g01) * static_cast<int>(fraction_x);
    const int blue_top = (b00 << 8) + (b10 - b00) * static_cast<int>(fraction_x);
    const int blue_bottom = (b01 << 8) + (b11 - b01) * static_cast<int>(fraction_x);

    const int red = (
        (red_top << 8) +
        (red_bottom - red_top) * static_cast<int>(fraction_y) +
        32768
    ) >> 16;
    const int green = (
        (green_top << 8) +
        (green_bottom - green_top) * static_cast<int>(fraction_y) +
        32768
    ) >> 16;
    const int blue = (
        (blue_top << 8) +
        (blue_bottom - blue_top) * static_cast<int>(fraction_y) +
        32768
    ) >> 16;

    return static_cast<uint16_t>(0x8000 | red | (green << 5) | (blue << 10));
}

// Pixel-center mapping, calculated once per axis rather than per pixel.
struct ResizeSample {
    int first;
    int second;
    unsigned fraction;
};

inline void build_resize_axis(int source_extent, int target_extent, ResizeSample* samples) {
    for (int index = 0; index < target_extent; ++index) {
        int fixed = ((2 * index + 1) * source_extent * 128) / target_extent - 128;
        if (fixed < 0) fixed = 0;
        if (fixed > (source_extent - 1) * 256) fixed = (source_extent - 1) * 256;
        const int first = fixed >> 8;
        samples[index] = {first, first + (first + 1 < source_extent),
                          static_cast<unsigned>(fixed & 255)};
    }
}

// The caller supplies separate source/destination buffers and axis workspace.
// Run only on a zoom/page/rotation change; panning copies the finished rows.
inline void resize_rgb555(
    const uint16_t* source, int source_width, int source_height,
    uint16_t* destination, int width, int height, int rotation,
    ResizeSample* xs, ResizeSample* ys
) {
    const bool exact = source_width == width && source_height == height;
    if (exact && (rotation & 3) == 0) {
        memcpy(destination, source, width * height * sizeof(uint16_t));
        return;
    }
    if (!exact) {
        build_resize_axis(source_width, width, xs);
        build_resize_axis(source_height, height, ys);
    }
    for (int y = 0; y < height; ++y) {
        int offset, step;
        rotated_row_layout(width, height, y, rotation, &offset, &step);
        if (exact) {
            for (int x = 0; x < width; ++x, offset += step) {
                destination[offset] = source[y * width + x];
            }
        } else {
            const uint16_t* top = source + ys[y].first * source_width;
            const uint16_t* bottom = source + ys[y].second * source_width;
            for (int x = 0; x < width; ++x, offset += step) {
                destination[offset] = bilinear_rgb555_fast(
                    top[xs[x].first], top[xs[x].second],
                    bottom[xs[x].first], bottom[xs[x].second],
                    xs[x].fraction, ys[y].fraction);
            }
        }
    }
}

} // namespace readmanna
