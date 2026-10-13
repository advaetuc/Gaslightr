#include "UxpAddon.h"
#include "TileProcessor.h"
#include "BackendCapabilities.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>

namespace Gaslightr::UXP {
namespace {
void Require(addon_status status, const char* operation) {
    if (status != addon_ok) throw std::runtime_error(operation);
}
addon_value Property(addon_env env, addon_value object, const char* name) {
    addon_value result = nullptr;
    Require(UxpAddonApis.uxp_addon_get_named_property(env, object, name, &result), name);
    return result;
}
addon_value String(addon_env env, const char* text) {
    addon_value result = nullptr;
    Require(UxpAddonApis.uxp_addon_create_string_utf8(env, text, std::strlen(text), &result), "Create string");
    return result;
}
size_t UnsignedInteger(addon_env env, addon_value value, size_t maximum, const char* name) {
    double number = 0;
    Require(UxpAddonApis.uxp_addon_get_value_double(env, value, &number), name);
    if (!std::isfinite(number) || number < 0 || number > static_cast<double>(maximum) || std::floor(number) != number)
        throw std::invalid_argument(std::string("Invalid ") + name);
    return static_cast<size_t>(number);
}
addon_value Fail(addon_env env, const char* message) noexcept {
    OutputDebugStringA("[Gaslightr] UXP adapter error: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
    bool pending = false;
    // Preserve an existing JavaScript exception (including a failing getter).
    if (UxpAddonApis.uxp_addon_is_exception_pending(env, &pending) != addon_ok || !pending)
        UxpAddonApis.uxp_addon_throw_error(env, "GASLIGHTR_NATIVE", message);
    return nullptr;
}

// SDK 0.1.0 has no get_typedarray_info. Invoke the actual %TypedArray%
// accessors instead of reading shadowable input.buffer/length/byteOffset.
// All handles stay local, and all JavaScript calls finish BEFORE memory borrow.
struct TypedArrayAccessors {
    addon_env env;
    addon_value object = nullptr;
    addon_value descriptorFunction = nullptr;
    addon_value prototype = nullptr;
    addon_value global = nullptr;
    explicit TypedArrayAccessors(addon_env environment) : env(environment) {
        Require(UxpAddonApis.uxp_addon_get_global(env, &global), "Get global");
        object = Property(env, global, "Object");
        descriptorFunction = Property(env, object, "getOwnPropertyDescriptor");
        addon_value buffer = nullptr, sample = nullptr, floatPrototype = nullptr;
        void* unused = nullptr;
        Require(UxpAddonApis.uxp_addon_create_arraybuffer(env, sizeof(float), &unused, &buffer), "Create type probe buffer");
        Require(UxpAddonApis.uxp_addon_create_typedarray(env, addon_float32_array, 1, buffer, 0, &sample), "Create type probe");
        Require(UxpAddonApis.uxp_addon_get_prototype(env, sample, &floatPrototype), "Get Float32Array prototype");
        Require(UxpAddonApis.uxp_addon_get_prototype(env, floatPrototype, &prototype), "Get TypedArray prototype");
    }
    addon_value Get(addon_value input, addon_value key) const {
        addon_value descriptor = nullptr, result = nullptr;
        const addon_value args[] = { prototype, key };
        Require(UxpAddonApis.uxp_addon_call_function(env, object, descriptorFunction, 2, args, &descriptor), "Get intrinsic descriptor");
        const addon_value getter = Property(env, descriptor, "get");
        Require(UxpAddonApis.uxp_addon_call_function(env, input, getter, 0, nullptr, &result), "Read intrinsic TypedArray metadata");
        return result;
    }
    addon_value Get(addon_value input, const char* name) const { return Get(input, String(env, name)); }
    void RequireFloat32(addon_value input) const {
        bool typed = false;
        Require(UxpAddonApis.uxp_addon_is_typedarray(env, input, &typed), "Check TypedArray");
        if (!typed) throw std::invalid_argument("Expected a Float32Array.");
        const addon_value symbol = Property(env, global, "Symbol");
        const addon_value tag = Get(input, Property(env, symbol, "toStringTag"));
        char name[32]{};
        size_t length = 0;
        Require(UxpAddonApis.uxp_addon_get_value_string_utf8(env, tag, name, sizeof(name), &length), "Read TypedArray brand");
        if (length != 12 || std::strcmp(name, "Float32Array") != 0)
            throw std::invalid_argument("Expected a Float32Array, not another typed-array type.");
    }
};
}

addon_value ProcessTile(addon_env env, addon_callback_info info) noexcept {
    try {
        size_t argc = 3;
        addon_value args[3]{};
        Require(UxpAddonApis.uxp_addon_get_cb_info(env, info, &argc, args, nullptr, nullptr), "Read arguments");
        if (argc != 2) throw std::invalid_argument("processTile requires pixels and options.");
        TileRequest request;
        // Unbox options before retrieving the pixel pointer. Option getters can
        // run JavaScript and must never run while a borrowed pointer is in use.
        request.width = static_cast<uint32_t>(UnsignedInteger(env, Property(env, args[1], "width"), 2048, "width"));
        request.height = static_cast<uint32_t>(UnsignedInteger(env, Property(env, args[1], "height"), 2048, "height"));
        request.preset = static_cast<int>(UnsignedInteger(env, Property(env, args[1], "preset"), 2, "preset"));
        Require(UxpAddonApis.uxp_addon_get_value_bool(env, Property(env, args[1], "detailOnly"), &request.detailOnly), "Read detailOnly");
        if (!request.width || !request.height) throw std::invalid_argument("Tile dimensions must be 1-2048.");
        const size_t expected = static_cast<size_t>(request.width) * request.height * 4;

        TypedArrayAccessors accessors(env);
        accessors.RequireFloat32(args[0]);
        const addon_value backing = accessors.Get(args[0], "buffer");
        const size_t offset = UnsignedInteger(env, accessors.Get(args[0], "byteOffset"), 9007199254740991ULL, "byteOffset");
        const size_t count = UnsignedInteger(env, accessors.Get(args[0], "length"), expected, "length");
        if (count != expected) throw std::invalid_argument("Expected exactly width * height * 4 components.");
        bool ordinary = false;
        Require(UxpAddonApis.uxp_addon_is_arraybuffer(env, backing, &ordinary), "Check ArrayBuffer");
        if (!ordinary) throw std::invalid_argument("SharedArrayBuffer input is unsupported.");
        void* base = nullptr;
        size_t bytes = 0;
        Require(UxpAddonApis.uxp_addon_get_arraybuffer_info(env, backing, &base, &bytes), "Borrow ArrayBuffer");
        if (!base || offset % alignof(float) != 0 || offset > bytes || count > (bytes - offset) / sizeof(float))
            throw std::invalid_argument("Detached, unaligned or out-of-bounds input view.");
        const auto* input = reinterpret_cast<const float*>(static_cast<const unsigned char*>(base) + offset);
        // No JS calls or callbacks between here and the synchronous core return.
        // The local handles root the view. The core validates finite half-float
        // range/alpha, serializes NGX access and checks WaitForGPU before return.
        ValidateTile({input, count}, request);
        OutputDebugStringA("[Gaslightr] processTile begin\n");
        const auto result = Gaslightr::ProcessTile({input, count}, request);
        if (result.size() != expected) throw std::runtime_error("Unexpected native output length.");
        OutputDebugStringA("[Gaslightr] processTile GPU work drained\n");
        addon_value buffer = nullptr, output = nullptr;
        void* destination = nullptr;
        Require(UxpAddonApis.uxp_addon_create_arraybuffer(env, result.size() * sizeof(float), &destination, &buffer), "Allocate output buffer");
        if (!destination) throw std::runtime_error("Cannot allocate output memory.");
        std::memcpy(destination, result.data(), result.size() * sizeof(float));
        Require(UxpAddonApis.uxp_addon_create_typedarray(env, addon_float32_array, result.size(), buffer, 0, &output), "Wrap output Float32Array");
        return output;
    } catch (const std::exception& error) { return Fail(env, error.what()); }
    catch (...) { return Fail(env, "Unknown native tile-processing failure."); }
}

addon_value Capabilities(addon_env env, addon_callback_info) noexcept {
    try {
        addon_value result = nullptr, value = nullptr;
        Require(UxpAddonApis.uxp_addon_create_object(env, &result), "Create capabilities");
        Require(UxpAddonApis.uxp_addon_create_int32(env, 1, &value), "Create protocol");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "protocol", value), "Set protocol");
        Require(UxpAddonApis.uxp_addon_get_boolean(env, false, &value), "Create capability flag");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "detailOnly", value), "Set detailOnly");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "sdkMessaging", value), "Set sdkMessaging");
        Require(UxpAddonApis.uxp_addon_get_boolean(env, kNeuralInferenceAvailable, &value), "Create inference flag");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "neuralInference", value), "Set neuralInference");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "neuralInferenceReason",
            String(env, kNeuralInferenceReason)), "Set neuralInferenceReason");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "executionMode",
            String(env, kExecutionMode)), "Set executionMode");
        Require(UxpAddonApis.uxp_addon_set_named_property(env, result, "detailOnlyReason",
            String(env, "CUDA texture interop requires repair and device validation.")), "Set detailOnlyReason");
        return result;
    } catch (const std::exception& error) { return Fail(env, error.what()); }
    catch (...) { return Fail(env, "Cannot query capabilities."); }
}

addon_value Init(addon_env env, addon_value exports, const addon_apis&) noexcept {
    try {
        const struct { const char* name; addon_callback callback; } functions[] = {
            {"capabilities", Capabilities}, {"processTile", ProcessTile}
        };
        for (const auto& function : functions) {
            addon_value wrapped = nullptr;
            Require(UxpAddonApis.uxp_addon_create_function(env, function.name, std::strlen(function.name), function.callback, nullptr, &wrapped), "Register function");
            Require(UxpAddonApis.uxp_addon_set_named_property(env, exports, function.name, wrapped), "Export function");
        }
        OutputDebugStringA("[Gaslightr] UXP addon initialized\n");
        return exports;
    } catch (const std::exception& error) { return Fail(env, error.what()); }
    catch (...) { return Fail(env, "Cannot initialize Gaslightr addon."); }
}
void Terminate(addon_env) noexcept {
    // No persistent JS handles, worker tasks or GPU queues belong to this adapter.
    // Each synchronous core call has already drained and destroyed its resources.
    OutputDebugStringA("[Gaslightr] UXP addon terminated\n");
}
}
UXP_ADDON_INIT(Gaslightr::UXP::Init)
UXP_ADDON_TERMINATE(Gaslightr::UXP::Terminate)
