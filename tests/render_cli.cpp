// Development renderer: CPU reference image -> 24-bit BMP.
// The input is a tightly packed bottom-up float RGBA raw file produced by
// tests/prepare_input.py.
#include "../src/CpuRender.h"
#include "../src/EffectMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

void writeBmp(const char* path, const std::vector<float>& rgba, int width, int height)
{
    const int rowBytes = width * 3;
    const int imageBytes = rowBytes * height;
    const int fileBytes = 54 + imageBytes;
    std::vector<unsigned char> data((size_t)fileBytes, 0);
    data[0] = 'B'; data[1] = 'M';
    data[2] = (unsigned char)(fileBytes);
    data[3] = (unsigned char)(fileBytes >> 8);
    data[4] = (unsigned char)(fileBytes >> 16);
    data[5] = (unsigned char)(fileBytes >> 24);
    data[10] = 54;
    data[14] = 40;
    data[18] = (unsigned char)(width);
    data[19] = (unsigned char)(width >> 8);
    data[20] = (unsigned char)(width >> 16);
    data[21] = (unsigned char)(width >> 24);
    data[22] = (unsigned char)(height);
    data[23] = (unsigned char)(height >> 8);
    data[24] = (unsigned char)(height >> 16);
    data[25] = (unsigned char)(height >> 24);
    data[26] = 1;
    data[28] = 24;
    data[34] = (unsigned char)(imageBytes);
    data[35] = (unsigned char)(imageBytes >> 8);
    data[36] = (unsigned char)(imageBytes >> 16);
    data[37] = (unsigned char)(imageBytes >> 24);
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            const float* p = &rgba[((size_t)y * width + x) * 4];
            unsigned char* o = &data[(size_t)54 + (size_t)y * rowBytes + (size_t)x * 3];
            o[0] = (unsigned char)(std::max(0.0f, std::min(1.0f, p[2])) * 255.0f + 0.5f);
            o[1] = (unsigned char)(std::max(0.0f, std::min(1.0f, p[1])) * 255.0f + 0.5f);
            o[2] = (unsigned char)(std::max(0.0f, std::min(1.0f, p[0])) * 255.0f + 0.5f);
        }
    }
    std::ofstream out(path, std::ios::binary);
    out.write((const char*)data.data(), (std::streamsize)data.size());
}

float getF(const std::map<std::string, std::string>& kv, const char* key, float fallback)
{
    auto it = kv.find(key);
    if (it == kv.end()) return fallback;
    return (float)std::atof(it->second.c_str());
}

int getI(const std::map<std::string, std::string>& kv, const char* key, int fallback)
{
    auto it = kv.find(key);
    if (it == kv.end()) return fallback;
    return std::atoi(it->second.c_str());
}

} // namespace

