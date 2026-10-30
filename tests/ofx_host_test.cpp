// Mixed Media - minimal OpenFX host used to exercise the real plugin binary.
//
// This is not a general-purpose OFX host. It implements only the properties
// and suites Mixed Media uses, which is enough to validate plugin registration,
// parameter definitions, instance creation, CPU rendering and CUDA rendering.
#include "ofxImageEffect.h"
#include "ofxGPURender.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

int gFailures = 0;

#ifdef _WIN32
// Minimal CUDA driver access for the host test. The plugin creates its own
// context/module state; this only makes a context current and supplies device
// pixel buffers so the real plugin CUDA branch can be exercised.
typedef int TestCUresult;
typedef int TestCUdevice;
typedef unsigned long long TestCUdeviceptr;
typedef void* TestCUstream;
constexpr TestCUresult TEST_CUDA_SUCCESS = 0;

typedef TestCUresult(__stdcall* Fn_cuInit)(unsigned int);
typedef TestCUresult(__stdcall* Fn_cuDeviceGet)(TestCUdevice*, int);
typedef TestCUresult(__stdcall* Fn_cuDevicePrimaryCtxRetain)(void**, TestCUdevice);
typedef TestCUresult(__stdcall* Fn_cuCtxSetCurrent)(void*);
typedef TestCUresult(__stdcall* Fn_cuMemAlloc)(TestCUdeviceptr*, size_t);
typedef TestCUresult(__stdcall* Fn_cuMemFree)(TestCUdeviceptr);
typedef TestCUresult(__stdcall* Fn_cuMemcpyHtoD)(TestCUdeviceptr, const void*, size_t);
typedef TestCUresult(__stdcall* Fn_cuMemcpyDtoH)(void*, TestCUdeviceptr, size_t);
typedef TestCUresult(__stdcall* Fn_cuCtxSynchronize)();
typedef TestCUresult(__stdcall* Fn_cuStreamCreate)(TestCUstream*, unsigned int);
typedef TestCUresult(__stdcall* Fn_cuStreamSynchronize)(TestCUstream);
typedef TestCUresult(__stdcall* Fn_cuStreamDestroy)(TestCUstream);

struct TestCudaRuntime {
  HMODULE dll = nullptr;
  void* ctx = nullptr;
  Fn_cuInit init = nullptr;
  Fn_cuDeviceGet deviceGet = nullptr;
  Fn_cuDevicePrimaryCtxRetain retain = nullptr;
  Fn_cuCtxSetCurrent setCurrent = nullptr;
  Fn_cuMemAlloc memAlloc = nullptr;
  Fn_cuMemFree memFree = nullptr;
  Fn_cuMemcpyHtoD htoD = nullptr;
  Fn_cuMemcpyDtoH dtoH = nullptr;
  Fn_cuCtxSynchronize sync = nullptr;
  Fn_cuStreamCreate streamCreate = nullptr;
  Fn_cuStreamSynchronize streamSync = nullptr;
  Fn_cuStreamDestroy streamDestroy = nullptr;
  bool available = false;

  void load() {
    dll = LoadLibraryA("nvcuda.dll");
    if (!dll) return;
    init = (Fn_cuInit)GetProcAddress(dll, "cuInit");
    deviceGet = (Fn_cuDeviceGet)GetProcAddress(dll, "cuDeviceGet");
    retain = (Fn_cuDevicePrimaryCtxRetain)GetProcAddress(dll, "cuDevicePrimaryCtxRetain");
    setCurrent = (Fn_cuCtxSetCurrent)GetProcAddress(dll, "cuCtxSetCurrent");
    memAlloc = (Fn_cuMemAlloc)GetProcAddress(dll, "cuMemAlloc_v2");
    if (!memAlloc) memAlloc = (Fn_cuMemAlloc)GetProcAddress(dll, "cuMemAlloc");
    memFree = (Fn_cuMemFree)GetProcAddress(dll, "cuMemFree_v2");
    if (!memFree) memFree = (Fn_cuMemFree)GetProcAddress(dll, "cuMemFree");
    htoD = (Fn_cuMemcpyHtoD)GetProcAddress(dll, "cuMemcpyHtoD_v2");
    if (!htoD) htoD = (Fn_cuMemcpyHtoD)GetProcAddress(dll, "cuMemcpyHtoD");
    dtoH = (Fn_cuMemcpyDtoH)GetProcAddress(dll, "cuMemcpyDtoH_v2");
    if (!dtoH) dtoH = (Fn_cuMemcpyDtoH)GetProcAddress(dll, "cuMemcpyDtoH");
    sync = (Fn_cuCtxSynchronize)GetProcAddress(dll, "cuCtxSynchronize");
    streamCreate = (Fn_cuStreamCreate)GetProcAddress(dll, "cuStreamCreate");
    streamSync = (Fn_cuStreamSynchronize)GetProcAddress(dll, "cuStreamSynchronize");
    streamDestroy = (Fn_cuStreamDestroy)GetProcAddress(dll, "cuStreamDestroy");
    if (!init || !deviceGet || !retain || !setCurrent || !memAlloc || !memFree ||
        !htoD || !dtoH || !sync || !streamCreate || !streamSync || !streamDestroy) {
      return;
    }
    if (init(0) != TEST_CUDA_SUCCESS) return;
    TestCUdevice device = 0;
    if (deviceGet(&device, 0) != TEST_CUDA_SUCCESS) return;
    if (retain(&ctx, device) != TEST_CUDA_SUCCESS) return;
    if (setCurrent(ctx) != TEST_CUDA_SUCCESS) return;
    available = true;
  }
};

TestCudaRuntime gTestCuda;
#endif


void check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++gFailures;
  }
}

void checkNear(float actual, float expected, const char* what,
               float tolerance = 2.0e-4f) {
  if (std::fabs(actual - expected) > tolerance) {
    std::fprintf(stderr, "FAIL: %s (got %g expected %g)\n", what,
                 static_cast<double>(actual), static_cast<double>(expected));
    ++gFailures;
  }
}

struct PropertySet {
  struct Value {
    enum Type { None, String, Double, Int, Pointer } type = None;
    std::vector<std::string> strings;
    std::vector<double> doubles;
    std::vector<int> ints;
    std::vector<void*> pointers;
  };
  std::map<std::string, Value> values;
};

PropertySet* asSet(OfxPropertySetHandle handle) {
  return reinterpret_cast<PropertySet*>(handle);
}

OfxPropertySetHandle asHandle(PropertySet* set) {
  return reinterpret_cast<OfxPropertySetHandle>(set);
}

void setString(PropertySet* set, const char* name, const std::string& value,
               int index = 0) {
  PropertySet::Value& v = set->values[name];
  if (v.type != PropertySet::Value::String) {
    v = PropertySet::Value();
    v.type = PropertySet::Value::String;
  }
  if (static_cast<int>(v.strings.size()) <= index) {
    v.strings.resize(index + 1);
  }
  v.strings[index] = value;
}

void setInt(PropertySet* set, const char* name, int value, int index = 0) {
  PropertySet::Value& v = set->values[name];
  if (v.type != PropertySet::Value::Int) {
    v = PropertySet::Value();
    v.type = PropertySet::Value::Int;
  }
  if (static_cast<int>(v.ints.size()) <= index) {
    v.ints.resize(index + 1);
  }
  v.ints[index] = value;
}

