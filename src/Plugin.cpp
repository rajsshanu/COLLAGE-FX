// Mixed Media - Paper Cutout + Halftone Screen, native OpenFX for Resolve.
//
// The plugin requests Resolve's CUDA image buffers and writes directly into
// them with custom kernels embedded as PTX. A multithreaded CPU reference path
// is used when CUDA images are not available.
#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "CpuRender.h"
#include "CudaBackend.h"
#include "EffectMath.h"
#include "Log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

namespace {

constexpr const char* kPluginGroup = "Mixed Media";
constexpr const char* kPaperId = "com.mixedmedia.openfx.papercutout";
constexpr const char* kHalftoneId = "com.mixedmedia.openfx.halftonescreen";
constexpr double kPi = 3.14159265358979323846;

OfxHost* gHost = nullptr;
OfxPropertySuiteV1* gProp = nullptr;
OfxImageEffectSuiteV1* gEffect = nullptr;
OfxParameterSuiteV1* gParam = nullptr;

// ---------------------------------------------------------------------------
// Parameter IDs
// ---------------------------------------------------------------------------

const char* kPaperColumns = "columns";
const char* kPaperRows = "rows";
const char* kPaperMissing = "missingPieces";
const char* kPaperSeed = "seed";
const char* kPaperRotation = "rotation";
const char* kPaperSizeVar = "sizeVariation";
const char* kPaperWidthVar = "widthVariation";
const char* kPaperHeightVar = "heightVariation";
const char* kPaperUvShift = "uvShift";
const char* kPaperColorVar = "colorVariation";
const char* kPaperTear = "tearSize";
const char* kPaperChunk = "chunkSize";
const char* kPaperRough = "roughness";
const char* kPaperWhite = "whiteEdge";
const char* kPaperUneven = "unevenness";
const char* kPaperShadow = "shadow";
const char* kPaperBgImage = "backgroundImage";
const char* kPaperBgColor = "backgroundColor";

const char* kHtPattern = "pattern";
const char* kHtFrequency = "frequency";
const char* kHtRough = "roughness";
const char* kHtFuzz = "fuzziness";
const char* kHtFiber = "paperFiber";
const char* kHtInkTexture = "inkTexture";
const char* kHtDensity = "inkDensity";
const char* kHtC = "channelC";
const char* kHtM = "channelM";
const char* kHtY = "channelY";
const char* kHtK = "channelK";
const char* kHtAngleC = "angleC";
const char* kHtAngleM = "angleM";
const char* kHtAngleY = "angleY";
const char* kHtAngleK = "angleK";
const char* kHtOffXC = "offsetXC";
const char* kHtOffYC = "offsetYC";
const char* kHtOffXM = "offsetXM";
const char* kHtOffYM = "offsetYM";
const char* kHtOffXY = "offsetXY";
const char* kHtOffYY = "offsetYY";
const char* kHtOffXK = "offsetXK";
const char* kHtOffYK = "offsetYK";

// ---------------------------------------------------------------------------
// Property helpers
// ---------------------------------------------------------------------------

void psetString(OfxPropertySetHandle set, const char* key, const char* value, int index = 0)
{
    if (gProp && set)
        gProp->propSetString(set, key, index, value);
}

void psetInt(OfxPropertySetHandle set, const char* key, int value, int index = 0)
{
    if (gProp && set)
        gProp->propSetInt(set, key, index, value);
}

void psetDouble(OfxPropertySetHandle set, const char* key, double value, int index = 0)
{
    if (gProp && set)
        gProp->propSetDouble(set, key, index, value);
}

bool pgetInt(OfxPropertySetHandle set, const char* key, int& value, int index = 0)
{
    return gProp && set && gProp->propGetInt(set, key, index, &value) == kOfxStatOK;
}

bool pgetDouble(OfxPropertySetHandle set, const char* key, double& value, int index = 0)
{
    return gProp && set && gProp->propGetDouble(set, key, index, &value) == kOfxStatOK;
}

bool pgetString(OfxPropertySetHandle set, const char* key, const char*& value, int index = 0)
{
    char* s = nullptr;
    if (!gProp || !set || gProp->propGetString(set, key, index, &s) != kOfxStatOK)
        return false;
    value = s ? s : "";
    return true;
}

bool pgetPointer(OfxPropertySetHandle set, const char* key, void*& value, int index = 0)
{
    return gProp && set && gProp->propGetPointer(set, key, index, &value) == kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

OfxPropertySetHandle defineParam(OfxParamSetHandle set, const char* type,
                                 const char* name, const char* label,
                                 const char* parent)
{
    OfxPropertySetHandle props = nullptr;
    if (!gParam || !set)
        return nullptr;
    if (gParam->paramDefine(set, type, name, &props) != kOfxStatOK)
        return nullptr;
    psetString(props, kOfxPropLabel, label);
    psetString(props, kOfxPropShortLabel, label);
    psetString(props, kOfxPropLongLabel, label);
    psetInt(props, kOfxParamPropAnimates, 1);
    psetInt(props, kOfxParamPropCanUndo, 1);
    if (parent && *parent)
        psetString(props, kOfxParamPropParent, parent);
    return props;
}

void addPageChild(OfxPropertySetHandle page, const char* name)
{
    if (!page || !gProp)
        return;
    int n = 0;
    if (gProp->propGetDimension(page, kOfxParamPropPageChild, &n) != kOfxStatOK)
        n = 0;
    psetString(page, kOfxParamPropPageChild, name, n);
}

OfxPropertySetHandle defineGroup(OfxParamSetHandle set, const char* name,
                                 const char* label, OfxPropertySetHandle page)
{
    OfxPropertySetHandle p = defineParam(set, kOfxParamTypeGroup, name, label, nullptr);
    if (!p)
        return nullptr;
    psetInt(p, kOfxParamPropGroupOpen, 1);
    addPageChild(page, name);
    return p;
}

OfxPropertySetHandle defineInt(OfxParamSetHandle set, const char* name,
                               const char* label, const char* parent,
                               int defaultValue, int minValue, int maxValue,
                               const char* hint)
{
    OfxPropertySetHandle p = defineParam(set, kOfxParamTypeInteger, name, label, parent);
    if (!p) return nullptr;
    psetInt(p, kOfxParamPropDefault, defaultValue);
    psetInt(p, kOfxParamPropMin, minValue);
    psetInt(p, kOfxParamPropMax, maxValue);
    psetInt(p, kOfxParamPropDisplayMin, minValue);
    psetInt(p, kOfxParamPropDisplayMax, maxValue);
    psetInt(p, kOfxParamPropIncrement, 1);
    if (hint) psetString(p, kOfxParamPropHint, hint);
    return p;
}

OfxPropertySetHandle defineDouble(OfxParamSetHandle set, const char* name,
                                  const char* label, const char* parent,
                                  double defaultValue, double minValue, double maxValue,
                                  double increment, int digits, const char* hint)
{
    OfxPropertySetHandle p = defineParam(set, kOfxParamTypeDouble, name, label, parent);
    if (!p) return nullptr;
    psetDouble(p, kOfxParamPropDefault, defaultValue);
    psetDouble(p, kOfxParamPropMin, minValue);
    psetDouble(p, kOfxParamPropMax, maxValue);
    psetDouble(p, kOfxParamPropDisplayMin, minValue);
    psetDouble(p, kOfxParamPropDisplayMax, maxValue);
    psetDouble(p, kOfxParamPropIncrement, increment);
    psetInt(p, kOfxParamPropDigits, digits);
    if (hint) psetString(p, kOfxParamPropHint, hint);
    return p;
}

OfxPropertySetHandle defineBool(OfxParamSetHandle set, const char* name,
                                const char* label, const char* parent,
                                bool defaultValue, const char* hint)
{
    OfxPropertySetHandle p = defineParam(set, kOfxParamTypeBoolean, name, label, parent);
    if (!p) return nullptr;
    psetInt(p, kOfxParamPropDefault, defaultValue ? 1 : 0);
    if (hint) psetString(p, kOfxParamPropHint, hint);
    return p;
}

OfxPropertySetHandle defineChoice(OfxParamSetHandle set, const char* name,
                                  const char* label, const char* parent,
                                  const char* const* options, int optionCount,
                                  int defaultValue, const char* hint)
{
    OfxPropertySetHandle p = defineParam(set, kOfxParamTypeChoice, name, label, parent);
    if (!p) return nullptr;
    for (int i = 0; i < optionCount; ++i)
        psetString(p, kOfxParamPropChoiceOption, options[i], i);
    psetInt(p, kOfxParamPropDefault, defaultValue);
    if (hint) psetString(p, kOfxParamPropHint, hint);
    return p;
}

OfxPropertySetHandle defineColor(OfxParamSetHandle set, const char* name,
                                 const char* label, const char* parent,
                                 double r, double g, double b, const char* hint)
{
    OfxPropertySetHandle p = defineParam(set, kOfxParamTypeRGB, name, label, parent);
    if (!p) return nullptr;
    psetDouble(p, kOfxParamPropDefault, r, 0);
    psetDouble(p, kOfxParamPropDefault, g, 1);
    psetDouble(p, kOfxParamPropDefault, b, 2);
    if (hint) psetString(p, kOfxParamPropHint, hint);
    return p;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

enum class Kind
{
    Paper,
    Halftone
};

struct PaperRefs
{
    OfxParamHandle columns = nullptr;
    OfxParamHandle rows = nullptr;
    OfxParamHandle missing = nullptr;
    OfxParamHandle seed = nullptr;
    OfxParamHandle rotation = nullptr;
    OfxParamHandle sizeVar = nullptr;
    OfxParamHandle widthVar = nullptr;
    OfxParamHandle heightVar = nullptr;
    OfxParamHandle uvShift = nullptr;
    OfxParamHandle colorVar = nullptr;
    OfxParamHandle tear = nullptr;
    OfxParamHandle chunk = nullptr;
    OfxParamHandle rough = nullptr;
    OfxParamHandle white = nullptr;
    OfxParamHandle uneven = nullptr;
    OfxParamHandle shadow = nullptr;
    OfxParamHandle bgImage = nullptr;
    OfxParamHandle bgColor = nullptr;
};

struct HalftoneRefs
{
    OfxParamHandle pattern = nullptr;
    OfxParamHandle frequency = nullptr;
    OfxParamHandle rough = nullptr;
    OfxParamHandle fuzz = nullptr;
    OfxParamHandle fiber = nullptr;
    OfxParamHandle inkTexture = nullptr;
    OfxParamHandle density = nullptr;
    OfxParamHandle channel[4] = {};
    OfxParamHandle angle[4] = {};
    OfxParamHandle offsetX[4] = {};
    OfxParamHandle offsetY[4] = {};
};

struct Instance
{
    Kind kind = Kind::Paper;
    OfxImageEffectHandle effect = nullptr;
    OfxPropertySetHandle effectProps = nullptr;
    OfxImageClipHandle srcClip = nullptr;
    OfxImageClipHandle dstClip = nullptr;
    OfxParamSetHandle paramSet = nullptr;
    PaperRefs paper;
    HalftoneRefs ht;
    mm::CudaRenderState cuda;
    std::mutex renderMutex;
    int cudaHint = -1;
    void* streamHint = nullptr;
    int loggedCpu = 0;
};

struct ImageGuard
{
    OfxPropertySetHandle image = nullptr;
    ~ImageGuard()
    {
        if (image && gEffect)
            gEffect->clipReleaseImage(image);
    }
};

Instance* getInstance(OfxImageEffectHandle effect)
{
    if (!gEffect || !gProp || !effect)
        return nullptr;
    OfxPropertySetHandle props = nullptr;
    if (gEffect->getPropertySet(effect, &props) != kOfxStatOK || !props)
        return nullptr;
    void* data = nullptr;
    if (gProp->propGetPointer(props, kOfxPropInstanceData, 0, &data) != kOfxStatOK)
        return nullptr;
    return static_cast<Instance*>(data);
}

bool fetchParam(OfxParamSetHandle set, const char* name, OfxParamHandle& out)
{
    if (!gParam || !set)
        return false;
    return gParam->paramGetHandle(set, name, &out, nullptr) == kOfxStatOK;
}

int getIntAt(OfxParamHandle param, OfxTime time, int fallback)
{
    int value = fallback;
    if (param && gParam && gParam->paramGetValueAtTime(param, time, &value) == kOfxStatOK)
        return value;
    return fallback;
}

double getDoubleAt(OfxParamHandle param, OfxTime time, double fallback)
{
    double value = fallback;
    if (param && gParam && gParam->paramGetValueAtTime(param, time, &value) == kOfxStatOK)
        return value;
    return fallback;
}

void getColorAt(OfxParamHandle param, OfxTime time, double& r, double& g, double& b)
{
    if (param && gParam)
    {
        double rr = r, gg = g, bb = b;
        if (gParam->paramGetValueAtTime(param, time, &rr, &gg, &bb) == kOfxStatOK)
        {
            r = rr;
            g = gg;
            b = bb;
        }
    }
}

// ---------------------------------------------------------------------------
// Image access
// ---------------------------------------------------------------------------

struct ImageInfo
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    int rowBytes = 0;
    int pitchFloats = 0;
    void* data = nullptr;
    bool premult = false;
};

bool getImageInfo(OfxPropertySetHandle image, ImageInfo& info)
{
    if (!gProp || !image)
        return false;

    const char* depth = nullptr;
    const char* comps = nullptr;
    if (!pgetString(image, kOfxImageEffectPropPixelDepth, depth) ||
        !pgetString(image, kOfxImageEffectPropComponents, comps))
        return false;
    if (std::strcmp(depth, kOfxBitDepthFloat) != 0 ||
        std::strcmp(comps, kOfxImageComponentRGBA) != 0)
        return false;

    int bounds[4] = {};
    if (gProp->propGetIntN(image, kOfxImagePropBounds, 4, bounds) != kOfxStatOK)
        return false;
    int rowBytes = 0;
    if (!pgetInt(image, kOfxImagePropRowBytes, rowBytes))
        rowBytes = 0;
    void* data = nullptr;
    if (!pgetPointer(image, kOfxImagePropData, data) || !data)
        return false;

    const char* premult = nullptr;
    pgetString(image, kOfxImageEffectPropPreMultiplication, premult);

    info.x1 = bounds[0];
    info.y1 = bounds[1];
    info.x2 = bounds[2];
    info.y2 = bounds[3];
    info.rowBytes = rowBytes;
    info.data = data;
    info.premult = premult && std::strcmp(premult, kOfxImagePreMultiplied) == 0;
    int w = info.x2 - info.x1;
    int h = info.y2 - info.y1;
    if (w <= 0 || h <= 0)
        return false;
    if (info.rowBytes == 0)
        info.rowBytes = w * 4 * (int)sizeof(float);
    info.pitchFloats = info.rowBytes / (int)sizeof(float);
    if (info.pitchFloats == 0)
        return false;
    if (std::abs(info.rowBytes) < w * 4 * (int)sizeof(float))
        return false;
    return true;
}

void fillCommonParams(PaperParams& p, const ImageInfo& dst, const ImageInfo& src)
{
    p.width = dst.x2 - dst.x1;
    p.height = dst.y2 - dst.y1;
    p.originX = dst.x1;
    p.originY = dst.y1;
    p.outPitch = dst.pitchFloats;
    p.srcPitch = src.pitchFloats;
    p.srcX = src.x1;
    p.srcY = src.y1;
    p.srcW = src.x2 - src.x1;
    p.srcH = src.y2 - src.y1;
    p.premult = src.premult ? 1 : 0;
}

void fillCommonParams(HalftoneParams& p, const ImageInfo& dst, const ImageInfo& src)
{
    p.width = dst.x2 - dst.x1;
    p.height = dst.y2 - dst.y1;
    p.originX = dst.x1;
    p.originY = dst.y1;
    p.outPitch = dst.pitchFloats;
    p.srcPitch = src.pitchFloats;
    p.srcX = src.x1;
    p.srcY = src.y1;
    p.srcW = src.x2 - src.x1;
    p.srcH = src.y2 - src.y1;
    p.premult = src.premult ? 1 : 0;
}

float referenceScale(const ImageInfo& dst)
{
    int w = dst.x2 - dst.x1;
    int h = dst.y2 - dst.y1;
    int shortSide = std::min(w, h);
    if (shortSide <= 0)
        shortSide = 1080;
    return std::max(0.05f, (float)shortSide / 1080.0f);
}

void fillPaperParams(Instance* inst, PaperParams& p, OfxTime time,
                     const ImageInfo& dst, const ImageInfo& src)
{
    std::memset(&p, 0, sizeof(p));
    fillCommonParams(p, dst, src);
    const float px = referenceScale(dst);

    p.columns = std::max(1, std::min(40, getIntAt(inst->paper.columns, time, 6)));
    p.rows = std::max(1, std::min(40, getIntAt(inst->paper.rows, time, 5)));
    p.seed = std::max(0, std::min(99999, getIntAt(inst->paper.seed, time, 7)));
    p.missing = (float)(std::max(0.0, std::min(100.0,
                getDoubleAt(inst->paper.missing, time, 16.0))) / 100.0);
    p.rotation = (float)(std::max(0.0, std::min(45.0,
                getDoubleAt(inst->paper.rotation, time, 5.0))) * kPi / 180.0);
    p.sizeVar = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->paper.sizeVar, time, 0.18)));
    p.widthVar = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->paper.widthVar, time, 0.28)));
    p.heightVar = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->paper.heightVar, time, 0.28)));
    p.colorVar = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->paper.colorVar, time, 0.24)));
    p.unevenness = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->paper.uneven, time, 0.35)));
    p.shadow = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->paper.shadow, time, 0.35)));
    p.uvShift = (float)std::max(0.0, std::min(150.0,
                getDoubleAt(inst->paper.uvShift, time, 14.0))) * px;
    p.tearSize = (float)std::max(0.0, std::min(120.0,
                getDoubleAt(inst->paper.tear, time, 16.0))) * px;
    p.chunkSize = (float)std::max(0.0, std::min(240.0,
                getDoubleAt(inst->paper.chunk, time, 60.0))) * px;
    p.roughness = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->paper.rough, time, 0.58)));
    p.whiteEdge = (float)std::max(0.0, std::min(60.0,
                getDoubleAt(inst->paper.white, time, 4.0))) * px;
    p.pxScale = px;

    p.bgImage = getIntAt(inst->paper.bgImage, time, 1) ? 1 : 0;
    double r = 0.96, g = 0.94, b = 0.90;
    getColorAt(inst->paper.bgColor, time, r, g, b);
    p.bgR = (float)std::max(0.0, std::min(1.0, r));
    p.bgG = (float)std::max(0.0, std::min(1.0, g));
    p.bgB = (float)std::max(0.0, std::min(1.0, b));
}

