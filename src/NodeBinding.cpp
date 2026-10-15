// Node-API diagnostic binding. A .node module is NOT a UXP .uxpaddon.
// UXP must use Adobe's addon API adapter once its SDK is installed.
#include <node_api.h>
#include "TileProcessor.h"
#include "BackendCapabilities.h"
#include <cstring>
#include <string>

namespace {
void Check(napi_status status) {
    if (status != napi_ok) throw std::runtime_error("Node-API operation failed.");
}
napi_value Property(napi_env env, napi_value object, const char* name) {
    napi_value result;
    Check(napi_get_named_property(env, object, name, &result));
    return result;
}
uint32_t Integer(napi_env env, napi_value object, const char* name, uint32_t maximum) {
    double value;
    Check(napi_get_value_double(env, Property(env, object, name), &value));
    if (!std::isfinite(value) || value < 0 || value > maximum || std::floor(value) != value)
        throw std::invalid_argument(std::string("Invalid ") + name);
    return static_cast<uint32_t>(value);
}
napi_value Process(napi_env env, napi_callback_info info) {
    try {
        size_t argc = 2;
        napi_value args[2];
        Check(napi_get_cb_info(env, info, &argc, args, nullptr, nullptr));
        if (argc != 2) throw std::invalid_argument("processTile requires a Float32Array and options.");
        napi_typedarray_type type;
        napi_value backing;
        size_t byteOffset, length;
        void* data = nullptr;
        Check(napi_get_typedarray_info(env, args[0], &type, &length, &data, &backing, &byteOffset));
        // data includes byteOffset. Never add it a second time.
        if (type != napi_float32_array || data == nullptr) throw std::invalid_argument("Expected Float32Array pixels.");
        bool ordinaryBuffer = false;
        Check(napi_is_arraybuffer(env, backing, &ordinaryBuffer));
        if (!ordinaryBuffer) throw std::invalid_argument("SharedArrayBuffer pixels are not supported.");
        Gaslightr::TileRequest request;
        request.width = Integer(env, args[1], "width", 2048);
        request.height = Integer(env, args[1], "height", 2048);
        request.preset = static_cast<int>(Integer(env, args[1], "preset", 2));
        Check(napi_get_value_bool(env, Property(env, args[1], "detailOnly"), &request.detailOnly));
        napi_value valOnlyVal = Property(env, args[1], "validationOnly");
        if (valOnlyVal) {
            bool b = false;
            if (napi_get_value_bool(env, valOnlyVal, &b) == napi_ok) {
                request.validationOnly = b;
                if (b) request.executionMode = Gaslightr::ExecutionMode::Validation;
            }
        }
        napi_value modeVal = Property(env, args[1], "mode");
        if (modeVal) {
            char modeStr[32]{};
            size_t modeLen = 0;
            if (napi_get_value_string_utf8(env, modeVal, modeStr, sizeof(modeStr), &modeLen) == napi_ok) {
                if (std::strcmp(modeStr, "neural") == 0) {
                    request.executionMode = Gaslightr::ExecutionMode::Neural;
                    request.validationOnly = false;
                } else if (std::strcmp(modeStr, "validation") == 0) {
                    request.executionMode = Gaslightr::ExecutionMode::Validation;
                    request.validationOnly = true;
                } else if (std::strcmp(modeStr, "enhancement") == 0) {
                    request.executionMode = Gaslightr::ExecutionMode::Enhancement;
                    request.validationOnly = false;
                }
            }
        }
        napi_value tileXVal = Property(env, args[1], "tileX");
        if (tileXVal) {
            double v = 0.0;
            if (napi_get_value_double(env, tileXVal, &v) == napi_ok && std::isfinite(v)) {
                request.tileX = static_cast<int32_t>(v);
            }
        }
        napi_value tileYVal = Property(env, args[1], "tileY");
        if (tileYVal) {
            double v = 0.0;
            if (napi_get_value_double(env, tileYVal, &v) == napi_ok && std::isfinite(v)) {
                request.tileY = static_cast<int32_t>(v);
            }
        }
        napi_value canvasWVal = Property(env, args[1], "canvasWidth");
        if (canvasWVal) {
            double v = 0.0;
            if (napi_get_value_double(env, canvasWVal, &v) == napi_ok && std::isfinite(v) && v >= 0) {
                request.canvasWidth = static_cast<uint32_t>(v);
            }
        }
        napi_value canvasHVal = Property(env, args[1], "canvasHeight");
        if (canvasHVal) {
            double v = 0.0;
            if (napi_get_value_double(env, canvasHVal, &v) == napi_ok && std::isfinite(v) && v >= 0) {
                request.canvasHeight = static_cast<uint32_t>(v);
            }
        }
        // Synchronous borrow: the local handle roots the allocation and JS on
        // this isolate cannot mutate/detach it during GPU processing. No JS
        // pointer escapes this callback; shared backing memory is rejected.
        const auto output = Gaslightr::ProcessTile({static_cast<const float*>(data), length}, request);
        napi_value buffer, typed;
        void* bytes = nullptr;
        Check(napi_create_arraybuffer(env, output.size() * sizeof(float), &bytes, &buffer));
        std::memcpy(bytes, output.data(), output.size() * sizeof(float));
        Check(napi_create_typedarray(env, napi_float32_array, output.size(), buffer, 0, &typed));
        return typed;
    } catch (const std::exception& error) {
        napi_throw_error(env, nullptr, error.what());
        return nullptr;
    } catch (...) {
        napi_throw_error(env, nullptr, "Unknown native processing failure.");
        return nullptr;
    }
}
napi_value Capabilities(napi_env env, napi_callback_info) {
    try {
        napi_value result, value;
        Check(napi_create_object(env, &result));
        Check(napi_create_int32(env, 1, &value));
        Check(napi_set_named_property(env, result, "protocol", value));
        Check(napi_get_boolean(env, false, &value));
        Check(napi_set_named_property(env, result, "detailOnly", value));
        Check(napi_set_named_property(env, result, "sdkMessaging", value));
        Check(napi_get_boolean(env, Gaslightr::kNeuralInferenceAvailable, &value));
        Check(napi_set_named_property(env, result, "neuralInference", value));
        Check(napi_create_string_utf8(env, Gaslightr::kNeuralInferenceReason, NAPI_AUTO_LENGTH, &value));
        Check(napi_set_named_property(env, result, "neuralInferenceReason", value));
        Check(napi_create_string_utf8(env, Gaslightr::kExecutionMode, NAPI_AUTO_LENGTH, &value));
        Check(napi_set_named_property(env, result, "executionMode", value));
        Check(napi_create_string_utf8(env, "CUDA texture interop requires repair and device validation.", NAPI_AUTO_LENGTH, &value));
        Check(napi_set_named_property(env, result, "detailOnlyReason", value));
        return result;
    } catch (const std::exception& error) { napi_throw_error(env, nullptr, error.what()); return nullptr; }
}
napi_value Init(napi_env env, napi_value exports) {
    const napi_property_descriptor properties[] = {
        { "processTile", nullptr, Process, nullptr, nullptr, nullptr, napi_default, nullptr },
        { "capabilities", nullptr, Capabilities, nullptr, nullptr, nullptr, napi_default, nullptr }
    };
    if (napi_define_properties(env, exports, 2, properties) != napi_ok) return nullptr;
    return exports;
}
}
NAPI_MODULE(gaslightr, Init)