void setDouble(PropertySet* set, const char* name, double value,
               int index = 0) {
  PropertySet::Value& v = set->values[name];
  if (v.type != PropertySet::Value::Double) {
    v = PropertySet::Value();
    v.type = PropertySet::Value::Double;
  }
  if (static_cast<int>(v.doubles.size()) <= index) {
    v.doubles.resize(index + 1);
  }
  v.doubles[index] = value;
}

void setPointer(PropertySet* set, const char* name, void* value) {
  PropertySet::Value& v = set->values[name];
  if (v.type != PropertySet::Value::Pointer) {
    v = PropertySet::Value();
    v.type = PropertySet::Value::Pointer;
  }
  v.pointers.resize(1);
  v.pointers[0] = value;
}

std::string getString(PropertySet* set, const char* name, int index = 0) {
  auto it = set->values.find(name);
  if (it == set->values.end() || it->second.type != PropertySet::Value::String ||
      static_cast<int>(it->second.strings.size()) <= index) {
    return std::string();
  }
  return it->second.strings[index];
}

int getInt(PropertySet* set, const char* name, int index = 0) {
  auto it = set->values.find(name);
  if (it == set->values.end() || it->second.type != PropertySet::Value::Int ||
      static_cast<int>(it->second.ints.size()) <= index) {
    return 0;
  }
  return it->second.ints[index];
}

double getDouble(PropertySet* set, const char* name, int index = 0) {
  auto it = set->values.find(name);
  if (it == set->values.end() || it->second.type != PropertySet::Value::Double ||
      static_cast<int>(it->second.doubles.size()) <= index) {
    return 0.0;
  }
  return it->second.doubles[index];
}

// ---------------------------------------------------------------------------
// Property suite
// ---------------------------------------------------------------------------
OfxStatus propSetPointer(OfxPropertySetHandle properties, const char* property,
                         int index, void* value) {
  if (!properties || !property) {
    return kOfxStatErrBadHandle;
  }
  if (index < 0) {
    return kOfxStatErrBadIndex;
  }
  PropertySet::Value& v = asSet(properties)->values[property];
  if (v.type != PropertySet::Value::Pointer) {
    v = PropertySet::Value();
    v.type = PropertySet::Value::Pointer;
  }
  if (static_cast<int>(v.pointers.size()) <= index) {
    v.pointers.resize(index + 1);
  }
  v.pointers[index] = value;
  return kOfxStatOK;
}

OfxStatus propSetString(OfxPropertySetHandle properties, const char* property,
                        int index, const char* value) {
  if (!properties || !property) {
    return kOfxStatErrBadHandle;
  }
  if (index < 0) {
    return kOfxStatErrBadIndex;
  }
  setString(asSet(properties), property, value ? value : "", index);
  return kOfxStatOK;
}

OfxStatus propSetDouble(OfxPropertySetHandle properties, const char* property,
                        int index, double value) {
  if (!properties || !property) {
    return kOfxStatErrBadHandle;
  }
  if (index < 0) {
    return kOfxStatErrBadIndex;
  }
  setDouble(asSet(properties), property, value, index);
  return kOfxStatOK;
}

OfxStatus propSetInt(OfxPropertySetHandle properties, const char* property,
                     int index, int value) {
  if (!properties || !property) {
    return kOfxStatErrBadHandle;
  }
  if (index < 0) {
    return kOfxStatErrBadIndex;
  }
  setInt(asSet(properties), property, value, index);
  return kOfxStatOK;
}

OfxStatus propSetPointerN(OfxPropertySetHandle properties, const char* property,
                          int count, void* const* value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  PropertySet::Value& v = asSet(properties)->values[property];
  v = PropertySet::Value();
  v.type = PropertySet::Value::Pointer;
  v.pointers.assign(value, value + count);
  return kOfxStatOK;
}

OfxStatus propSetStringN(OfxPropertySetHandle properties, const char* property,
                         int count, const char* const* value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  PropertySet::Value& v = asSet(properties)->values[property];
  v = PropertySet::Value();
  v.type = PropertySet::Value::String;
  v.strings.reserve(count);
  for (int i = 0; i < count; ++i) {
    v.strings.emplace_back(value[i] ? value[i] : "");
  }
  return kOfxStatOK;
}

OfxStatus propSetDoubleN(OfxPropertySetHandle properties, const char* property,
                         int count, const double* value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  PropertySet::Value& v = asSet(properties)->values[property];
  v = PropertySet::Value();
  v.type = PropertySet::Value::Double;
  v.doubles.assign(value, value + count);
  return kOfxStatOK;
}

OfxStatus propSetIntN(OfxPropertySetHandle properties, const char* property,
                      int count, const int* value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  PropertySet::Value& v = asSet(properties)->values[property];
  v = PropertySet::Value();
  v.type = PropertySet::Value::Int;
  v.ints.assign(value, value + count);
  return kOfxStatOK;
}

OfxStatus propGetPointer(OfxPropertySetHandle properties, const char* property,
                         int index, void** value) {
  if (!properties || !property || !value || index < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::Pointer ||
      static_cast<int>(it->second.pointers.size()) <= index) {
    *value = nullptr;
    return kOfxStatErrUnknown;
  }
  *value = it->second.pointers[index];
  return kOfxStatOK;
}

OfxStatus propGetString(OfxPropertySetHandle properties, const char* property,
                        int index, char** value) {
  if (!properties || !property || !value || index < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::String ||
      static_cast<int>(it->second.strings.size()) <= index) {
    *value = nullptr;
    return kOfxStatErrUnknown;
  }
  *value = const_cast<char*>(it->second.strings[index].c_str());
  return kOfxStatOK;
}

OfxStatus propGetDouble(OfxPropertySetHandle properties, const char* property,
                        int index, double* value) {
  if (!properties || !property || !value || index < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::Double ||
      static_cast<int>(it->second.doubles.size()) <= index) {
    *value = 0.0;
    return kOfxStatErrUnknown;
  }
  *value = it->second.doubles[index];
  return kOfxStatOK;
}

OfxStatus propGetInt(OfxPropertySetHandle properties, const char* property,
                     int index, int* value) {
  if (!properties || !property || !value || index < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::Int ||
      static_cast<int>(it->second.ints.size()) <= index) {
    *value = 0;
    return kOfxStatErrUnknown;
  }
  *value = it->second.ints[index];
  return kOfxStatOK;
}

OfxStatus propGetPointerN(OfxPropertySetHandle properties, const char* property,
                          int count, void** value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::Pointer ||
      static_cast<int>(it->second.pointers.size()) < count) {
    return kOfxStatErrUnknown;
  }
  for (int i = 0; i < count; ++i) {
    value[i] = it->second.pointers[i];
  }
  return kOfxStatOK;
}

OfxStatus propGetStringN(OfxPropertySetHandle properties, const char* property,
                         int count, char** value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::String ||
      static_cast<int>(it->second.strings.size()) < count) {
    return kOfxStatErrUnknown;
  }
  for (int i = 0; i < count; ++i) {
    value[i] = const_cast<char*>(it->second.strings[i].c_str());
  }
  return kOfxStatOK;
}

