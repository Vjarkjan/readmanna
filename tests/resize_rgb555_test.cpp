#include "resize_rgb555.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <vector>

// Independent floating-point pixel-center reference. The reader's 8-bit
// fractions may differ by at most one RGB555 step from this reference.
static void check(int sw, int sh, int w, int h) {
    std::vector<uint16_t> source(sw * sh);
    uint32_t random = 7;
    for (auto& pixel : source) {
        random = random * 1664525 + 1013904223;
        pixel = 0x8000 | (random & 0x7fff);
    }
    std::vector<readmanna::ResizeSample> xs(w), ys(h);
    for (int rotation = 0; rotation < 4; ++rotation) {
        std::vector<uint16_t> output(w * h + 2, 0x1234);
        readmanna::resize_rgb555(source.data(), sw, sh, output.data() + 1,
                                w, h, rotation, xs.data(), ys.data());
        assert(output.front() == 0x1234 && output.back() == 0x1234);
        for (int y = 0; y < h; ++y) {
            double sy = std::max(0., std::min(double(sh - 1), (y + .5) * sh / h - .5));
            int y0 = int(sy), y1 = std::min(y0 + 1, sh - 1);
            double fy = sy - y0;
            for (int x = 0; x < w; ++x) {
                double sx = std::max(0., std::min(double(sw - 1), (x + .5) * sw / w - .5));
                int x0 = int(sx), x1 = std::min(x0 + 1, sw - 1);
                double fx = sx - x0;
                int rx = x, ry = y, stride = w;
                if (rotation == 1) { rx = h - 1 - y; ry = x; stride = h; }
                if (rotation == 2) { rx = w - 1 - x; ry = h - 1 - y; }
                if (rotation == 3) { rx = y; ry = w - 1 - x; stride = h; }
                uint16_t actual = output[1 + ry * stride + rx];
                assert(actual & 0x8000);
                if (sw == w && sh == h) assert(actual == source[y * w + x]);
                for (int shift : {0, 5, 10}) {
                    auto channel = [&](int px, int py) { return (source[py * sw + px] >> shift) & 31; };
                    double expected = channel(x0, y0) * (1 - fx) * (1 - fy)
                        + channel(x1, y0) * fx * (1 - fy)
                        + channel(x0, y1) * (1 - fx) * fy
                        + channel(x1, y1) * fx * fy;
                    assert(std::abs(int((actual >> shift) & 31) - int(std::lround(expected))) <= 1);
                }
            }
        }
    }
}

int main() {
    check(256, 384, 256, 384); // Preview, all rotations.
    check(480, 720, 320, 480);
    check(480, 720, 384, 576);
    check(480, 720, 480, 720);
    check(512, 768, 512, 768);
    check(960, 1440, 640, 960);
    check(960, 1440, 768, 1152);
    check(1, 1, 3, 7); // Clamped boundaries and degenerate dimensions.
    check(7, 9, 1, 1);
    check(3, 7, 8, 12);
}
