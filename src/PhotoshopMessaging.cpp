// Requires the Adobe SDK and its automation sample's PiPL/resource packaging.
// This translation unit implements messaging lifecycle, not pixel extraction.
#include "SPBasic.h"
#include "SPInterf.h"
#include "SPMessag.h"
#include "PIAction.h"
#include "PIUXPSuite.h"
#include <cstring>
#include <string>

namespace {
SPBasicSuite* basic = nullptr;
SPPluginRef self = nullptr;
const PsUXPSuite1* sUxpProcs = nullptr;
const PSActionDescriptorProcs* descriptors = nullptr;
bool listening = false;
constexpr DescriptorKeyID requestKey = 0x67726571; // greq
constexpr DescriptorKeyID commandKey = 0x67636d64; // gcmd
constexpr DescriptorKeyID configKey = 0x67636667;  // gcfg
constexpr DescriptorKeyID errorKey = 0x67657272;   // gerr
SPErr ReadString(PIActionDescriptor descriptor, DescriptorKeyID key, std::string& output) {
    uint32 length = 0;
    SPErr error = descriptors->GetStringLength(descriptor, key, &length);
    if (error) return error;
    if (length > 16384) return -1;
    output.resize(static_cast<size_t>(length) + 1);
    error = descriptors->GetString(descriptor, key, output.data(), length + 1);
    if (!error) output.resize(length);
    return error;
}
void UXPMessageHandler(PIActionDescriptor message) noexcept {
    PIActionDescriptor reply = nullptr;
    try {
        std::string id, command, config;
        if (ReadString(message, requestKey, id) || id.empty() || id.size() > 64) return;
        const bool valid = !ReadString(message, commandKey, command) && command == "configure" &&
            !ReadString(message, configKey, config) && !config.empty();
        if (descriptors->Make(&reply)) return;
        SPErr error = descriptors->PutString(reply, requestKey, id.c_str());
        if (!error && !valid) error = descriptors->PutString(reply, errorKey, "Invalid Gaslightr configuration envelope.");
        // Receipt only; no DOM or GPU work runs inside this notifier.
        if (!error) sUxpProcs->SendUXPMessage(self, "com.advaet.gaslightr", reply);
    } catch (...) { /* Never unwind into Photoshop's notifier dispatcher. */ }
    if (reply) descriptors->Free(reply);
}
SPErr Shutdown() {
    if (!basic) return kSPNoError;
    if (listening) {
        const SPErr error = sUxpProcs->RemoveUXPMessageListener(self);
        if (error) return error;
        listening = false;
    }
    if (descriptors) basic->ReleaseSuite(kPSActionDescriptorSuite, kPSActionDescriptorSuiteVersion);
    if (sUxpProcs) basic->ReleaseSuite(kPSUXPSuite, kPSUXPSuiteVersion1);
    descriptors = nullptr; sUxpProcs = nullptr; basic = nullptr; self = nullptr;
    return kSPNoError;
}
SPErr Startup(SPMessageData* message) {
    if (listening) return kSPNoError;
    if (!message || !message->basic || !message->self) return -1;
    basic = message->basic; self = message->self;
    SPErr error = basic->AcquireSuite(kPSUXPSuite, kPSUXPSuiteVersion1, reinterpret_cast<const void**>(&sUxpProcs));
    if (!error) error = basic->AcquireSuite(kPSActionDescriptorSuite, kPSActionDescriptorSuiteVersion, reinterpret_cast<const void**>(&descriptors));
    if (!error) error = sUxpProcs->AddUXPMessageListener(self, UXPMessageHandler);
    if (error) { Shutdown(); return error; }
    listening = true;
    return kSPNoError;
}
}
extern "C" __declspec(dllexport) SPErr SPAPI AutoPluginMain(const char* caller, const char* selector, void* message) {
    try {
        if (!caller || !selector) return -1;
        if (std::strcmp(caller, kSPInterfaceCaller) != 0) return kSPNoError;
        if (std::strcmp(selector, kSPInterfaceStartupSelector) == 0) return Startup(static_cast<SPMessageData*>(message));
        if (std::strcmp(selector, kSPInterfaceShutdownSelector) == 0) return Shutdown();
        return kSPNoError;
    } catch (...) { return -1; }
}