void fillHalftoneParams(Instance* inst, HalftoneParams& p, OfxTime time,
                        const ImageInfo& dst, const ImageInfo& src)
{
    std::memset(&p, 0, sizeof(p));
    fillCommonParams(p, dst, src);
    const float px = referenceScale(dst);

    p.pattern = std::max(0, std::min(5, getIntAt(inst->ht.pattern, time, 0)));
    p.frequency = (float)std::max(4.0, std::min(400.0,
                getDoubleAt(inst->ht.frequency, time, 95.0)));
    p.roughness = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->ht.rough, time, 0.18)));
    p.fuzziness = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->ht.fuzz, time, 0.22)));
    p.paperFiber = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->ht.fiber, time, 0.25)));
    p.inkTexture = (float)std::max(0.0, std::min(1.0,
                getDoubleAt(inst->ht.inkTexture, time, 0.20)));
    p.inkDensity = (float)std::max(0.0, std::min(2.0,
                getDoubleAt(inst->ht.density, time, 1.0)));
    p.pxScale = px;

    p.useC = getIntAt(inst->ht.channel[0], time, 1) ? 1 : 0;
    p.useM = getIntAt(inst->ht.channel[1], time, 1) ? 1 : 0;
    p.useY = getIntAt(inst->ht.channel[2], time, 1) ? 1 : 0;
    p.useK = getIntAt(inst->ht.channel[3], time, 1) ? 1 : 0;

    for (int i = 0; i < 4; ++i)
    {
        p.angle[i] = (float)(getDoubleAt(inst->ht.angle[i], time,
                                         i == 0 ? 15.0 : (i == 1 ? 75.0 : (i == 2 ? 0.0 : 45.0)))
                              * kPi / 180.0);
        p.offsetX[i] = (float)getDoubleAt(inst->ht.offsetX[i], time, 0.0) * px;
        p.offsetY[i] = (float)getDoubleAt(inst->ht.offsetY[i], time, 0.0) * px;
    }
}

