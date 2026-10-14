#pragma once

namespace Gaslightr {
// This is an implementation capability, never inferred from DLL presence.
inline constexpr bool kNeuralInferenceAvailable = false;
inline constexpr char kNeuralInferenceReason[] =
    "NVIDIA NGX DLSS-D Ray Reconstruction verified in standalone probe; runtime neural execution disabled inside Photoshop host runtime to ensure application stability.";
inline constexpr char kExecutionMode[] = "d3d12-validation-copy";
}