OfxStatus propGetDoubleN(OfxPropertySetHandle properties, const char* property,
                         int count, double* value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::Double ||
      static_cast<int>(it->second.doubles.size()) < count) {
    return kOfxStatErrUnknown;
  }
  for (int i = 0; i < count; ++i) {
    value[i] = it->second.doubles[i];
  }
  return kOfxStatOK;
}

OfxStatus propGetIntN(OfxPropertySetHandle properties, const char* property,
                      int count, int* value) {
  if (!properties || !property || count < 0) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end() ||
      it->second.type != PropertySet::Value::Int ||
      static_cast<int>(it->second.ints.size()) < count) {
    return kOfxStatErrUnknown;
  }
  for (int i = 0; i < count; ++i) {
    value[i] = it->second.ints[i];
  }
  return kOfxStatOK;
}

OfxStatus propReset(OfxPropertySetHandle properties, const char* property) {
  if (!properties || !property) {
    return kOfxStatErrBadHandle;
  }
  asSet(properties)->values.erase(property);
  return kOfxStatOK;
}

OfxStatus propGetDimension(OfxPropertySetHandle properties, const char* property,
                           int* count) {
  if (!properties || !property || !count) {
    return kOfxStatErrBadHandle;
  }
  auto it = asSet(properties)->values.find(property);
  if (it == asSet(properties)->values.end()) {
    *count = 0;
    return kOfxStatErrUnknown;
  }
  switch (it->second.type) {
    case PropertySet::Value::String:
      *count = static_cast<int>(it->second.strings.size());
      break;
    case PropertySet::Value::Double:
      *count = static_cast<int>(it->second.doubles.size());
      break;
    case PropertySet::Value::Int:
      *count = static_cast<int>(it->second.ints.size());
      break;
    case PropertySet::Value::Pointer:
      *count = static_cast<int>(it->second.pointers.size());
      break;
    default:
      *count = 0;
      break;
  }
  return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Parameter suite
// ---------------------------------------------------------------------------
struct ParamDef {
  std::string name;
  std::string type;
  PropertySet* props = nullptr;
  bool explicitInt = false;
  bool explicitDouble = false;
  bool explicitString = false;
  int intValue = 0;
  double doubleValue = 0.0;
  std::string stringValue;
  std::map<double, int> intKeys;
  std::map<double, double> doubleKeys;
};

struct ParamSet {
  std::map<std::string, ParamDef*> params;
  ~ParamSet() {
    for (auto& entry : params) {
      delete entry.second->props;
      delete entry.second;
    }
  }
};

ParamDef* asParam(OfxParamHandle handle) {
  return reinterpret_cast<ParamDef*>(handle);
}

int resolvedInt(ParamDef* param) {
  if (param->explicitInt) {
    return param->intValue;
  }
  return getInt(param->props, kOfxParamPropDefault);
}

double resolvedDouble(ParamDef* param) {
  if (param->explicitDouble) {
    return param->doubleValue;
  }
  return getDouble(param->props, kOfxParamPropDefault);
}

const std::string& resolvedString(ParamDef* param) {
  if (param->explicitString) {
    return param->stringValue;
  }
  auto it = param->props->values.find(kOfxParamPropDefault);
  if (it != param->props->values.end() &&
      it->second.type == PropertySet::Value::String &&
      !it->second.strings.empty()) {
    return it->second.strings[0];
  }
  static const std::string empty;
  return empty;
}

OfxStatus paramDefine(OfxParamSetHandle paramSet, const char* paramType,
                      const char* name, OfxPropertySetHandle* propertySet) {
  if (!paramSet || !paramType || !name) {
    return kOfxStatErrBadHandle;
  }
  auto* set = reinterpret_cast<ParamSet*>(paramSet);
  if (set->params.count(name)) {
    return kOfxStatErrExists;
  }
  auto* param = new ParamDef();
  param->name = name;
  param->type = paramType;
  param->props = new PropertySet();
  set->params[name] = param;
  if (propertySet) {
    *propertySet = reinterpret_cast<OfxPropertySetHandle>(param->props);
  }
  return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle paramSet, const char* name,
                         OfxParamHandle* param, OfxPropertySetHandle* propertySet) {
  if (!paramSet || !name || !param) {
    return kOfxStatErrBadHandle;
  }
  auto* set = reinterpret_cast<ParamSet*>(paramSet);
  auto it = set->params.find(name);
  if (it == set->params.end()) {
    return kOfxStatErrUnknown;
  }
  *param = reinterpret_cast<OfxParamHandle>(it->second);
  if (propertySet) {
    *propertySet = reinterpret_cast<OfxPropertySetHandle>(it->second->props);
  }
  return kOfxStatOK;
}

OfxStatus paramGetPropertySet(OfxParamHandle param,
                              OfxPropertySetHandle* propertySet) {
  if (!param || !propertySet) {
    return kOfxStatErrBadHandle;
  }
  *propertySet = reinterpret_cast<OfxPropertySetHandle>(asParam(param)->props);
  return kOfxStatOK;
}

OfxStatus paramGetValue(OfxParamHandle paramHandle, ...) {
  if (!paramHandle) {
    return kOfxStatErrBadHandle;
  }
  ParamDef* param = asParam(paramHandle);
  va_list args;
  va_start(args, paramHandle);

  if (param->type == kOfxParamTypeString) {
    char** out = va_arg(args, char**);
    if (out) {
      *out = const_cast<char*>(resolvedString(param).c_str());
    }
  } else if (param->type == kOfxParamTypeRGB) {
    double* r = va_arg(args, double*);
    double* g = va_arg(args, double*);
    double* b = va_arg(args, double*);
    if (r) *r = getDouble(param->props, kOfxParamPropDefault, 0);
    if (g) *g = getDouble(param->props, kOfxParamPropDefault, 1);
    if (b) *b = getDouble(param->props, kOfxParamPropDefault, 2);
  } else if (param->type == kOfxParamTypeDouble) {
    double* out = va_arg(args, double*);
    if (out) {
      *out = resolvedDouble(param);
    }
  } else {
    int* out = va_arg(args, int*);
    if (out) {
      *out = resolvedInt(param);
    }
  }

  va_end(args);
  return kOfxStatOK;
}

OfxStatus paramGetValueAtTime(OfxParamHandle paramHandle, OfxTime time, ...) {
  if (!paramHandle) {
    return kOfxStatErrBadHandle;
  }
  ParamDef* param = asParam(paramHandle);
  va_list args;
  va_start(args, time);

  if (param->type == kOfxParamTypeString) {
    char** out = va_arg(args, char**);
    if (out) {
      *out = const_cast<char*>(resolvedString(param).c_str());
    }
  } else if (param->type == kOfxParamTypeRGB) {
    double* r = va_arg(args, double*);
    double* g = va_arg(args, double*);
    double* b = va_arg(args, double*);
    if (r) *r = getDouble(param->props, kOfxParamPropDefault, 0);
    if (g) *g = getDouble(param->props, kOfxParamPropDefault, 1);
    if (b) *b = getDouble(param->props, kOfxParamPropDefault, 2);
  } else if (param->type == kOfxParamTypeDouble) {
    double* out = va_arg(args, double*);
    if (out) {
      auto key = param->doubleKeys.find(time);
      *out = key != param->doubleKeys.end() ? key->second : resolvedDouble(param);
    }
  } else {
    int* out = va_arg(args, int*);
    if (out) {
      auto key = param->intKeys.find(time);
      *out = key != param->intKeys.end() ? key->second : resolvedInt(param);
    }
  }

  va_end(args);
  return kOfxStatOK;
}

// ---------------------------------------------------------------------------
// Mock clips and image effect suite
// ---------------------------------------------------------------------------
struct FakeImage {
  std::vector<float> data;
  int x1 = 0;
  int y1 = 0;
  int x2 = 0;
  int y2 = 0;
  int rowBytes = 0;
  bool opaque = true;
  float par = 1.0f;
  void* deviceData = nullptr;
};

struct ClipDef {
  std::string name;
  PropertySet* props = nullptr;
  FakeImage* image = nullptr;
};

struct Effect {
  PropertySet* props = nullptr;
  ParamSet* params = nullptr;
  std::map<std::string, ClipDef*> clips;

  ~Effect() {
    for (auto& entry : clips) {
      delete entry.second->props;
      delete entry.second;
    }
    delete props;
    delete params;
  }
};

Effect* asEffect(OfxImageEffectHandle handle) {
  return reinterpret_cast<Effect*>(handle);
}

OfxStatus effectGetPropertySet(OfxImageEffectHandle effect,
                               OfxPropertySetHandle* propertySet) {
  if (!effect || !propertySet) {
    return kOfxStatErrBadHandle;
  }
  *propertySet = reinterpret_cast<OfxPropertySetHandle>(asEffect(effect)->props);
  return kOfxStatOK;
}

OfxStatus effectGetParamSet(OfxImageEffectHandle effect,
                            OfxParamSetHandle* paramSet) {
  if (!effect || !paramSet) {
    return kOfxStatErrBadHandle;
  }
  *paramSet = reinterpret_cast<OfxParamSetHandle>(asEffect(effect)->params);
  return kOfxStatOK;
}

OfxStatus clipDefine(OfxImageEffectHandle effect, const char* name,
                     OfxPropertySetHandle* propertySet) {
  if (!effect || !name) {
    return kOfxStatErrBadHandle;
  }
  Effect* fx = asEffect(effect);
  auto it = fx->clips.find(name);
  if (it != fx->clips.end()) {
    if (propertySet) {
      *propertySet = reinterpret_cast<OfxPropertySetHandle>(it->second->props);
    }
    return kOfxStatOK;
  }
  auto* clip = new ClipDef();
  clip->name = name;
  clip->props = new PropertySet();
  fx->clips[name] = clip;
  if (propertySet) {
    *propertySet = reinterpret_cast<OfxPropertySetHandle>(clip->props);
  }
  return kOfxStatOK;
}

OfxStatus clipGetHandle(OfxImageEffectHandle effect, const char* name,
                        OfxImageClipHandle* clip,
                        OfxPropertySetHandle* propertySet) {
  if (!effect || !name || !clip) {
    return kOfxStatErrBadHandle;
  }
  Effect* fx = asEffect(effect);
  auto it = fx->clips.find(name);
  if (it == fx->clips.end()) {
    return kOfxStatErrUnknown;
  }
  *clip = reinterpret_cast<OfxImageClipHandle>(it->second);
  if (propertySet) {
    *propertySet = reinterpret_cast<OfxPropertySetHandle>(it->second->props);
  }
  return kOfxStatOK;
}

OfxStatus clipGetPropertySet(OfxImageClipHandle clip,
                             OfxPropertySetHandle* propertySet) {
  if (!clip || !propertySet) {
    return kOfxStatErrBadHandle;
  }
  *propertySet = reinterpret_cast<OfxPropertySetHandle>(
      reinterpret_cast<ClipDef*>(clip)->props);
  return kOfxStatOK;
}

OfxStatus clipGetImage(OfxImageClipHandle clip, OfxTime /*time*/,
                       const OfxRectD* /*region*/,
                       OfxPropertySetHandle* imageHandle) {
  if (!clip || !imageHandle) {
    return kOfxStatErrBadHandle;
  }
  ClipDef* def = reinterpret_cast<ClipDef*>(clip);
  if (!def->image) {
    return kOfxStatFailed;
  }
  FakeImage* image = def->image;
  auto* props = new PropertySet();

  setString(props, kOfxImageEffectPropPixelDepth, kOfxBitDepthFloat);
  setString(props, kOfxImageEffectPropComponents, kOfxImageComponentRGBA);
  const int bounds[4] = {image->x1, image->y1, image->x2, image->y2};
  setInt(props, kOfxImagePropBounds, bounds[0], 0);
  setInt(props, kOfxImagePropBounds, bounds[1], 1);
  setInt(props, kOfxImagePropBounds, bounds[2], 2);
  setInt(props, kOfxImagePropBounds, bounds[3], 3);
  setInt(props, kOfxImagePropRowBytes, image->rowBytes);
  setPointer(props, kOfxImagePropData,
             image->deviceData ? image->deviceData : image->data.data());
  setString(props, kOfxImageEffectPropPreMultiplication,
            image->opaque ? kOfxImageOpaque : kOfxImageUnPreMultiplied);
  setDouble(props, kOfxImagePropPixelAspectRatio, image->par);

  *imageHandle = reinterpret_cast<OfxPropertySetHandle>(props);
  return kOfxStatOK;
}

OfxStatus clipReleaseImage(OfxPropertySetHandle imageHandle) {
  delete asSet(imageHandle);
  return kOfxStatOK;
}

OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle clip,
                                     OfxTime /*time*/, OfxRectD* bounds) {
  if (!clip || !bounds) {
    return kOfxStatErrBadHandle;
  }
  ClipDef* def = reinterpret_cast<ClipDef*>(clip);
  if (!def->image) {
    return kOfxStatFailed;
  }
  bounds->x1 = def->image->x1;
  bounds->y1 = def->image->y1;
  bounds->x2 = def->image->x2;
  bounds->y2 = def->image->y2;
  return kOfxStatOK;
}