// ---------------------------------------------------------------------------
// Description
// ---------------------------------------------------------------------------

OfxStatus actionDescribe(OfxImageEffectHandle effect, Kind kind)
{
    OfxPropertySetHandle props = nullptr;
    if (!gEffect || gEffect->getPropertySet(effect, &props) != kOfxStatOK || !props)
        return kOfxStatFailed;

    const bool paper = kind == Kind::Paper;
    psetString(props, kOfxPropLabel, paper ? "Paper Cutout" : "Halftone Screen");
    psetString(props, kOfxPropShortLabel, paper ? "Paper Cutout" : "Halftone");
    psetString(props, kOfxPropLongLabel,
               paper ? "Mixed Media - Paper Cutout" : "Mixed Media - Halftone Screen");
    psetString(props, kOfxPropPluginDescription,
               paper ? "Torn paper collage with per-scrap transforms, missing pieces, shadows and a paper core edge."
                     : "CMYK halftone screen with per-channel angles, registration offsets, ink and paper texture.");
    psetString(props, kOfxImageEffectPluginPropGrouping, kPluginGroup);

    psetString(props, kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter, 0);
    psetString(props, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);
    psetInt(props, kOfxImageEffectPropSupportsTiles, 0);
    psetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0);
    psetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0);
    psetInt(props, kOfxImageEffectPropSupportsMultipleClipPARs, 0);
    psetInt(props, kOfxImageEffectPropTemporalClipAccess, 0);
    psetInt(props, kOfxImageEffectPluginPropSingleInstance, 0);
    psetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0);
    psetString(props, kOfxImageEffectPluginRenderThreadSafety, kOfxImageEffectRenderInstanceSafe);
    psetInt(props, kOfxImageEffectPropSetableFrameRate, 0);
    psetInt(props, kOfxImageEffectPropSetableFielding, 0);

    // We have a CPU reference path, so advertise it as available. CUDA is the
    // preferred path and Resolve hands us device pointers when enabled.
    psetString(props, kOfxImageEffectPropCPURenderSupported, "true");
    psetString(props, kOfxImageEffectPropCudaRenderSupported, "true");
    psetString(props, kOfxImageEffectPropCudaStreamSupported, "true");
    psetString(props, kOfxImageEffectPropOpenCLRenderSupported, "false");
    psetString(props, kOfxImageEffectPropMetalRenderSupported, "false");
    psetString(props, kOfxImageEffectPropOpenGLRenderSupported, "false");
    return kOfxStatOK;
}

