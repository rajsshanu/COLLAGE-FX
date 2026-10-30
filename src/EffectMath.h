// Shared CPU / CUDA pixel math for Mixed Media OFX.
//
// This header is compiled by MSVC for the CPU fallback and by NVRTC for the
// CUDA PTX. Keep it self-contained: no STL, no allocation, no host-only C++.
#pragma once

#if defined(__CUDACC__) || defined(__CUDACC_RTC__)
#define MM_HD __device__ __forceinline__
#else
#include <math.h>
#define MM_HD inline
#endif

#define MM_PI 3.14159265358979323846f
#define MM_TWO_PI 6.28318530717958647692f
#define MM_MAX_PAPER_PIECES 1600

struct Vec2
{
    float x, y;
};

struct Vec3
{
    float x, y, z;
};

struct Vec4
{
    float x, y, z, w;
};

// ---------------------------------------------------------------------------
// Small scalar helpers
// ---------------------------------------------------------------------------

MM_HD float mm_clamp(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

MM_HD float mm_saturate(float v)
{
    return mm_clamp(v, 0.0f, 1.0f);
}

MM_HD float mm_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

MM_HD float mm_min(float a, float b) { return a < b ? a : b; }
MM_HD float mm_max(float a, float b) { return a > b ? a : b; }
MM_HD float mm_abs(float a) { return a < 0.0f ? -a : a; }

MM_HD Vec2 mm_v2(float x, float y)
{
    Vec2 r;
    r.x = x;
    r.y = y;
    return r;
}

MM_HD Vec3 mm_v3(float x, float y, float z)
{
    Vec3 r;
    r.x = x;
    r.y = y;
    r.z = z;
    return r;
}

MM_HD Vec4 mm_v4(float x, float y, float z, float w)
{
    Vec4 r;
    r.x = x;
    r.y = y;
    r.z = z;
    r.w = w;
    return r;
}

MM_HD Vec3 mm_add(Vec3 a, Vec3 b) { return mm_v3(a.x + b.x, a.y + b.y, a.z + b.z); }
MM_HD Vec3 mm_sub(Vec3 a, Vec3 b) { return mm_v3(a.x - b.x, a.y - b.y, a.z - b.z); }
MM_HD Vec3 mm_mul(Vec3 a, Vec3 b) { return mm_v3(a.x * b.x, a.y * b.y, a.z * b.z); }
MM_HD Vec3 mm_scale(Vec3 a, float s) { return mm_v3(a.x * s, a.y * s, a.z * s); }
MM_HD Vec3 mm_mix(Vec3 a, Vec3 b, float t) { return mm_add(a, mm_scale(mm_sub(b, a), t)); }

MM_HD float mm_luma(Vec3 c)
{
    return c.x * 0.2126f + c.y * 0.7152f + c.z * 0.0722f;
}

MM_HD float mm_smoothstep(float edge0, float edge1, float x)
{
    if (edge1 <= edge0)
        return x >= edge1 ? 1.0f : 0.0f;
    float t = mm_saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

MM_HD float mm_step_aa(float edge, float x)
{
    // A sub-pixel anti-aliased step. x > edge is inside.
    return mm_saturate(0.5f + (x - edge) * 1.25f);
}

MM_HD float mm_wrap_angle(float radians)
{
    float a = radians;
    while (a > MM_PI)
        a -= MM_TWO_PI;
    while (a < -MM_PI)
        a += MM_TWO_PI;
    return a;
}

// ---------------------------------------------------------------------------
// Hashing / noise
// ---------------------------------------------------------------------------

MM_HD unsigned int mm_hash_u32(unsigned int x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

MM_HD float mm_hash_cell(int x, int y, unsigned int seed, unsigned int salt)
{
    unsigned int h = (unsigned int)x * 0x8da6b343u;
    h ^= (unsigned int)y * 0xd8163841u;
    h ^= seed * 0xcb1ab31fu;
    h ^= salt * 0x9e3779b9u;
    h = mm_hash_u32(h);
    return (float)(h & 0x00ffffffu) * (1.0f / 16777216.0f);
}

MM_HD float mm_hash_float(unsigned int a, unsigned int b, unsigned int salt)
{
    unsigned int h = a * 0x8da6b343u;
    h ^= b * 0xd8163841u;
    h ^= salt * 0x9e3779b9u;
    h = mm_hash_u32(h);
    return (float)(h & 0x00ffffffu) * (1.0f / 16777216.0f);
}

MM_HD float mm_value_noise(float x, float y, unsigned int seed)
{
    float fx = floorf(x);
    float fy = floorf(y);
    int ix = (int)fx;
    int iy = (int)fy;
    float tx = x - fx;
    float ty = y - fy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);

    float a = mm_hash_cell(ix, iy, seed, 0x11u);
    float b = mm_hash_cell(ix + 1, iy, seed, 0x11u);
    float c = mm_hash_cell(ix, iy + 1, seed, 0x11u);
    float d = mm_hash_cell(ix + 1, iy + 1, seed, 0x11u);
    return mm_lerp(mm_lerp(a, b, tx), mm_lerp(c, d, tx), ty);
}

MM_HD float mm_fbm2(float x, float y, unsigned int seed, int octaves)
{
    float sum = 0.0f;
    float amp = 0.5f;
    float total = 0.0f;
    int i;
    for (i = 0; i < octaves; ++i)
    {
        sum += mm_value_noise(x, y, seed + (unsigned int)i * 0x9e3779b9u) * amp;
        total += amp;
        x = x * 2.03f + 17.1f;
        y = y * 2.01f - 9.7f;
        amp *= 0.5f;
    }
    return total > 0.0f ? sum / total : 0.5f;
}

// Fine fibrous noise used for paper and torn edges. The x/y scales differ so
// the texture has a paper-like directional grain instead of round blobs.
MM_HD float mm_fiber_noise(float x, float y, unsigned int seed)
{
    float a = mm_value_noise(x * 0.32f, y * 2.8f, seed);
    float b = mm_value_noise(x * 2.9f, y * 0.44f, seed ^ 0x5bd1e995u);
    float c = mm_value_noise(x * 1.35f + 13.0f, y * 1.21f - 7.0f, seed ^ 0x27d4eb2fu);
    return a * 0.48f + b * 0.30f + c * 0.22f;
}

// ---------------------------------------------------------------------------
// Image sampling
// ---------------------------------------------------------------------------

// u/v are in source-local pixel-centre coordinates: (0.5, 0.5) is the centre
// of the first pixel. Reads with clamp-to-edge.
MM_HD Vec4 mm_sample_source(const float* src, int srcPitchFloats, int srcW, int srcH,
                            float u, float v)
{
    if (!src || srcW <= 0 || srcH <= 0)
        return mm_v4(0.0f, 0.0f, 0.0f, 0.0f);

    float x = mm_clamp(u - 0.5f, 0.0f, (float)(srcW - 1));
    float y = mm_clamp(v - 0.5f, 0.0f, (float)(srcH - 1));
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    int x1 = x0 + 1 < srcW ? x0 + 1 : x0;
    int y1 = y0 + 1 < srcH ? y0 + 1 : y0;
    float tx = x - (float)x0;
    float ty = y - (float)y0;

    const float* p00 = src + (long long)y0 * srcPitchFloats + x0 * 4;
    const float* p10 = src + (long long)y0 * srcPitchFloats + x1 * 4;
    const float* p01 = src + (long long)y1 * srcPitchFloats + x0 * 4;
    const float* p11 = src + (long long)y1 * srcPitchFloats + x1 * 4;

    float a0 = mm_lerp(p00[0], p10[0], tx);
    float a1 = mm_lerp(p00[1], p10[1], tx);
    float a2 = mm_lerp(p00[2], p10[2], tx);
    float a3 = mm_lerp(p00[3], p10[3], tx);
    float b0 = mm_lerp(p01[0], p11[0], tx);
    float b1 = mm_lerp(p01[1], p11[1], tx);
    float b2 = mm_lerp(p01[2], p11[2], tx);
    float b3 = mm_lerp(p01[3], p11[3], tx);

    return mm_v4(mm_lerp(a0, b0, ty), mm_lerp(a1, b1, ty),
                 mm_lerp(a2, b2, ty), mm_lerp(a3, b3, ty));
}

MM_HD Vec4 mm_sample_source_straight(const float* src, int pitch, int w, int h,
                                     float u, float v, int premult)
{
    Vec4 c = mm_sample_source(src, pitch, w, h, u, v);
    if (premult && c.w > 1.0e-6f)
    {
        float inv = 1.0f / c.w;
        c.x *= inv;
        c.y *= inv;
        c.z *= inv;
    }
    return c;
}

MM_HD Vec4 mm_sample_source_canonical(const float* src, int pitchFloats, int srcW, int srcH,
                                      int srcX, int srcY, float px, float py, int premult)
{
    // px/py are canonical pixel-centre coordinates. Convert them to the
    // local centre convention expected by mm_sample_source (0.5 = first pixel).
    return mm_sample_source_straight(src, pitchFloats, srcW, srcH,
                                     px - (float)srcX,
                                     py - (float)srcY, premult);
}

// ---------------------------------------------------------------------------
// Paper Cutout
// ---------------------------------------------------------------------------

struct PaperParams
{
    int width;
    int height;
    int originX;
    int originY;
    int outPitch;
    int srcPitch;
    int srcX;
    int srcY;
    int srcW;
    int srcH;

    int columns;
    int rows;
    int seed;
    int bgImage;
    int premult;

    float missing;       // 0..1
    float rotation;      // radians
    float sizeVar;
    float widthVar;
    float heightVar;
    float uvShift;       // pixels
    float colorVar;
    float tearSize;      // pixels
    float chunkSize;     // pixels
    float roughness;
    float whiteEdge;     // pixels
    float unevenness;
    float shadow;
    float pxScale;       // shortSide / 1080
    float bgR, bgG, bgB;
};

struct PaperPiece
{
    float cx, cy;
    float halfW, halfH;
    float cosR, sinR;
    float invScaleX, invScaleY;
    float uvx, uvy;
    float tintR, tintG, tintB;
    float paperR, paperG, paperB;
    float seed;
    float boundRadius;
    float maxEdgeDisp;
    unsigned int hash;
    int missing;
    int pad0, pad1;
};

MM_HD float mm_paper_signed_distance(const PaperPiece& pc, const PaperParams& p,
                                     float px, float py)
{
    float dx = px - pc.cx;
    float dy = py - pc.cy;
    float qx = dx * pc.cosR + dy * pc.sinR;
    float qy = -dx * pc.sinR + dy * pc.cosR;
    float d = mm_min(pc.halfW - mm_abs(qx), pc.halfH - mm_abs(qy));

    // Only spend noise samples near the silhouette.
    if (d > pc.maxEdgeDisp + 2.0f)
        return d;

    float seed = pc.seed;
    float invTear = 1.0f / mm_max(p.tearSize * 0.55f, 1.0f);
    float invChunk = 1.0f / mm_max(p.chunkSize * 0.75f, 1.0f);
    unsigned int s = pc.hash;

    float nT = mm_fbm2(qx * invTear + seed * 0.071f,
                       qy * invTear - seed * 0.093f, s, 3) - 0.5f;
    float nC = mm_fbm2(qx * invChunk - seed * 0.041f,
                       qy * invChunk + seed * 0.057f, s ^ 0x68bc21ebu, 2) - 0.5f;
    float nF = mm_value_noise(qx * 0.37f + seed, qy * 0.41f - seed,
                              s ^ 0x02e5be93u) - 0.5f;

    float uneven = 0.55f + 0.9f * p.unevenness;
    float disp = (nT * p.tearSize * 1.05f +
                  nC * p.chunkSize * 0.58f +
                  nF * 1.8f) * p.roughness * uneven;
    float limit = mm_min(pc.halfW, pc.halfH) * 0.72f + 1.0f;
    disp = mm_clamp(disp, -limit, limit);
    return d - disp;
}

MM_HD void mm_build_paper_piece(const PaperParams& p, int col, int row,
                                PaperPiece& pc)
{
    float cellW = (float)p.width / (float)mm_max((float)p.columns, 1.0f);
    float cellH = (float)p.height / (float)mm_max((float)p.rows, 1.0f);

    unsigned int us = (unsigned int)(p.seed < 0 ? -p.seed : p.seed);
    unsigned int hash = mm_hash_u32(((unsigned int)row * 0x9e3779b9u) ^
                                    ((unsigned int)col * 0x85ebca6bu) ^
                                    (us * 0xc2b2ae35u));
    float h0 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 1u);
    float h1 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 2u);
    float h2 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 3u);
    float h3 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 4u);
    float h4 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 5u);
    float h5 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 6u);
    float h6 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 7u);
    float h7 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 8u);
    float h8 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 9u);
    float h9 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 10u);
    float h10 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 11u);
    float h11 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 12u);
    float h12 = mm_hash_float((unsigned int)col, (unsigned int)row, us + 13u);

    float sizeF = 1.0f + (h1 * 2.0f - 1.0f) * p.sizeVar * 0.30f;
    float widthF = 1.0f + (h2 * 2.0f - 1.0f) * p.widthVar * 0.34f;
    float heightF = 1.0f + (h3 * 2.0f - 1.0f) * p.heightVar * 0.34f;
    pc.invScaleX = 1.0f / mm_max(sizeF * widthF, 0.15f);
    pc.invScaleY = 1.0f / mm_max(sizeF * heightF, 0.15f);

    // A small base overlap makes the scraps read as a layered collage instead
    // of a perfectly separated checkerboard.
    pc.halfW = cellW * 0.53f * sizeF * widthF;
    pc.halfH = cellH * 0.53f * sizeF * heightF;

    pc.cx = (float)p.originX + ((float)col + 0.5f) * cellW
          + (h4 - 0.5f) * cellW * 0.16f * p.unevenness;
    pc.cy = (float)p.originY + ((float)row + 0.5f) * cellH
          + (h5 - 0.5f) * cellH * 0.16f * p.unevenness;

    float angle = p.rotation * (h6 * 2.0f - 1.0f);
    pc.cosR = cosf(angle);
    pc.sinR = sinf(angle);

    pc.uvx = (h7 * 2.0f - 1.0f) * p.uvShift;
    pc.uvy = (h8 * 2.0f - 1.0f) * p.uvShift;

    float cv = p.colorVar;
    float exposure = 1.0f + (h9 * 2.0f - 1.0f) * cv * 0.24f;
    float warm = 1.0f + (h10 * 2.0f - 1.0f) * cv * 0.13f;
    float cool = 1.0f - (h10 * 2.0f - 1.0f) * cv * 0.10f;
    float green = 1.0f + (h11 * 2.0f - 1.0f) * cv * 0.10f;
    pc.tintR = exposure * warm;
    pc.tintG = exposure * green;
    pc.tintB = exposure * cool;

    float pv = cv * 0.20f;
    pc.paperR = 1.0f + (h11 * 2.0f - 1.0f) * pv;
    pc.paperG = 1.0f + (h12 * 2.0f - 1.0f) * pv * 0.8f;
    pc.paperB = 1.0f + (h9 * 2.0f - 1.0f) * pv * 0.9f;

    float edgeLimit = mm_min(pc.halfW, pc.halfH) * 0.72f + 1.0f;
    float fullDisp = (p.tearSize * 0.525f + p.chunkSize * 0.29f + 0.9f)
                   * p.roughness * (0.55f + 0.9f * p.unevenness);
    pc.maxEdgeDisp = mm_min(fullDisp, edgeLimit) + 2.0f;
    pc.boundRadius = sqrtf(pc.halfW * pc.halfW + pc.halfH * pc.halfH)
                   + pc.maxEdgeDisp + p.whiteEdge + 3.0f;

    pc.seed = h0 * 997.0f + (float)p.seed * 0.173f;
    pc.hash = hash;
    pc.missing = (h12 < p.missing) ? 1 : 0;
    pc.pad0 = 0;
    pc.pad1 = 0;
}

