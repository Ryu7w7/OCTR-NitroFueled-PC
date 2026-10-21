# DLSS bridge (`sacredfire_dlss.dll`)

The port itself contains no NVIDIA code: the GPU renderer loads this DLL at run time when it is
next to `sacredfire.exe` together with NVIDIA's `nvngx_dlss.dll`, and renders as usual when they
are missing or the GPU is not an RTX card. The bridge is the only part that uses the NVIDIA DLSS
(NGX) SDK.

It comes from [IFreemz/shadPS4-Bloodborne-DLSS-FSR](https://github.com/IFreemz/shadPS4-Bloodborne-DLSS-FSR)
(`dlss_bridge/`, MIT, see `LICENSE.txt`), with this project's own NGX project id and engine name.
The C interface (`shadps4_dlss_bridge.h`) is unchanged.

## Building (Windows, Visual Studio 2022)

1. Get the NVIDIA DLSS SDK from [github.com/NVIDIA/DLSS](https://github.com/NVIDIA/DLSS)
   (`include/`, `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib`,
   `lib/Windows_x86_64/rel/nvngx_dlss.dll`). It is licensed by NVIDIA under its own terms
   (`LICENSE.txt` in the SDK) and is not part of this repository.
2. `build.bat <DLSS SDK folder> <folder with vulkan/vulkan.h>` builds `sacredfire_dlss.dll`
   with MSVC (static runtime: no Visual C++ redistributable needed).
3. Put `sacredfire_dlss.dll` and the SDK's `nvngx_dlss.dll` next to `sacredfire.exe` (`bin\`).