OfxStatus definePaperParams(OfxImageEffectHandle effect)
{
    OfxParamSetHandle set = nullptr;
    if (!gEffect || gEffect->getParamSet(effect, &set) != kOfxStatOK)
        return kOfxStatFailed;

    OfxPropertySetHandle page = defineParam(set, kOfxParamTypePage, "controls", "Controls", nullptr);
    if (!page) return kOfxStatFailed;

    OfxPropertySetHandle shape = defineGroup(set, "shapeGroup", "SHAPE", page);
    if (!shape) return kOfxStatFailed;
    defineInt(set, kPaperColumns, "Columns", "shapeGroup", 6, 1, 40,
              "Number of scraps across the image.");
    defineInt(set, kPaperRows, "Rows", "shapeGroup", 5, 1, 40,
              "Number of scraps down the image.");
    defineDouble(set, kPaperRotation, "Rotation", "shapeGroup", 5.0, 0.0, 45.0, 0.1, 2,
                 "Maximum random rotation of each scrap, in degrees.");
    defineDouble(set, kPaperUneven, "Unevenness", "shapeGroup", 0.35, 0.0, 1.0, 0.01, 2,
                 "Offsets scraps and tears the grid away from a perfect checkerboard.");

    OfxPropertySetHandle frag = defineGroup(set, "fragmentGroup", "PIECES", page);
    if (!frag) return kOfxStatFailed;
    defineDouble(set, kPaperMissing, "Missing Pieces (%)", "fragmentGroup", 16.0, 0.0, 100.0,
                 0.5, 1, "Percentage of scraps removed to reveal the background. Keyframe this for animation.");
    defineInt(set, kPaperSeed, "Seed", "fragmentGroup", 7, 0, 99999,
              "Layout seed. Keyframe it to move every torn scrap and missing gap.");
    defineDouble(set, kPaperSizeVar, "Size Variation", "fragmentGroup", 0.18, 0.0, 1.0,
                 0.01, 2, "Random overall scale variation per scrap.");
    defineDouble(set, kPaperWidthVar, "Width Variation", "fragmentGroup", 0.28, 0.0, 1.0,
                 0.01, 2, "Random width variation per scrap.");
    defineDouble(set, kPaperHeightVar, "Height Variation", "fragmentGroup", 0.28, 0.0, 1.0,
                 0.01, 2, "Random height variation per scrap.");
    defineDouble(set, kPaperUvShift, "UV Shift (px)", "fragmentGroup", 14.0, 0.0, 150.0,
                 0.5, 1, "Random source-image shift inside each scrap, creating a cut-and-pasted misregistration.");
    defineDouble(set, kPaperColorVar, "Color Variation", "fragmentGroup", 0.24, 0.0, 1.0,
                 0.01, 2, "Per-scrap tint and exposure variation.");

    OfxPropertySetHandle edge = defineGroup(set, "edgeGroup", "TORN PAPER", page);
    if (!edge) return kOfxStatFailed;
    defineDouble(set, kPaperTear, "Tear Size (px)", "edgeGroup", 16.0, 0.0, 120.0,
                 0.5, 1, "Scale of the ragged tear along the scrap edges.");
    defineDouble(set, kPaperChunk, "Chunk Size (px)", "edgeGroup", 60.0, 0.0, 240.0,
                 1.0, 1, "Scale of large missing chunks and deep bites in the edge.");
    defineDouble(set, kPaperRough, "Roughness", "edgeGroup", 0.58, 0.0, 1.0,
                 0.01, 2, "Amount of high-frequency tearing and edge break-up.");
    defineDouble(set, kPaperWhite, "White Edge (px)", "edgeGroup", 4.0, 0.0, 60.0,
                 0.25, 2, "Exposed white paper core around the printed image.");

    OfxPropertySetHandle comp = defineGroup(set, "compositeGroup", "COMPOSITE", page);
    if (!comp) return kOfxStatFailed;
    defineDouble(set, kPaperShadow, "Shadow", "compositeGroup", 0.35, 0.0, 1.0,
                 0.01, 2, "Drop shadow cast by each scrap onto the layers below.");
    defineBool(set, kPaperBgImage, "Background Image", "compositeGroup", true,
               "Show the original image behind missing scraps. Turn off to use a flat background colour.");
    defineColor(set, kPaperBgColor, "Background Color", "compositeGroup",
                0.96, 0.94, 0.90, "Flat background used when Background Image is off.");
    return kOfxStatOK;
}

