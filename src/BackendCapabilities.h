#pragma once

namespace Gaslightr {
// This is an implementation capability, never inferred from DLL presence.
inline constexpr bool kNeuralInferenceAvailable = false;
inline constexpr char kNeuralInferenceReason[] =
    "DLSS-NR ABI is unverified; matching runtime headers and integration contract are unavailable.";
inline constexpr char kExecutionMode[] = "d3d12-validation-copy";
}
