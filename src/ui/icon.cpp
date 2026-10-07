#include "ui/icon.h"

#include <cstdio>

namespace {

float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

float Len(float x, float y) { return std::sqrt(x * x + y * y); }

float SdRoundBox(float px, float py, float bx, float by, float r) {
    float qx = std::fabs(px) - bx + r, qy = std::fabs(py) - by + r;
    return Len(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}

float SdSquircle(float px, float py, float bx, float by, float r) {
    float qx = std::fabs(px) - bx + r, qy = std::fabs(py) - by + r;
    float ax = std::max(qx, 0.0f), ay = std::max(qy, 0.0f);
    float l4 = std::sqrt(std::sqrt(ax * ax * ax * ax + ay * ay * ay * ay));
    return l4 + std::min(std::max(qx, qy), 0.0f) - r;
}

float SdBox(float px, float py, float bx, float by) {
    float dx = std::fabs(px) - bx, dy = std::fabs(py) - by;
    return Len(std::max(dx, 0.0f), std::max(dy, 0.0f)) + std::min(std::max(dx, dy), 0.0f);
}

float SdTriangle(float px, float py, float x0, float y0, float x1, float y1, float x2, float y2) {
    float e0x = x1 - x0, e0y = y1 - y0, e1x = x2 - x1, e1y = y2 - y1, e2x = x0 - x2, e2y = y0 - y2;
    float v0x = px - x0, v0y = py - y0, v1x = px - x1, v1y = py - y1, v2x = px - x2, v2y = py - y2;
    auto proj = [](float vx, float vy, float ex, float ey, float& ox, float& oy) {
        float h = Clamp01((vx * ex + vy * ey) / (ex * ex + ey * ey));
        ox = vx - ex * h;
        oy = vy - ey * h;
    };
    float p0x, p0y, p1x, p1y, p2x, p2y;
    proj(v0x, v0y, e0x, e0y, p0x, p0y);
    proj(v1x, v1y, e1x, e1y, p1x, p1y);
    proj(v2x, v2y, e2x, e2y, p2x, p2y);
    float s = (e0x * e2y - e0y * e2x) < 0 ? -1.0f : 1.0f;
    float dA = p0x * p0x + p0y * p0y, sA = s * (v0x * e0y - v0y * e0x);
    float dB = p1x * p1x + p1y * p1y, sB = s * (v1x * e1y - v1y * e1x);
    float dC = p2x * p2x + p2y * p2y, sC = s * (v2x * e2y - v2y * e2x);
    float d = std::min({dA, dB, dC});
    float sg = std::min({sA, sB, sC});
    return -std::sqrt(d) * (sg < 0 ? -1.0f : 1.0f);
}

float Glyph(float gx, float gy) {
    float screen = std::fabs(SdRoundBox(gx, gy + 0.18f, 0.92f, 0.62f, 0.16f)) - 0.085f;
    float cut = SdBox(gx, gy - 0.55f, 0.40f, 0.30f);
    screen = std::max(screen, -cut);
    float tri = SdTriangle(gx, gy, 0.0f, 0.16f, -0.46f, 0.80f, 0.46f, 0.80f) - 0.04f;
    return std::min(screen, tri);
}

}  // namespace

std::vector<uint32_t> RenderIconPixels(int size) {
    std::vector<uint32_t> px(size_t(size) * size);
    const int ss = 4;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    float qx = ((float(x) + (float(sx) + 0.5f) / ss) / float(size) - 0.5f) * 2.0f;
                    float qy = ((float(y) + (float(sy) + 0.5f) / ss) / float(size) - 0.5f) * 2.0f;
                    float dTile = SdSquircle(qx, qy, 0.92f, 0.92f, 0.62f);
                    if (dTile >= 0) continue;
                    float t = Clamp01((qx + qy) * 0.25f + 0.5f);
                    float cr = 0.26f + (0.60f - 0.26f) * t;
                    float cg = 0.52f + (0.32f - 0.52f) * t;
                    float cb = 1.00f;
                    float hl = Clamp01((0.1f - qy) / 1.0f) * 0.20f;
                    float rim = std::exp(dTile / 0.07f) * 0.35f;
                    cr += hl + rim;
                    cg += hl + rim;
                    cb += hl + rim;
                    float dg = Glyph(qx / 0.60f, qy / 0.60f);
                    if (dg < 0) {
                        cr = cg = cb = 1.0f;
                    }
                    r += std::min(cr, 1.0f);
                    g += std::min(cg, 1.0f);
                    b += std::min(cb, 1.0f);
                    a += 1.0f;
                }
            }
            float n = float(ss * ss);
            // straight (non-premultiplied) alpha for icon bitmaps
            float alpha = a / n;
            float cr = a > 0 ? r / a : 0, cg = a > 0 ? g / a : 0, cb = a > 0 ? b / a : 0;
            uint32_t A = uint32_t(std::lround(alpha * 255.0f));
            uint32_t R = uint32_t(std::lround(cr * 255.0f)), G = uint32_t(std::lround(cg * 255.0f)),
                     B = uint32_t(std::lround(cb * 255.0f));
            px[size_t(y) * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
        }
    }
    return px;
}

HICON CreateAppIcon(int size) {
    auto px = RenderIconPixels(size);
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = size;
    bi.bV5Height = -size;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    HDC dc = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    std::memcpy(bits, px.data(), px.size() * 4);
    std::vector<uint8_t> zero(size_t((size + 15) / 16 * 2) * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, zero.data());
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

bool WriteIconFile(const std::wstring& path) {
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    const int count = int(sizeof(sizes) / sizeof(sizes[0]));
    std::vector<std::vector<uint8_t>> images;
    for (int s : sizes) {
        auto px = RenderIconPixels(s);
        std::vector<uint8_t> img;
        BITMAPINFOHEADER bh{};
        bh.biSize = sizeof(bh);
        bh.biWidth = s;
        bh.biHeight = s * 2;
        bh.biPlanes = 1;
        bh.biBitCount = 32;
        bh.biCompression = BI_RGB;
        int maskStride = ((s + 31) / 32) * 4;
        bh.biSizeImage = DWORD(s * s * 4 + maskStride * s);
        img.insert(img.end(), reinterpret_cast<uint8_t*>(&bh), reinterpret_cast<uint8_t*>(&bh) + sizeof(bh));
        for (int y = s - 1; y >= 0; --y) {  // bottom-up
            const uint8_t* row = reinterpret_cast<const uint8_t*>(&px[size_t(y) * s]);
            img.insert(img.end(), row, row + s * 4);
        }
        img.insert(img.end(), size_t(maskStride) * s, 0);
        images.push_back(std::move(img));
    }
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return false;
    auto put16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
    auto put32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
    put16(0);
    put16(1);
    put16(uint16_t(count));
    uint32_t offset = 6 + 16 * uint32_t(count);
    for (int i = 0; i < count; ++i) {
        uint8_t wh = uint8_t(sizes[i] >= 256 ? 0 : sizes[i]);
        fputc(wh, f);
        fputc(wh, f);
        fputc(0, f);
        fputc(0, f);
        put16(1);
        put16(32);
        put32(uint32_t(images[i].size()));
        put32(offset);
        offset += uint32_t(images[i].size());
    }
    for (auto& img : images) fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    return true;
}