OfxStatus defineHalftoneParams(OfxImageEffectHandle effect)
{
    OfxParamSetHandle set = nullptr;
    if (!gEffect || gEffect->getParamSet(effect, &set) != kOfxStatOK)
        return kOfxStatFailed;

    OfxPropertySetHandle page = defineParam(set, kOfxParamTypePage, "controls", "Controls", nullptr);
    if (!page) return kOfxStatFailed;

    OfxPropertySetHandle screen = defineGroup(set, "screenGroup", "SCREEN", page);
    if (!screen) return kOfxStatFailed;
    static const char* patternOptions[] = {
        "Round", "Ellipse", "Square", "Diamond", "Line", "Cross"
    };
    defineChoice(set, kHtPattern, "Pattern", "screenGroup", patternOptions, 6, 0,
                 "Halftone dot shape used by every enabled ink channel.");
    defineDouble(set, kHtFrequency, "Frequency", "screenGroup", 95.0, 4.0, 400.0,
                 1.0, 1, "Screen frequency in lines per 1000 px of the 1080p reference. Higher is finer.");
    defineDouble(set, kHtRough, "Roughness", "screenGroup", 0.18, 0.0, 1.0,
                 0.01, 2, "Irregularity of dot edges and screen registration.");
    defineDouble(set, kHtFuzz, "Fuzziness", "screenGroup", 0.22, 0.0, 1.0,
                 0.01, 2, "Softness of the ink edge, from crisp to absorbed.");

    OfxPropertySetHandle ink = defineGroup(set, "inkGroup", "INK & PAPER", page);
    if (!ink) return kOfxStatFailed;
    defineDouble(set, kHtFiber, "Paper Fiber", "inkGroup", 0.25, 0.0, 1.0,
                 0.01, 2, "Directional paper tooth that breaks up dots and adds mottling.");
    defineDouble(set, kHtInkTexture, "Ink Texture", "inkGroup", 0.20, 0.0, 1.0,
                 0.01, 2, "Ink density mottling and fine press grain.");
    defineDouble(set, kHtDensity, "Ink Density", "inkGroup", 1.0, 0.0, 2.0,
                 0.01, 2, "Dot gain / ink load. 1.0 is the neutral print.");

    OfxPropertySetHandle ch = defineGroup(set, "channelsGroup", "CHANNELS", page);
    if (!ch) return kOfxStatFailed;
    defineBool(set, kHtC, "Cyan", "channelsGroup", true, "Enable the cyan ink screen.");
    defineBool(set, kHtM, "Magenta", "channelsGroup", true, "Enable the magenta ink screen.");
    defineBool(set, kHtY, "Yellow", "channelsGroup", true, "Enable the yellow ink screen.");
    defineBool(set, kHtK, "Black", "channelsGroup", true, "Enable the black ink screen.");

    OfxPropertySetHandle cyan = defineGroup(set, "cyanGroup", "CYAN SCREEN", page);
    if (!cyan) return kOfxStatFailed;
    defineDouble(set, kHtAngleC, "Angle (deg)", "cyanGroup", 15.0, -180.0, 180.0, 0.1, 2,
                 "Cyan screen angle. 15 degrees is the standard offset angle.");
    defineDouble(set, kHtOffXC, "Offset X (px)", "cyanGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Cyan plate horizontal registration offset.");
    defineDouble(set, kHtOffYC, "Offset Y (px)", "cyanGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Cyan plate vertical registration offset.");

    OfxPropertySetHandle magenta = defineGroup(set, "magentaGroup", "MAGENTA SCREEN", page);
    if (!magenta) return kOfxStatFailed;
    defineDouble(set, kHtAngleM, "Angle (deg)", "magentaGroup", 75.0, -180.0, 180.0, 0.1, 2,
                 "Magenta screen angle. 75 degrees is the standard offset angle.");
    defineDouble(set, kHtOffXM, "Offset X (px)", "magentaGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Magenta plate horizontal registration offset.");
    defineDouble(set, kHtOffYM, "Offset Y (px)", "magentaGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Magenta plate vertical registration offset.");

    OfxPropertySetHandle yellow = defineGroup(set, "yellowGroup", "YELLOW SCREEN", page);
    if (!yellow) return kOfxStatFailed;
    defineDouble(set, kHtAngleY, "Angle (deg)", "yellowGroup", 0.0, -180.0, 180.0, 0.1, 2,
                 "Yellow screen angle. 0 degrees is the standard offset angle.");
    defineDouble(set, kHtOffXY, "Offset X (px)", "yellowGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Yellow plate horizontal registration offset.");
    defineDouble(set, kHtOffYY, "Offset Y (px)", "yellowGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Yellow plate vertical registration offset.");

    OfxPropertySetHandle black = defineGroup(set, "blackGroup", "BLACK SCREEN", page);
    if (!black) return kOfxStatFailed;
    defineDouble(set, kHtAngleK, "Angle (deg)", "blackGroup", 45.0, -180.0, 180.0, 0.1, 2,
                 "Black screen angle. 45 degrees is the standard key angle.");
    defineDouble(set, kHtOffXK, "Offset X (px)", "blackGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Black plate horizontal registration offset.");
    defineDouble(set, kHtOffYK, "Offset Y (px)", "blackGroup", 0.0, -200.0, 200.0, 0.25, 2,
                 "Black plate vertical registration offset.");
    return kOfxStatOK;
}