int main(int argc, char** argv)
{
    std::string effect = "paper";
    std::string input;
    std::string output;
    int width = 0;
    int height = 0;
    std::map<std::string, std::string> kv;

    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--effect" && i + 1 < argc) effect = argv[++i];
        else if (a == "--in" && i + 1 < argc) input = argv[++i];
        else if (a == "--out" && i + 1 < argc) output = argv[++i];
        else if (a == "--width" && i + 1 < argc) width = std::atoi(argv[++i]);
        else if (a == "--height" && i + 1 < argc) height = std::atoi(argv[++i]);
        else if (a.rfind("--", 0) == 0 && i + 1 < argc)
        {
            kv[a.substr(2)] = argv[++i];
        }
    }

    if (input.empty() || output.empty() || width <= 0 || height <= 0)
    {
        std::fprintf(stderr, "usage: render_cli --effect paper|halftone --width W --height H --in in.rgba --out out.bmp [--key value]\n");
        return 2;
    }

    size_t count = (size_t)width * height * 4;
    std::vector<float> src(count);
    std::ifstream in(input, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "cannot open %s\n", input.c_str());
        return 2;
    }
    in.read((char*)src.data(), (std::streamsize)(count * sizeof(float)));
    if ((size_t)in.gcount() != count * sizeof(float))
    {
        std::fprintf(stderr, "input is not the expected size\n");
        return 2;
    }
    std::vector<float> dst(count, 0.0f);

    const float pxScale = std::max(0.05f, (float)std::min(width, height) / 1080.0f);

    if (effect == "halftone")
    {
        HalftoneParams p{};
        p.width = width;
        p.height = height;
        p.originX = 0;
        p.originY = 0;
        p.outPitch = width * 4;
        p.srcPitch = width * 4;
        p.srcX = 0;
        p.srcY = 0;
        p.srcW = width;
        p.srcH = height;
        p.pattern = getI(kv, "pattern", 0);
        p.frequency = getF(kv, "frequency", 95.0f);
        p.roughness = getF(kv, "rough", 0.18f);
        p.fuzziness = getF(kv, "fuzz", 0.22f);
        p.paperFiber = getF(kv, "fiber", 0.25f);
        p.inkTexture = getF(kv, "inkTexture", 0.20f);
        p.inkDensity = getF(kv, "density", 1.0f);
        p.pxScale = pxScale;
        p.useC = getI(kv, "useC", 1);
        p.useM = getI(kv, "useM", 1);
        p.useY = getI(kv, "useY", 1);
        p.useK = getI(kv, "useK", 1);
        const char* names[4] = { "C", "M", "Y", "K" };
        const float defs[4] = { 15.0f, 75.0f, 0.0f, 45.0f };
        for (int c = 0; c < 4; ++c)
        {
            std::string angleKey = std::string("angle") + names[c];
            std::string oxKey = std::string("offsetX") + names[c];
            std::string oyKey = std::string("offsetY") + names[c];
            p.angle[c] = getF(kv, angleKey.c_str(), defs[c]) * 3.14159265358979323846f / 180.0f;
            p.offsetX[c] = getF(kv, oxKey.c_str(), 0.0f) * pxScale;
            p.offsetY[c] = getF(kv, oyKey.c_str(), 0.0f) * pxScale;
        }
        mm::renderHalftoneCpu(p, src.data(), dst.data(), getI(kv, "threads", 0));
    }
    else
    {
        PaperParams p{};
        p.width = width;
        p.height = height;
        p.originX = 0;
        p.originY = 0;
        p.outPitch = width * 4;
        p.srcPitch = width * 4;
        p.srcX = 0;
        p.srcY = 0;
        p.srcW = width;
        p.srcH = height;
        p.columns = getI(kv, "columns", 6);
        p.rows = getI(kv, "rows", 5);
        p.seed = getI(kv, "seed", 7);
        p.missing = getF(kv, "missing", 16.0f) / 100.0f;
        p.rotation = getF(kv, "rotation", 5.0f) * 3.14159265358979323846f / 180.0f;
        p.sizeVar = getF(kv, "sizeVar", 0.18f);
        p.widthVar = getF(kv, "widthVar", 0.28f);
        p.heightVar = getF(kv, "heightVar", 0.28f);
        p.uvShift = getF(kv, "uvShift", 14.0f) * pxScale;
        p.colorVar = getF(kv, "colorVar", 0.24f);
        p.tearSize = getF(kv, "tear", 16.0f) * pxScale;
        p.chunkSize = getF(kv, "chunk", 60.0f) * pxScale;
        p.roughness = getF(kv, "rough", 0.58f);
        p.whiteEdge = getF(kv, "white", 4.0f) * pxScale;
        p.unevenness = getF(kv, "uneven", 0.35f);
        p.shadow = getF(kv, "shadow", 0.35f);
        p.bgImage = getI(kv, "bgImage", 1);
        p.bgR = getF(kv, "bgR", 0.96f);
        p.bgG = getF(kv, "bgG", 0.94f);
        p.bgB = getF(kv, "bgB", 0.90f);
        p.pxScale = pxScale;
        p.premult = getI(kv, "premult", 0);
        mm::renderPaperCpu(p, src.data(), dst.data(), getI(kv, "threads", 0));
    }

    writeBmp(output.c_str(), dst, width, height);
    std::printf("wrote %s\n", output.c_str());
    return 0;
}