MM_HD Vec3 mm_paper_color(const PaperPiece& pc, float qx, float qy)
{
    float n1 = mm_fiber_noise(qx * 0.21f + pc.seed * 1.7f,
                              qy * 0.21f - pc.seed * 1.3f, pc.hash);
    float n2 = mm_value_noise(qx * 0.055f + 2.0f, qy * 0.055f - 5.0f,
                              pc.hash ^ 0x51ed270bu);
    float grain = (n1 - 0.5f) * 0.075f + (n2 - 0.5f) * 0.045f;
    Vec3 base = mm_v3(0.965f * pc.paperR, 0.952f * pc.paperG, 0.914f * pc.paperB);
    return mm_scale(base, 1.0f + grain);
}

MM_HD Vec3 mm_apply_piece_tint(Vec3 c, const PaperPiece& pc, float amount)
{
    Vec3 tint = mm_v3(pc.tintR, pc.tintG, pc.tintB);
    Vec3 tinted = mm_mul(c, tint);
    float l = mm_luma(c);
    // Keep some neutral luminance so heavily varied pieces do not wash out.
    Vec3 neutral = mm_v3(l, l, l);
    tinted = mm_mix(tinted, neutral, amount * 0.12f);
    return tinted;
}

MM_HD Vec2 mm_piece_source_uv(const PaperPiece& pc, float px, float py)
{
    float dx = px - pc.cx;
    float dy = py - pc.cy;
    float qx = (dx * pc.cosR + dy * pc.sinR) * pc.invScaleX;
    float qy = (-dx * pc.sinR + dy * pc.cosR) * pc.invScaleY;
    return mm_v2(pc.cx + qx * pc.cosR - qy * pc.sinR + pc.uvx,
                 pc.cy + qx * pc.sinR + qy * pc.cosR + pc.uvy);
}