OfxStatus actionDescribeInContext(OfxImageEffectHandle effect, Kind kind)
{
    OfxPropertySetHandle src = nullptr;
    OfxPropertySetHandle dst = nullptr;
    if (!gEffect ||
        gEffect->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &src) != kOfxStatOK ||
        gEffect->clipDefine(effect, kOfxImageEffectOutputClipName, &dst) != kOfxStatOK)
        return kOfxStatFailed;

    psetString(src, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
    psetInt(src, kOfxImageEffectPropSupportsTiles, 0);
    psetInt(src, kOfxImageEffectPropTemporalClipAccess, 0);
    psetString(src, kOfxPropLabel, "Source");
    psetInt(src, kOfxImageClipPropOptional, 0);
    psetString(src, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);

    psetString(dst, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA, 0);
    psetInt(dst, kOfxImageEffectPropSupportsTiles, 0);
    psetString(dst, kOfxPropLabel, "Output");
    psetString(dst, kOfxImageEffectPropSupportedPixelDepths, kOfxBitDepthFloat, 0);

    return kind == Kind::Paper ? definePaperParams(effect) : defineHalftoneParams(effect);
}

// ---------------------------------------------------------------------------
// Instance setup
// ---------------------------------------------------------------------------

bool fetchPaperHandles(Instance* inst)
{
    return fetchParam(inst->paramSet, kPaperColumns, inst->paper.columns) &&
           fetchParam(inst->paramSet, kPaperRows, inst->paper.rows) &&
           fetchParam(inst->paramSet, kPaperMissing, inst->paper.missing) &&
           fetchParam(inst->paramSet, kPaperSeed, inst->paper.seed) &&
           fetchParam(inst->paramSet, kPaperRotation, inst->paper.rotation) &&
           fetchParam(inst->paramSet, kPaperSizeVar, inst->paper.sizeVar) &&
           fetchParam(inst->paramSet, kPaperWidthVar, inst->paper.widthVar) &&
           fetchParam(inst->paramSet, kPaperHeightVar, inst->paper.heightVar) &&
           fetchParam(inst->paramSet, kPaperUvShift, inst->paper.uvShift) &&
           fetchParam(inst->paramSet, kPaperColorVar, inst->paper.colorVar) &&
           fetchParam(inst->paramSet, kPaperTear, inst->paper.tear) &&
           fetchParam(inst->paramSet, kPaperChunk, inst->paper.chunk) &&
           fetchParam(inst->paramSet, kPaperRough, inst->paper.rough) &&
           fetchParam(inst->paramSet, kPaperWhite, inst->paper.white) &&
           fetchParam(inst->paramSet, kPaperUneven, inst->paper.uneven) &&
           fetchParam(inst->paramSet, kPaperShadow, inst->paper.shadow) &&
           fetchParam(inst->paramSet, kPaperBgImage, inst->paper.bgImage) &&
           fetchParam(inst->paramSet, kPaperBgColor, inst->paper.bgColor);
}

bool fetchHalftoneHandles(Instance* inst)
{
    const char* channels[4] = { kHtC, kHtM, kHtY, kHtK };
    const char* angles[4] = { kHtAngleC, kHtAngleM, kHtAngleY, kHtAngleK };
    const char* offX[4] = { kHtOffXC, kHtOffXM, kHtOffXY, kHtOffXK };
    const char* offY[4] = { kHtOffYC, kHtOffYM, kHtOffYY, kHtOffYK };

    if (!fetchParam(inst->paramSet, kHtPattern, inst->ht.pattern) ||
        !fetchParam(inst->paramSet, kHtFrequency, inst->ht.frequency) ||
        !fetchParam(inst->paramSet, kHtRough, inst->ht.rough) ||
        !fetchParam(inst->paramSet, kHtFuzz, inst->ht.fuzz) ||
        !fetchParam(inst->paramSet, kHtFiber, inst->ht.fiber) ||
        !fetchParam(inst->paramSet, kHtInkTexture, inst->ht.inkTexture) ||
        !fetchParam(inst->paramSet, kHtDensity, inst->ht.density))
        return false;
    for (int i = 0; i < 4; ++i)
    {
        if (!fetchParam(inst->paramSet, channels[i], inst->ht.channel[i]) ||
            !fetchParam(inst->paramSet, angles[i], inst->ht.angle[i]) ||
            !fetchParam(inst->paramSet, offX[i], inst->ht.offsetX[i]) ||
            !fetchParam(inst->paramSet, offY[i], inst->ht.offsetY[i]))
            return false;
    }
    return true;
}