int effectAbort(OfxImageEffectHandle /*effect*/) { return 0; }

// ---------------------------------------------------------------------------
// Host
// ---------------------------------------------------------------------------
OfxPropertySuiteV1 gPropSuite{};
OfxParameterSuiteV1 gParamSuite{};
OfxImageEffectSuiteV1 gEffectSuite{};
OfxMessageSuiteV1 gMessageSuite{};
PropertySet* gHostProperties = nullptr;

void initSuites() {
  gPropSuite.propSetPointer = &propSetPointer;
  gPropSuite.propSetString = &propSetString;
  gPropSuite.propSetDouble = &propSetDouble;
  gPropSuite.propSetInt = &propSetInt;
  gPropSuite.propSetPointerN = &propSetPointerN;
  gPropSuite.propSetStringN = &propSetStringN;
  gPropSuite.propSetDoubleN = &propSetDoubleN;
  gPropSuite.propSetIntN = &propSetIntN;
  gPropSuite.propGetPointer = &propGetPointer;
  gPropSuite.propGetString = &propGetString;
  gPropSuite.propGetDouble = &propGetDouble;
  gPropSuite.propGetInt = &propGetInt;
  gPropSuite.propGetPointerN = &propGetPointerN;
  gPropSuite.propGetStringN = &propGetStringN;
  gPropSuite.propGetDoubleN = &propGetDoubleN;
  gPropSuite.propGetIntN = &propGetIntN;
  gPropSuite.propReset = &propReset;
  gPropSuite.propGetDimension = &propGetDimension;

  gParamSuite.paramDefine = &paramDefine;
  gParamSuite.paramGetHandle = &paramGetHandle;
  gParamSuite.paramGetPropertySet = &paramGetPropertySet;
  gParamSuite.paramGetValue = &paramGetValue;
  gParamSuite.paramGetValueAtTime = &paramGetValueAtTime;

  gEffectSuite.getPropertySet = &effectGetPropertySet;
  gEffectSuite.getParamSet = &effectGetParamSet;
  gEffectSuite.clipDefine = &clipDefine;
  gEffectSuite.clipGetHandle = &clipGetHandle;
  gEffectSuite.clipGetPropertySet = &clipGetPropertySet;
  gEffectSuite.clipGetImage = &clipGetImage;
  gEffectSuite.clipReleaseImage = &clipReleaseImage;
  gEffectSuite.clipGetRegionOfDefinition = &clipGetRegionOfDefinition;
  gEffectSuite.abort = &effectAbort;

  gHostProperties = new PropertySet();
  setString(gHostProperties, kOfxPropName, "MixedMediaMockHost");
  setInt(gHostProperties, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 1);
}