MM_HD Vec4 mm_paper_pixel(const PaperParams& p, const PaperPiece* pieces,
                          const float* src, int px, int py)
{
    float fx = (float)(p.originX + px) + 0.5f;
    float fy = (float)(p.originY + py) + 0.5f;

    // Preserve the source alpha while compositing in straight colour.
    Vec4 base = mm_sample_source_canonical(src, p.srcPitch, p.srcW, p.srcH,
                                           p.srcX, p.srcY, fx, fy, p.premult);
    Vec4 bg;
    if (p.bgImage)
        bg = base;
    else
        bg = mm_v4(p.bgR, p.bgG, p.bgB, base.w);

    Vec3 color = mm_v3(bg.x, bg.y, bg.z);

    float cellW = (float)p.width / (float)p.columns;
    float cellH = (float)p.height / (float)p.rows;
    if (cellW < 1.0f) cellW = 1.0f;
    if (cellH < 1.0f) cellH = 1.0f;

    float localX = (fx - (float)p.originX) / cellW;
    float localY = (fy - (float)p.originY) / cellH;
    int cx = (int)floorf(localX);
    int cy = (int)floorf(localY);

    float shadowStrength = mm_saturate(p.shadow);
    float shadowDist = p.shadow * 20.0f * p.pxScale;
    float shadowSoft = (2.0f + p.shadow * 15.0f) * p.pxScale;
    float shx = shadowDist * 0.62f;
    float shy = -shadowDist * 0.95f;

    // Large rotation/variation values can push a scrap into a neighbouring
    // cell. A 5x5 search is cheap because every candidate has a round bound
    // reject before any noise is evaluated.
    int y0 = cy - 2;
    int y1 = cy + 2;
    int x0 = cx - 2;
    int x1 = cx + 2;
    if (y0 < 0) y0 = 0;
    if (x0 < 0) x0 = 0;
    if (y1 >= p.rows) y1 = p.rows - 1;
    if (x1 >= p.columns) x1 = p.columns - 1;

    int ry;
    int rx;
    for (ry = y0; ry <= y1; ++ry)
    {
        for (rx = x0; rx <= x1; ++rx)
        {
            const PaperPiece& pc = pieces[ry * p.columns + rx];
            if (pc.missing)
                continue;

            // Cheap round bound test for both the piece and its shadow.
            float bdx = fx - pc.cx;
            float bdy = fy - pc.cy;
            float br = pc.boundRadius + shadowDist + shadowSoft + 2.0f;
            if (bdx * bdx + bdy * bdy > br * br)
                continue;

            if (shadowStrength > 0.001f)
            {
                float sd = mm_paper_signed_distance(pc, p, fx - shx, fy - shy);
                float sa = mm_smoothstep(-shadowSoft, shadowSoft, sd);
                sa *= shadowStrength * 0.56f;
                if (sa > 0.001f)
                {
                    Vec3 shadowColor = mm_v3(0.035f, 0.026f, 0.018f);
                    color = mm_mix(color, shadowColor, sa);
                }
            }

            float d = mm_paper_signed_distance(pc, p, fx, fy);
            float alpha = mm_step_aa(0.0f, d);
            if (alpha <= 0.001f)
                continue;

            float dx = fx - pc.cx;
            float dy = fy - pc.cy;
            float qx = dx * pc.cosR + dy * pc.sinR;
            float qy = -dx * pc.sinR + dy * pc.cosR;

            float whiteNoise = mm_value_noise(qx * 0.15f + pc.seed * 3.1f,
                                              qy * 0.15f - pc.seed * 2.7f,
                                              pc.hash ^ 0x1b56c4e9u);
            float white = p.whiteEdge * (0.72f + 0.62f * whiteNoise);
            float imageD = d - white;
            float imageAlpha = mm_step_aa(0.0f, imageD);

            Vec3 paper = mm_paper_color(pc, qx, qy);
            Vec3 pieceColor = paper;
            if (imageAlpha > 0.001f)
            {
                Vec2 uv = mm_piece_source_uv(pc, fx, fy);
                Vec4 sc = mm_sample_source_straight(src, p.srcPitch, p.srcW, p.srcH,
                                                    uv.x - (float)p.srcX,
                                                    uv.y - (float)p.srcY,
                                                    p.premult);
                Vec3 image = mm_apply_piece_tint(mm_v3(sc.x, sc.y, sc.z), pc, p.colorVar);
                pieceColor = mm_mix(paper, image, imageAlpha);
            }

            color = mm_mix(color, pieceColor, alpha);
        }
    }

    Vec4 out = mm_v4(color.x, color.y, color.z, base.w);
    if (p.premult)
    {
        out.x *= out.w;
        out.y *= out.w;
        out.z *= out.w;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Halftone Screen
// ---------------------------------------------------------------------------

struct HalftoneParams
{
    int width;
    int height;
    int originX;
    int originY;
    int outPitch;
    int srcPitch;
    int srcX;
    int srcY;
    int srcW;
    int srcH;
    int pattern;
    int premult;

    int useC, useM, useY, useK;

    float frequency;     // lines per 1000 px of the 1080p reference
    float roughness;
    float fuzziness;
    float paperFiber;
    float inkTexture;
    float inkDensity;
    float pxScale;
    float angle[4];      // radians: C, M, Y, K
    float offsetX[4];    // pixels
    float offsetY[4];    // pixels
};

MM_HD float mm_halftone_spot(int pattern, float x, float y)
{
    // x/y are centred cell coordinates in [-0.5, 0.5]. Return 1 in the middle
    // of the cell and 0 at the cell boundary (or corners).
    float ax = mm_abs(x);
    float ay = mm_abs(y);
    float v;
    if (pattern == 1) // ellipse
    {
        float ex = x / 1.24f;
        float ey = y / 0.82f;
        v = sqrtf(ex * ex + ey * ey) / 0.56f;
        return mm_saturate(1.0f - v);
    }
    if (pattern == 2) // square
    {
        v = mm_max(ax, ay) * 2.0f;
        return mm_saturate(1.0f - v);
    }
    if (pattern == 3) // diamond
    {
        v = (ax + ay) * 1.0f;
        return mm_saturate(1.0f - v);
    }
    if (pattern == 4) // line
    {
        v = ay * 2.0f;
        return mm_saturate(1.0f - v);
    }
    if (pattern == 5) // cross
    {
        float lx = mm_saturate(1.0f - ax * 2.0f);
        float ly = mm_saturate(1.0f - ay * 2.0f);
        return mm_max(lx, ly);
    }
    // round
    v = sqrtf(x * x + y * y) / 0.70710678f;
    return mm_saturate(1.0f - v);
}

MM_HD float mm_screen_ink(const HalftoneParams& p, int channel,
                          float px, float py, float cell, float tone,
                          unsigned int noiseSeed)
{
    float a = p.angle[channel];
    float ca = cosf(a);
    float sa = sinf(a);
    float ox = p.offsetX[channel];
    float oy = p.offsetY[channel];

    // Roughness is a small distortion of the screen, not random per-frame
    // noise. It stays locked to the image so the print does not crawl.
    float n1 = mm_fbm2(px / mm_max(cell * 3.2f, 1.0f),
                       py / mm_max(cell * 3.2f, 1.0f), noiseSeed, 2) - 0.5f;
    float n2 = mm_value_noise(px / mm_max(cell * 0.55f, 1.0f) + 7.0f,
                              py / mm_max(cell * 0.55f, 1.0f) - 3.0f,
                              noiseSeed ^ 0x9e3779b9u) - 0.5f;
    float rough = p.roughness;
    float dx = (px + ox) + n1 * cell * 0.20f * rough;
    float dy = (py + oy) + n2 * cell * 0.13f * rough;

    float u = dx * ca + dy * sa;
    float v = -dx * sa + dy * ca;

    float gx = u / cell;
    float gy = v / cell;
    float fx = (gx - floorf(gx)) - 0.5f;
    float fy = (gy - floorf(gy)) - 0.5f;

    float spot = mm_halftone_spot(p.pattern, fx, fy);

    // Fine edge break-up and ink bleed.
    float edgeNoise = mm_value_noise(px / mm_max(cell * 0.38f, 1.0f) + 19.0f,
                                     py / mm_max(cell * 0.38f, 1.0f) - 11.0f,
                                     noiseSeed ^ 0x85ebca6bu) - 0.5f;
    spot += edgeNoise * rough * 0.16f;

    float threshold = 1.0f - mm_saturate(tone);
    float edge = spot - threshold;
    float width = 0.008f + p.fuzziness * 0.20f;
    float coverage = mm_smoothstep(-width, width, edge);

    // A little ink tooth at the edge, matching the paper fibre scale.
    float fiber = mm_fiber_noise(px * 0.42f, py * 0.42f, 0x6a09e667u) - 0.5f;
    coverage *= 1.0f + fiber * p.paperFiber * 0.20f;
    return mm_saturate(coverage);
}

MM_HD Vec3 mm_print_ink(Vec3 paper, float aC, float aM, float aY, float aK,
                        float texture)
{
    // Fraction of each ink's complementary RGB transmittance. These are
    // deliberately broad process-ink colours, then multiplied like translucent
    // ink layers on warm paper.
    Vec3 tC = mm_v3(0.015f, 0.70f, 0.94f);
    Vec3 tM = mm_v3(0.90f, 0.10f, 0.52f);
    Vec3 tY = mm_v3(1.00f, 0.96f, 0.10f);
    Vec3 tK = mm_v3(0.055f, 0.055f, 0.065f);

    float mottle = 1.0f + (texture - 0.5f) * 0.16f;
    aC = mm_saturate(aC * mottle);
    aM = mm_saturate(aM * mottle);
    aY = mm_saturate(aY * mottle);
    aK = mm_saturate(aK * mottle);

    Vec3 out = paper;
    out.x *= 1.0f - aC * (1.0f - tC.x);
    out.y *= 1.0f - aC * (1.0f - tC.y);
    out.z *= 1.0f - aC * (1.0f - tC.z);
    out.x *= 1.0f - aM * (1.0f - tM.x);
    out.y *= 1.0f - aM * (1.0f - tM.y);
    out.z *= 1.0f - aM * (1.0f - tM.z);
    out.x *= 1.0f - aY * (1.0f - tY.x);
    out.y *= 1.0f - aY * (1.0f - tY.y);
    out.z *= 1.0f - aY * (1.0f - tY.z);
    out.x *= 1.0f - aK * (1.0f - tK.x);
    out.y *= 1.0f - aK * (1.0f - tK.y);
    out.z *= 1.0f - aK * (1.0f - tK.z);
    return out;
}

MM_HD Vec4 mm_halftone_pixel(const HalftoneParams& p, const float* src, int px, int py)
{
    float fx = (float)(p.originX + px) + 0.5f;
    float fy = (float)(p.originY + py) + 0.5f;

    Vec4 sc = mm_sample_source_canonical(src, p.srcPitch, p.srcW, p.srcH,
                                         p.srcX, p.srcY, fx, fy, p.premult);
    float r = mm_saturate(sc.x);
    float g = mm_saturate(sc.y);
    float b = mm_saturate(sc.z);

    // Naive but stable CMYK separation with maximum black generation. The
    // halftone dots themselves do the rest of the print simulation.
    float c = 1.0f - r;
    float m = 1.0f - g;
    float y = 1.0f - b;
    float k = mm_min(c, mm_min(m, y));
    if (k < 0.9999f)
    {
        float inv = 1.0f / (1.0f - k);
        c = (c - k) * inv;
        m = (m - k) * inv;
        y = (y - k) * inv;
    }
    else
    {
        c = 0.0f;
        m = 0.0f;
        y = 0.0f;
    }

    float density = mm_clamp(p.inkDensity, 0.0f, 2.0f);
    c = mm_saturate(c * density);
    m = mm_saturate(m * density);
    y = mm_saturate(y * density);
    k = mm_saturate(k * density);

    // A subtle paper tooth changes dot growth and gives the screen a physical
    // edge. It is stronger at low frequencies because each dot is larger.
    float fiber = mm_fiber_noise(fx * 0.36f, fy * 0.36f, 0x243f6a88u) - 0.5f;
    float mottle = mm_value_noise(fx * 0.012f, fy * 0.012f, 0x13198a2eu) - 0.5f;
    float toneJitter = (fiber * 0.10f + mottle * 0.08f) * p.paperFiber;
    c = mm_saturate(c + toneJitter * (1.0f - c));
    m = mm_saturate(m + toneJitter * (1.0f - m));
    y = mm_saturate(y + toneJitter * (1.0f - y));
    k = mm_saturate(k + toneJitter * (1.0f - k) * 0.7f);

    // Frequency is authored against a 1080p short side and scales with the
    // rendered image, so proxy/full-res renders match.
    float cell = (1000.0f / mm_max(p.frequency, 1.0f)) * p.pxScale;
    cell = mm_max(cell, 1.5f);

    float aC = 0.0f, aM = 0.0f, aY = 0.0f, aK = 0.0f;
    if (p.useC) aC = mm_screen_ink(p, 0, fx, fy, cell, c, 0x243f6a88u);
    if (p.useM) aM = mm_screen_ink(p, 1, fx, fy, cell, m, 0x13198a2eu);
    if (p.useY) aY = mm_screen_ink(p, 2, fx, fy, cell, y, 0xa4093822u);
    if (p.useK) aK = mm_screen_ink(p, 3, fx, fy, cell, k, 0x299f31d0u);

    // Ink/paper texture: low-frequency mottle plus fine grain. Keep this
    // separate from the screen so it reads as paper, not noise in the dots.
    float paperN = mm_fiber_noise(fx * 0.14f, fy * 0.14f, 0x082efa98u) - 0.5f;
    float paperN2 = mm_value_noise(fx * 0.021f, fy * 0.021f, 0xec4e6c89u) - 0.5f;
    float paperMod = 1.0f + p.paperFiber * (paperN * 0.12f + paperN2 * 0.07f);
    Vec3 paper = mm_v3(0.972f * paperMod, 0.958f * paperMod, 0.918f * paperMod);

    float inkN = mm_fiber_noise(fx * 0.71f, fy * 0.71f, 0x452821e6u) - 0.5f;
    float inkN2 = mm_value_noise(fx * 0.031f, fy * 0.031f, 0x38d01377u) - 0.5f;
    float inkTexture = 0.5f + inkN * p.inkTexture * 0.25f + inkN2 * p.inkTexture * 0.35f;

    Vec3 out = mm_print_ink(paper, aC, aM, aY, aK, inkTexture);

    Vec4 res = mm_v4(out.x, out.y, out.z, sc.w);
    if (p.premult)
    {
        res.x *= res.w;
        res.y *= res.w;
        res.z *= res.w;
    }
    return res;
}