OfxStatus actionCreateInstance(OfxImageEffectHandle effect, Kind kind)
{
    Instance* inst = new (std::nothrow) Instance();
    if (!inst)
        return kOfxStatErrMemory;
    inst->kind = kind;
    inst->effect = effect;

    if (!gEffect ||
        gEffect->getPropertySet(effect, &inst->effectProps) != kOfxStatOK ||
        gEffect->getParamSet(effect, &inst->paramSet) != kOfxStatOK ||
        gEffect->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &inst->srcClip, nullptr) != kOfxStatOK ||
        gEffect->clipGetHandle(effect, kOfxImageEffectOutputClipName, &inst->dstClip, nullptr) != kOfxStatOK)
    {
        delete inst;
        return kOfxStatFailed;
    }

    bool ok = kind == Kind::Paper ? fetchPaperHandles(inst) : fetchHalftoneHandles(inst);
    if (!ok)
    {
        delete inst;
        return kOfxStatFailed;
    }

    gProp->propSetPointer(inst->effectProps, kOfxPropInstanceData, 0, inst);
    mm::logMessage("instance created: %s", kind == Kind::Paper ? "Paper Cutout" : "Halftone Screen");
    return kOfxStatOK;
}

OfxStatus actionDestroyInstance(OfxImageEffectHandle effect)
{
    Instance* inst = getInstance(effect);
    if (!inst)
        return kOfxStatOK;
    if (inst->effectProps)
        gProp->propSetPointer(inst->effectProps, kOfxPropInstanceData, 0, nullptr);

    {
        std::lock_guard<std::mutex> lock(inst->renderMutex);
        std::lock_guard<std::mutex> gpuLock(inst->cuda.mutex);
        mm::cudaDestroy(inst->cuda);
    }
    delete inst;
    return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

OfxStatus renderCpu(Instance* inst, const PaperParams& p, const float* src, float* dst)
{
    mm::renderPaperCpu(p, src, dst);
    if (inst->loggedCpu != 1)
    {
        inst->loggedCpu = 1;
        mm::logMessage("Paper Cutout: CPU fallback render %dx%d", p.width, p.height);
    }
    return kOfxStatOK;
}

OfxStatus renderCpu(Instance* inst, const HalftoneParams& p, const float* src, float* dst)
{
    mm::renderHalftoneCpu(p, src, dst);
    if (inst->loggedCpu != 1)
    {
        inst->loggedCpu = 1;
        mm::logMessage("Halftone Screen: CPU fallback render %dx%d", p.width, p.height);
    }
    return kOfxStatOK;
}

OfxStatus actionRender(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    Instance* inst = getInstance(effect);
    if (!inst || !inArgs)
        return kOfxStatErrBadHandle;

    double time = 0.0;
    pgetDouble(inArgs, kOfxPropTime, time);

    int cudaEnabled = 0;
    if (!pgetInt(inArgs, kOfxImageEffectPropCudaEnabled, cudaEnabled))
    {
        if (inst->cudaHint >= 0)
            cudaEnabled = inst->cudaHint;
        else
            pgetInt(inst->effectProps, kOfxImageEffectPropCudaEnabled, cudaEnabled);
    }
    void* stream = nullptr;
    if (!pgetPointer(inArgs, kOfxImageEffectPropCudaStream, stream))
        stream = inst->streamHint;

    ImageGuard srcImg;
    ImageGuard dstImg;
    if (gEffect->clipGetImage(inst->srcClip, time, nullptr, &srcImg.image) != kOfxStatOK)
        srcImg.image = nullptr;
    if (gEffect->clipGetImage(inst->dstClip, time, nullptr, &dstImg.image) != kOfxStatOK)
        return kOfxStatErrValue;

    ImageInfo si;
    ImageInfo di;
    if (!getImageInfo(dstImg.image, di))
        return kOfxStatErrUnsupported;
    if (!srcImg.image || !getImageInfo(srcImg.image, si))
        return kOfxStatErrUnsupported;

    if (inst->kind == Kind::Paper)
    {
        PaperParams p;
        fillPaperParams(inst, p, time, di, si);

        std::lock_guard<std::mutex> lock(inst->renderMutex);
        if (cudaEnabled)
        {
            if (di.pitchFloats <= 0 || si.pitchFloats <= 0)
            {
                mm::logMessage("Paper Cutout: CUDA images have unsupported negative row stride");
                return kOfxStatGPURenderFailed;
            }
            std::string error;
            {
                std::lock_guard<std::mutex> gpuLock(inst->cuda.mutex);
                if (!mm::cudaRenderPaper(inst->cuda, p, si.data, di.data, stream, error))
                {
                    mm::logMessage("Paper Cutout CUDA render failed: %s", error.c_str());
                    return kOfxStatGPURenderFailed;
                }
            }
            if (inst->loggedCpu != 2)
            {
                inst->loggedCpu = 2;
                mm::logMessage("Paper Cutout: CUDA render %dx%d", p.width, p.height);
            }
            return kOfxStatOK;
        }

        return renderCpu(inst, p, (const float*)si.data, (float*)di.data);
    }

    HalftoneParams p;
    fillHalftoneParams(inst, p, time, di, si);

    std::lock_guard<std::mutex> lock(inst->renderMutex);
    if (cudaEnabled)
    {
        if (di.pitchFloats <= 0 || si.pitchFloats <= 0)
        {
            mm::logMessage("Halftone Screen: CUDA images have unsupported negative row stride");
            return kOfxStatGPURenderFailed;
        }
        std::string error;
        {
            std::lock_guard<std::mutex> gpuLock(inst->cuda.mutex);
            if (!mm::cudaRenderHalftone(inst->cuda, p, si.data, di.data, stream, error))
            {
                mm::logMessage("Halftone Screen CUDA render failed: %s", error.c_str());
                return kOfxStatGPURenderFailed;
            }
        }
        if (inst->loggedCpu != 2)
        {
            inst->loggedCpu = 2;
            mm::logMessage("Halftone Screen: CUDA render %dx%d", p.width, p.height);
        }
        return kOfxStatOK;
    }

    return renderCpu(inst, p, (const float*)si.data, (float*)di.data);
}

// ---------------------------------------------------------------------------
// Other OFX actions
// ---------------------------------------------------------------------------

OfxStatus actionIsIdentity(OfxImageEffectHandle, OfxPropertySetHandle, OfxPropertySetHandle)
{
    return kOfxStatReplyDefault;
}

OfxStatus actionGetRegionOfDefinition(OfxImageEffectHandle effect,
                                      OfxPropertySetHandle inArgs,
                                      OfxPropertySetHandle outArgs)
{
    Instance* inst = getInstance(effect);
    if (!inst)
        return kOfxStatReplyDefault;
    double time = 0.0;
    pgetDouble(inArgs, kOfxPropTime, time);
    OfxRectD rod = {};
    if (gEffect->clipGetRegionOfDefinition(inst->srcClip, time, &rod) != kOfxStatOK)
        return kOfxStatReplyDefault;
    psetDouble(outArgs, kOfxImagePropRegionOfDefinition, rod.x1, 0);
    psetDouble(outArgs, kOfxImagePropRegionOfDefinition, rod.y1, 1);
    psetDouble(outArgs, kOfxImagePropRegionOfDefinition, rod.x2, 2);
    psetDouble(outArgs, kOfxImagePropRegionOfDefinition, rod.y2, 3);
    return kOfxStatOK;
}

OfxStatus actionGetClipPreferences(OfxImageEffectHandle, OfxPropertySetHandle, OfxPropertySetHandle outArgs)
{
    psetString(outArgs, "OfxImageClipPropComponents_Output", kOfxImageComponentRGBA, 0);
    psetString(outArgs, "OfxImageClipPropDepth_Output", kOfxBitDepthFloat, 0);
    return kOfxStatOK;
}

OfxStatus actionInstanceChanged(OfxImageEffectHandle, OfxPropertySetHandle, OfxPropertySetHandle)
{
    return kOfxStatReplyDefault;
}

OfxStatus actionBeginSequenceRender(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs)
{
    Instance* inst = getInstance(effect);
    if (!inst)
        return kOfxStatReplyDefault;
    int enabled = 0;
    if (pgetInt(inArgs, kOfxImageEffectPropCudaEnabled, enabled))
        inst->cudaHint = enabled;
    void* stream = nullptr;
    if (pgetPointer(inArgs, kOfxImageEffectPropCudaStream, stream))
        inst->streamHint = stream;
    return kOfxStatReplyDefault;
}

OfxStatus actionPurgeCaches(OfxImageEffectHandle)
{
    return kOfxStatOK;
}

OfxStatus mainEntry(const char* action, const void* handle,
                    OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs,
                    Kind kind)
{
    if (!action)
        return kOfxStatErrBadHandle;
    if (std::strcmp(action, kOfxActionLoad) == 0 ||
        std::strcmp(action, kOfxActionUnload) == 0)
        return kOfxStatOK;
    if (!handle)
        return kOfxStatErrBadHandle;

    if (std::strcmp(action, kOfxActionDescribe) == 0)
        return actionDescribe((OfxImageEffectHandle)handle, kind);
    if (std::strcmp(action, kOfxImageEffectActionDescribeInContext) == 0)
        return actionDescribeInContext((OfxImageEffectHandle)handle, kind);
    if (std::strcmp(action, kOfxActionCreateInstance) == 0)
        return actionCreateInstance((OfxImageEffectHandle)handle, kind);
    if (std::strcmp(action, kOfxActionDestroyInstance) == 0)
        return actionDestroyInstance((OfxImageEffectHandle)handle);
    if (std::strcmp(action, kOfxImageEffectActionRender) == 0)
        return actionRender((OfxImageEffectHandle)handle, inArgs);
    if (std::strcmp(action, kOfxImageEffectActionIsIdentity) == 0)
        return actionIsIdentity((OfxImageEffectHandle)handle, inArgs, outArgs);
    if (std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0)
        return actionGetRegionOfDefinition((OfxImageEffectHandle)handle, inArgs, outArgs);
    if (std::strcmp(action, kOfxImageEffectActionGetClipPreferences) == 0)
        return actionGetClipPreferences((OfxImageEffectHandle)handle, inArgs, outArgs);
    if (std::strcmp(action, kOfxActionInstanceChanged) == 0)
        return actionInstanceChanged((OfxImageEffectHandle)handle, inArgs, outArgs);
    if (std::strcmp(action, kOfxActionPurgeCaches) == 0)
        return actionPurgeCaches((OfxImageEffectHandle)handle);
    if (std::strcmp(action, kOfxImageEffectActionBeginSequenceRender) == 0)
        return actionBeginSequenceRender((OfxImageEffectHandle)handle, inArgs);
    if (std::strcmp(action, kOfxImageEffectActionEndSequenceRender) == 0)
        return kOfxStatReplyDefault;
    return kOfxStatReplyDefault;
}

OfxStatus paperEntry(const char* action, const void* handle,
                     OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
    return mainEntry(action, handle, inArgs, outArgs, Kind::Paper);
}

OfxStatus halftoneEntry(const char* action, const void* handle,
                        OfxPropertySetHandle inArgs, OfxPropertySetHandle outArgs)
{
    return mainEntry(action, handle, inArgs, outArgs, Kind::Halftone);
}

void setHost(OfxHost* host)
{
    gHost = host;
    if (!host || !host->fetchSuite)
        return;
    gProp = (OfxPropertySuiteV1*)host->fetchSuite(host->host, kOfxPropertySuite, 1);
    gEffect = (OfxImageEffectSuiteV1*)host->fetchSuite(host->host, kOfxImageEffectSuite, 1);
    gParam = (OfxParameterSuiteV1*)host->fetchSuite(host->host, kOfxParameterSuite, 1);
}

OfxPlugin gPaperPlugin = {
    kOfxImageEffectPluginApi,
    kOfxImageEffectPluginApiVersion,
    kPaperId,
    1,
    1,
    setHost,
    paperEntry
};

OfxPlugin gHalftonePlugin = {
    kOfxImageEffectPluginApi,
    kOfxImageEffectPluginApiVersion,
    kHalftoneId,
    1,
    1,
    setHost,
    halftoneEntry
};

} // namespace

OfxExport int OfxGetNumberOfPlugins(void)
{
    return 2;
}

OfxExport OfxPlugin* OfxGetPlugin(int nth)
{
    if (nth == 0) return &gPaperPlugin;
    if (nth == 1) return &gHalftonePlugin;
    return nullptr;
}

OfxExport OfxStatus OfxSetHost(const OfxHost* host)
{
    setHost(const_cast<OfxHost*>(host));
    return kOfxStatOK;
}
