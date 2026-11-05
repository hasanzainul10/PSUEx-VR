# Third-party notices

PSUEx is licensed under the GNU General Public License v3 (see `LICENSE`). Source code:
https://github.com/hasanzainul10/PSUEx-VR

PSUEx and its tools include or ship the following third-party components.

## OpenXR loader (`openxr_loader.dll`, shipped as-is)

Copyright 2017-2024 The Khronos Group Inc.
Licensed under the Apache License, Version 2.0: https://www.apache.org/licenses/LICENSE-2.0
Source: https://github.com/KhronosGroup/OpenXR-SDK

## OpenVR SDK header (`third_party/openvr/openvr.h`, v2.5.1, compiled into `PSUEx.dll`)

Copyright (c) 2015, Valve Corporation. BSD 3-Clause License (`third_party/openvr/LICENSE`).
Source: https://github.com/ValveSoftware/openvr. Only the header; `openvr_api.dll` is not shipped (PSUEx uses
SteamVR's own `vrclient.dll`).

## Microsoft Detours (compiled into `PSUEx.dll`)

Copyright (c) Microsoft Corporation. MIT License.
Source: https://github.com/microsoft/Detours

## Dear ImGui (compiled into `PSUEx.dll` and `PSUExVRConfig.exe`)

Copyright (c) 2014-2024 Omar Cornut. MIT License.
Source: https://github.com/ocornut/imgui

## JSON for Modern C++ (compiled into `PSUEx.dll` and `PSUExVRConfig.exe`)

Copyright (c) 2013-2023 Niels Lohmann. MIT License.
Source: https://github.com/nlohmann/json

## Moonshine Voice (compiled into `PSUExSpeech\PSUExSpeech.exe`; English models shipped as-is)

Copyright (c) Moonshine AI (Useful Sensors). MIT License (code and English streaming models).
Source: https://github.com/moonshine-ai/moonshine

## ONNX Runtime (`PSUExSpeech\onnxruntime.dll`, shipped as-is)

Copyright (c) Microsoft Corporation. MIT License.
Source: https://github.com/microsoft/onnxruntime

## MIT License (applies to Detours, Dear ImGui, JSON for Modern C++, Moonshine and ONNX Runtime)

Permission is hereby granted, free of charge, to any person obtaining a copy of this software
and associated documentation files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or
substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