const void* fetchSuite(OfxPropertySetHandle /*host*/, const char* suiteName,
                       int version) {
  if (!suiteName || version != 1) {
    return nullptr;
  }
  if (std::strcmp(suiteName, kOfxPropertySuite) == 0) {
    return &gPropSuite;
  }
  if (std::strcmp(suiteName, kOfxParameterSuite) == 0) {
    return &gParamSuite;
  }
  if (std::strcmp(suiteName, kOfxImageEffectSuite) == 0) {
    return &gEffectSuite;
  }
  if (std::strcmp(suiteName, kOfxMessageSuite) == 0) {
    return &gMessageSuite;
  }
  return nullptr;
}

void setParamInt(ParamSet* set, const char* name, int value) {
  auto it = set->params.find(name);
  if (it != set->params.end()) {
    it->second->explicitInt = true;
    it->second->intValue = value;
  }
}

void setParamDouble(ParamSet* set, const char* name, double value) {
  auto it = set->params.find(name);
  if (it != set->params.end()) {
    it->second->explicitDouble = true;
    it->second->doubleValue = value;
  }
}

void setParamIntAtTime(ParamSet* set, const char* name, double time, int value) {
  auto it = set->params.find(name);
  if (it != set->params.end()) {
    it->second->intKeys[time] = value;
  }
}

void setParamDoubleAtTime(ParamSet* set, const char* name, double time,
                          double value) {
  auto it = set->params.find(name);
  if (it != set->params.end()) {
    it->second->doubleKeys[time] = value;
  }
}

void setParamString(ParamSet* set, const char* name, const std::string& value) {
  auto it = set->params.find(name);
  if (it != set->params.end()) {
    it->second->explicitString = true;
    it->second->stringValue = value;
  }
}

void writeLe16(std::ofstream& out, uint16_t value) {
  char bytes[2] = {static_cast<char>(value & 0xff),
                   static_cast<char>((value >> 8) & 0xff)};
  out.write(bytes, 2);
}

void writeLe32(std::ofstream& out, uint32_t value) {
  char bytes[4] = {static_cast<char>(value & 0xff),
                   static_cast<char>((value >> 8) & 0xff),
                   static_cast<char>((value >> 16) & 0xff),
                   static_cast<char>((value >> 24) & 0xff)};
  out.write(bytes, 4);
}

std::filesystem::path makeTestImage() {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "mixedmedia_ofx_host_test.bmp";

  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return path;
  }

  const uint32_t rowBytes = 4;  // one 24-bit pixel padded to 4 bytes
  const uint32_t fileSize = 14 + 40 + rowBytes;
  out.put('B');
  out.put('M');
  writeLe32(out, fileSize);
  writeLe16(out, 0);
  writeLe16(out, 0);
  writeLe32(out, 54);
  writeLe32(out, 40);
  writeLe32(out, 1);
  writeLe32(out, 1);
  writeLe16(out, 1);
  writeLe16(out, 24);
  writeLe32(out, 0);
  writeLe32(out, rowBytes);
  writeLe32(out, 2835);
  writeLe32(out, 2835);
  writeLe32(out, 0);
  writeLe32(out, 0);

  // BGR order: RGB (0.8, 0.4, 0.2).
  const unsigned char pixel[4] = {51, 102, 204, 0};
  out.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
  return path;
}

OfxHost gOfxHost{};

void fillSource(FakeImage& source, int width, int height) {
  source.x1 = 0;
  source.y1 = 0;
  source.x2 = width;
  source.y2 = height;
  source.rowBytes = width * 16;
  source.opaque = true;
  source.data.resize(static_cast<size_t>(width) * height * 4);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float* p =
          source.data.data() + (static_cast<size_t>(y) * width + x) * 4;
      p[0] = static_cast<float>(x) / static_cast<float>(width - 1);
      p[1] = static_cast<float>(y) / static_cast<float>(height - 1);
      p[2] = 0.3f + 0.2f * static_cast<float>(x + y) /
                         static_cast<float>(width + height);
      p[3] = 1.0f;
    }
  }
}

void setParamRgb(ParamSet* set, const char* name, double r, double g, double b) {
  auto it = set->params.find(name);
  if (it == set->params.end()) {
    return;
  }
  setDouble(it->second->props, kOfxParamPropDefault, r, 0);
  setDouble(it->second->props, kOfxParamPropDefault, g, 1);
  setDouble(it->second->props, kOfxParamPropDefault, b, 2);
}

ParamDef* findParam(Effect* effect, const char* name) {
  if (!effect || !effect->params) {
    return nullptr;
  }
  auto it = effect->params->params.find(name);
  return it == effect->params->params.end() ? nullptr : it->second;
}

bool allFinite(const std::vector<float>& data) {
  for (float v : data) {
    if (!std::isfinite(v)) {
      return false;
    }
  }
  return true;
}

Effect* makeInstance(Effect* descriptor) {
  auto* instance = new Effect();
  instance->props = new PropertySet();
  instance->params = descriptor->params;
  descriptor->params = nullptr;
  for (auto& clip : descriptor->clips) {
    instance->clips[clip.first] = clip.second;
  }
  descriptor->clips.clear();
  return instance;
}

void setInstanceProperties(Effect* instance) {
  setDouble(instance->props, kOfxImageEffectPropProjectSize, 128.0, 0);
  setDouble(instance->props, kOfxImageEffectPropProjectSize, 128.0, 1);
  setDouble(instance->props, kOfxImageEffectPropProjectOffset, 0.0, 0);
  setDouble(instance->props, kOfxImageEffectPropProjectOffset, 0.0, 1);
  setDouble(instance->props, kOfxImagePropPixelAspectRatio, 1.0);
}

PropertySet* makeRenderArgs(int width, int height, double time) {
  PropertySet* args = new PropertySet();
  setDouble(args, kOfxPropTime, time);
  const int window[4] = {0, 0, width, height};
  setInt(args, kOfxImageEffectPropRenderWindow, window[0], 0);
  setInt(args, kOfxImageEffectPropRenderWindow, window[1], 1);
  setInt(args, kOfxImageEffectPropRenderWindow, window[2], 2);
  setInt(args, kOfxImageEffectPropRenderWindow, window[3], 3);
  setDouble(args, kOfxImageEffectPropRenderScale, 1.0, 0);
  setDouble(args, kOfxImageEffectPropRenderScale, 1.0, 1);
  return args;
}

bool renderOnce(OfxPluginEntryPoint* entry, Effect* instance,
                FakeImage& source, FakeImage& output, PropertySet* args,
                std::vector<float>& result) {
  (void)source;
  output.data.assign(static_cast<size_t>(output.x2 - output.x1) *
                         static_cast<size_t>(output.y2 - output.y1) * 4,
                     0.0f);
  const OfxStatus status =
      entry(kOfxImageEffectActionRender, instance, asHandle(args), nullptr);
  result = output.data;
  if (status != kOfxStatOK) {
    std::fprintf(stderr, "FAIL: render returned %d\n", (int)status);
    ++gFailures;
    return false;
  }
  return true;
}

bool runCudaPath(OfxPluginEntryPoint* entry, Effect* instance,
                 FakeImage& source, FakeImage& output, PropertySet* renderArgs,
                 const std::vector<float>& cpuResult, const char* what) {
#ifdef _WIN32
  if (!gTestCuda.available) {
    std::printf("%s: CUDA not available, host CUDA path skipped\n", what);
    return true;
  }
  const size_t count = source.data.size();
  const size_t bytes = count * sizeof(float);
  TestCUdeviceptr devSrc = 0;
  TestCUdeviceptr devDst = 0;
  if (gTestCuda.memAlloc(&devSrc, bytes) != TEST_CUDA_SUCCESS ||
      gTestCuda.memAlloc(&devDst, bytes) != TEST_CUDA_SUCCESS) {
    std::fprintf(stderr, "FAIL: %s CUDA allocation\n", what);
    ++gFailures;
    return false;
  }
  if (gTestCuda.htoD(devSrc, source.data.data(), bytes) != TEST_CUDA_SUCCESS) {
    std::fprintf(stderr, "FAIL: %s CUDA upload\n", what);
    ++gFailures;
    gTestCuda.memFree(devSrc);
    gTestCuda.memFree(devDst);
    return false;
  }

  TestCUstream stream = nullptr;
  bool hasStream = gTestCuda.streamCreate(&stream, 0) == TEST_CUDA_SUCCESS && stream;
  source.deviceData = (void*)devSrc;
  output.deviceData = (void*)devDst;
  setInt(renderArgs, kOfxImageEffectPropCudaEnabled, 1);
  if (hasStream) {
    setPointer(renderArgs, kOfxImageEffectPropCudaStream, stream);
  }
  output.data.assign(count, 0.0f);
  const OfxStatus status =
      entry(kOfxImageEffectActionRender, instance, asHandle(renderArgs), nullptr);
  if (hasStream) {
    gTestCuda.streamSync(stream);
  } else {
    gTestCuda.sync();
  }
  if (status != kOfxStatOK) {
    std::fprintf(stderr, "FAIL: %s CUDA render returned %d\n", what, (int)status);
    ++gFailures;
  } else {
    gTestCuda.dtoH(output.data.data(), devDst, bytes);
    float maxDiff = 0.0f;
    for (size_t i = 0; i < count; ++i) {
      maxDiff = std::max(maxDiff, std::fabs(output.data[i] - cpuResult[i]));
    }
    std::printf("%s: CPU/CUDA host-path max diff %.6f (%s stream)\n", what,
                maxDiff, hasStream ? "with" : "without");
    if (maxDiff > 2.5e-3f) {
      std::fprintf(stderr, "FAIL: %s CUDA host-path parity\n", what);
      ++gFailures;
    }
  }

  propReset(asHandle(renderArgs), kOfxImageEffectPropCudaEnabled);
  if (hasStream) {
    propReset(asHandle(renderArgs), kOfxImageEffectPropCudaStream);
    gTestCuda.streamDestroy(stream);
  }
  source.deviceData = nullptr;
  output.deviceData = nullptr;
  gTestCuda.memFree(devSrc);
  gTestCuda.memFree(devDst);
  return status == kOfxStatOK;
#else
  (void)entry; (void)instance; (void)source; (void)output;
  (void)renderArgs; (void)cpuResult; (void)what;
  return true;
#endif
}

bool checkNonIdentity(const std::vector<float>& source,
                      const std::vector<float>& result, const char* what) {
  size_t different = 0;
  for (size_t i = 0; i < source.size(); ++i) {
    if (std::fabs(source[i] - result[i]) > 1.0e-5f) {
      ++different;
    }
  }
  check(different > source.size() / 10, what);
  return different > 0;
}

void testPaper(OfxPlugin* plugin, OfxPluginEntryPoint* entry) {
  plugin->setHost(&gOfxHost);
  check(entry(kOfxActionLoad, nullptr, nullptr, nullptr) == kOfxStatOK,
        "paper load");

  auto* descriptor = new Effect();
  descriptor->props = new PropertySet();
  descriptor->params = new ParamSet();
  entry(kOfxActionDescribe, descriptor, nullptr, nullptr);
  check(getString(descriptor->props, kOfxPropLabel) == "Paper Cutout",
        "paper label");
  check(getString(descriptor->props, kOfxImageEffectPluginPropGrouping) ==
            "Mixed Media",
        "paper grouping");
  check(getString(descriptor->props, kOfxImageEffectPropCudaRenderSupported) ==
            "true",
        "paper advertises CUDA");
  check(getString(descriptor->props, kOfxImageEffectPropCudaStreamSupported) ==
            "true",
        "paper advertises CUDA streams");
  check(getString(descriptor->props, kOfxImageEffectPropSupportedPixelDepths) ==
            kOfxBitDepthFloat,
        "paper float only");
  check(getString(descriptor->props, kOfxImageEffectPropSupportedContexts) ==
            kOfxImageEffectContextFilter,
        "paper filter context");

  PropertySet* describeArgs = new PropertySet();
  setString(describeArgs, kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  entry(kOfxImageEffectActionDescribeInContext, descriptor,
        asHandle(describeArgs), nullptr);
  delete describeArgs;

  check(descriptor->clips.count(kOfxImageEffectSimpleSourceClipName) == 1,
        "paper source clip");
  check(descriptor->clips.count(kOfxImageEffectOutputClipName) == 1,
        "paper output clip");
  if (descriptor->clips.count(kOfxImageEffectSimpleSourceClipName) == 1) {
    check(getString(descriptor->clips[kOfxImageEffectSimpleSourceClipName]->props,
                    kOfxImageEffectPropSupportedComponents) ==
              kOfxImageComponentRGBA,
          "paper source RGBA");
  }

  const char* paperParams[] = {
      "columns", "rows", "missingPieces", "seed", "rotation",
      "sizeVariation", "widthVariation", "heightVariation", "uvShift",
      "colorVariation", "tearSize", "chunkSize", "roughness", "whiteEdge",
      "unevenness", "shadow", "backgroundImage", "backgroundColor"};
  for (const char* name : paperParams) {
    ParamDef* param = findParam(descriptor, name);
    check(param != nullptr, name);
    if (param) {
      check(getInt(param->props, kOfxParamPropAnimates) == 1,
            "paper parameter animates");
    }
  }
  ParamDef* missing = findParam(descriptor, "missingPieces");
  ParamDef* seed = findParam(descriptor, "seed");
  check(missing && missing->type == kOfxParamTypeDouble, "missing double");
  check(seed && seed->type == kOfxParamTypeInteger, "seed integer");
  check(findParam(descriptor, "backgroundColor")->type == kOfxParamTypeRGB,
        "background RGB");

  auto* instance = makeInstance(descriptor);
  setInstanceProperties(instance);
  entry(kOfxActionCreateInstance, instance, nullptr, nullptr);

  const int width = 128;
  const int height = 128;
  FakeImage source;
  fillSource(source, width, height);
  FakeImage output;
  output.x1 = 0; output.y1 = 0; output.x2 = width; output.y2 = height;
  output.rowBytes = width * 16;
  output.opaque = true;
  instance->clips[kOfxImageEffectSimpleSourceClipName]->image = &source;
  instance->clips[kOfxImageEffectOutputClipName]->image = &output;
  setInt(instance->clips[kOfxImageEffectSimpleSourceClipName]->props,
         kOfxImageClipPropConnected, 1);

  PropertySet* renderArgs = makeRenderArgs(width, height, 0.0);
  std::vector<float> flat;
  setParamInt(instance->params, "columns", 4);
  setParamInt(instance->params, "rows", 4);
  setParamDouble(instance->params, "missingPieces", 100.0);
  setParamInt(instance->params, "seed", 1);
  setParamInt(instance->params, "backgroundImage", 0);
  setParamDouble(instance->params, "shadow", 0.0);
  setParamRgb(instance->params, "backgroundColor", 0.1, 0.2, 0.3);
  renderOnce(entry, instance, source, output, renderArgs, flat);
  for (size_t i = 0; i < flat.size(); i += 4) {
    checkNear(flat[i + 0], 0.1f, "paper flat r");
    checkNear(flat[i + 1], 0.2f, "paper flat g");
    checkNear(flat[i + 2], 0.3f, "paper flat b");
    checkNear(flat[i + 3], 1.0f, "paper flat a");
  }

  setParamDouble(instance->params, "missingPieces", 0.0);
  setParamInt(instance->params, "backgroundImage", 1);
  std::vector<float> cutout1;
  renderOnce(entry, instance, source, output, renderArgs, cutout1);
  check(allFinite(cutout1), "paper finite");
  checkNonIdentity(source.data, cutout1, "paper changes image");
  for (size_t i = 3; i < cutout1.size(); i += 4) {
    checkNear(cutout1[i], 1.0f, "paper alpha");
  }
  runCudaPath(entry, instance, source, output, renderArgs, cutout1,
              "paper host CUDA path");

  setParamInt(instance->params, "seed", 2);
  std::vector<float> cutout2;
  renderOnce(entry, instance, source, output, renderArgs, cutout2);
  bool seedDiff = false;
  for (size_t i = 0; i < cutout1.size(); ++i) {
    if (std::fabs(cutout1[i] - cutout2[i]) > 1.0e-4f) {
      seedDiff = true;
      break;
    }
  }
  check(seedDiff, "seed changes paper layout");

  setParamDouble(instance->params, "missingPieces", 100.0);
  setParamInt(instance->params, "backgroundImage", 1);
  std::vector<float> backgroundOnly;
  renderOnce(entry, instance, source, output, renderArgs, backgroundOnly);
  for (size_t i = 0; i < source.data.size(); ++i) {
    checkNear(backgroundOnly[i], source.data[i], "paper background image", 2.0e-4f);
  }

  // Canonical bounds with a non-zero origin must not shift the frame.
  source.x1 = 23; source.y1 = -14;
  source.x2 = source.x1 + width; source.y2 = source.y1 + height;
  output.x1 = source.x1; output.y1 = source.y1;
  output.x2 = source.x2; output.y2 = source.y2;
  std::vector<float> offsetBounds;
  renderOnce(entry, instance, source, output, renderArgs, offsetBounds);
  for (size_t i = 0; i < source.data.size(); ++i) {
    checkNear(offsetBounds[i], source.data[i], "paper offset bounds", 2.0e-4f);
  }
  source.x1 = 0; source.y1 = 0; source.x2 = width; source.y2 = height;
  output.x1 = 0; output.y1 = 0; output.x2 = width; output.y2 = height;

  // Animated seed + missing pieces: these are the two controls the Mixed Media
  // preset keys to move the collage over time.
  setParamIntAtTime(instance->params, "seed", 0.0, 1);
  setParamIntAtTime(instance->params, "seed", 1.0, 2);
  setParamDoubleAtTime(instance->params, "missingPieces", 0.0, 0.0);
  setParamDoubleAtTime(instance->params, "missingPieces", 1.0, 100.0);
  PropertySet* keyArgs0 = makeRenderArgs(width, height, 0.0);
  PropertySet* keyArgs1 = makeRenderArgs(width, height, 1.0);
  std::vector<float> key0;
  std::vector<float> key1;
  renderOnce(entry, instance, source, output, keyArgs0, key0);
  renderOnce(entry, instance, source, output, keyArgs1, key1);
  for (size_t i = 0; i < key0.size(); ++i) {
    checkNear(key0[i], cutout1[i], "keyframed seed at t=0", 2.0e-4f);
    checkNear(key1[i], source.data[i], "keyframed missing at t=1", 2.0e-4f);
  }
  delete keyArgs0;
  delete keyArgs1;

  entry(kOfxActionDestroyInstance, instance, nullptr, nullptr);
  delete renderArgs;
  delete instance;
  delete descriptor;
  entry(kOfxActionUnload, nullptr, nullptr, nullptr);
}

void testHalftone(OfxPlugin* plugin, OfxPluginEntryPoint* entry) {
  plugin->setHost(&gOfxHost);
  check(entry(kOfxActionLoad, nullptr, nullptr, nullptr) == kOfxStatOK,
        "halftone load");

  auto* descriptor = new Effect();
  descriptor->props = new PropertySet();
  descriptor->params = new ParamSet();
  entry(kOfxActionDescribe, descriptor, nullptr, nullptr);
  check(getString(descriptor->props, kOfxPropLabel) == "Halftone Screen",
        "halftone label");
  check(getString(descriptor->props, kOfxImageEffectPluginPropGrouping) ==
            "Mixed Media",
        "halftone grouping");

  PropertySet* describeArgs = new PropertySet();
  setString(describeArgs, kOfxImageEffectPropContext, kOfxImageEffectContextFilter);
  entry(kOfxImageEffectActionDescribeInContext, descriptor,
        asHandle(describeArgs), nullptr);
  delete describeArgs;

  const char* htParams[] = {
      "pattern", "frequency", "roughness", "fuzziness", "paperFiber",
      "inkTexture", "inkDensity", "channelC", "channelM", "channelY",
      "channelK", "angleC", "angleM", "angleY", "angleK", "offsetXC",
      "offsetYC", "offsetXM", "offsetYM", "offsetXY", "offsetYY",
      "offsetXK", "offsetYK"};
  for (const char* name : htParams) {
    ParamDef* param = findParam(descriptor, name);
    check(param != nullptr, name);
    if (param) {
      check(getInt(param->props, kOfxParamPropAnimates) == 1,
            "halftone parameter animates");
    }
  }
  ParamDef* pattern = findParam(descriptor, "pattern");
  check(pattern && pattern->type == kOfxParamTypeChoice, "pattern choice");
  if (pattern) {
    check(getString(pattern->props, kOfxParamPropChoiceOption, 0) == "Round",
          "pattern Round");
    check(getString(pattern->props, kOfxParamPropChoiceOption, 5) == "Cross",
          "pattern Cross");
  }

  auto* instance = makeInstance(descriptor);
  setInstanceProperties(instance);
  entry(kOfxActionCreateInstance, instance, nullptr, nullptr);

  const int width = 128;
  const int height = 128;
  FakeImage source;
  fillSource(source, width, height);
  FakeImage output;
  output.x1 = 0; output.y1 = 0; output.x2 = width; output.y2 = height;
  output.rowBytes = width * 16;
  output.opaque = true;
  instance->clips[kOfxImageEffectSimpleSourceClipName]->image = &source;
  instance->clips[kOfxImageEffectOutputClipName]->image = &output;
  setInt(instance->clips[kOfxImageEffectSimpleSourceClipName]->props,
         kOfxImageClipPropConnected, 1);

  PropertySet* renderArgs = makeRenderArgs(width, height, 0.0);
  setParamDouble(instance->params, "frequency", 35.0);
  setParamInt(instance->params, "channelC", 0);
  setParamInt(instance->params, "channelM", 0);
  setParamInt(instance->params, "channelY", 0);
  setParamInt(instance->params, "channelK", 0);
  std::vector<float> paperOnly;
  renderOnce(entry, instance, source, output, renderArgs, paperOnly);
  check(allFinite(paperOnly), "halftone paper finite");
  checkNonIdentity(source.data, paperOnly, "halftone paper differs");

  setParamInt(instance->params, "channelC", 1);
  setParamInt(instance->params, "channelM", 1);
  setParamInt(instance->params, "channelY", 1);
  setParamInt(instance->params, "channelK", 1);
  std::vector<float> allInks;
  renderOnce(entry, instance, source, output, renderArgs, allInks);
  check(allFinite(allInks), "halftone finite");
  checkNonIdentity(source.data, allInks, "halftone changes image");
  bool differsFromPaper = false;
  for (size_t i = 0; i < allInks.size(); ++i) {
    if (std::fabs(allInks[i] - paperOnly[i]) > 1.0e-4f) {
      differsFromPaper = true;
      break;
    }
  }
  check(differsFromPaper, "CMYK channels affect output");
  for (size_t i = 3; i < allInks.size(); i += 4) {
    checkNear(allInks[i], 1.0f, "halftone alpha");
  }
  runCudaPath(entry, instance, source, output, renderArgs, allInks,
              "halftone host CUDA path");

  setParamDouble(instance->params, "angleC", 30.0);
  setParamDouble(instance->params, "offsetXC", 6.0);
  setParamDouble(instance->params, "offsetYC", -3.0);
  std::vector<float> movedPlate;
  renderOnce(entry, instance, source, output, renderArgs, movedPlate);
  bool plateDiff = false;
  for (size_t i = 0; i < allInks.size(); ++i) {
    if (std::fabs(allInks[i] - movedPlate[i]) > 1.0e-4f) {
      plateDiff = true;
      break;
    }
  }
  check(plateDiff, "cyan angle and offsets affect output");

  entry(kOfxActionDestroyInstance, instance, nullptr, nullptr);
  delete renderArgs;
  delete instance;
  delete descriptor;
  entry(kOfxActionUnload, nullptr, nullptr, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  initSuites();
  const std::string pluginPath =
      argc > 1 ? argv[1] : "dist/MixedMedia.ofx.bundle/Contents/Win64/MixedMedia.ofx";

#ifdef _WIN32
  HMODULE module = LoadLibraryA(pluginPath.c_str());
  if (!module) {
    std::fprintf(stderr, "FAIL: LoadLibrary(%s) error %lu\n", pluginPath.c_str(),
                 static_cast<unsigned long>(GetLastError()));
    return 1;
  }
  using GetCountFn = int (*)();
  using GetPluginFn = OfxPlugin* (*)(int);
  auto getCount = reinterpret_cast<GetCountFn>(
      GetProcAddress(module, "OfxGetNumberOfPlugins"));
  auto getPlugin = reinterpret_cast<GetPluginFn>(
      GetProcAddress(module, "OfxGetPlugin"));
#else
  void* module = dlopen(pluginPath.c_str(), RTLD_NOW);
  if (!module) {
    std::fprintf(stderr, "FAIL: dlopen: %s\n", dlerror());
    return 1;
  }
  using GetCountFn = int (*)();
  using GetPluginFn = OfxPlugin* (*)(int);
  auto getCount = reinterpret_cast<GetCountFn>(dlsym(module, "OfxGetNumberOfPlugins"));
  auto getPlugin = reinterpret_cast<GetPluginFn>(dlsym(module, "OfxGetPlugin"));
#endif
  check(getCount != nullptr, "OfxGetNumberOfPlugins export");
  check(getPlugin != nullptr, "OfxGetPlugin export");
  if (!getCount || !getPlugin) {
    return 1;
  }
  check(getCount() == 2, "plugin count is two");

  OfxPlugin* paper = getPlugin(0);
  OfxPlugin* halftone = getPlugin(1);
  check(paper != nullptr, "paper plugin pointer");
  check(halftone != nullptr, "halftone plugin pointer");
  if (!paper || !halftone) {
    return 1;
  }
  check(std::strcmp(paper->pluginIdentifier,
                    "com.mixedmedia.openfx.papercutout") == 0,
        "paper identifier");
  check(std::strcmp(halftone->pluginIdentifier,
                    "com.mixedmedia.openfx.halftonescreen") == 0,
        "halftone identifier");

  gOfxHost.host = reinterpret_cast<OfxPropertySetHandle>(gHostProperties);
  gOfxHost.fetchSuite = &fetchSuite;

#ifdef _WIN32
  gTestCuda.load();
#endif

  testPaper(paper, paper->mainEntry);
  testHalftone(halftone, halftone->mainEntry);

#ifdef _WIN32
  FreeLibrary(module);
#else
  dlclose(module);
#endif

  delete gHostProperties;
  gHostProperties = nullptr;

  if (gFailures != 0) {
    std::fprintf(stderr, "%d host test(s) failed\n", gFailures);
    return 1;
  }
  std::printf("All OFX host tests passed\n");
  return 0;
}
