# OpenPPP2 HarmonyOS NEXT

This directory contains the native HarmonyOS NEXT application surface for
OpenPPP2. It is intentionally separate from the existing Android Flutter and
iOS UIKit clients.

## Scope

- ArkTS / ArkUI control plane.
- `VpnExtensionAbility` tunnel owner.
- N-API bridge to the shared C++ OpenPPP2 runtime.
- V1 arm64-only native build.

The VPN API is device, SDK, and signing sensitive. If the current development
profile cannot create a VPN extension or TUN file descriptor, the app still
builds as a technical preview with the UI, profile storage, subscription
parser, native configuration parsing, and diagnostics paths in place.

## Layout

```text
ohos/
  AppScope/app.json5
  entry/src/main/ets/
    entryability/EntryAbility.ets
    extensionability/OpenPpp2VpnExtensionAbility.ets
    pages/Index.ets
    services/
    models/
  entry/src/main/cpp/
    CMakeLists.txt
    openppp2_ohos.cpp
    types/libopenppp2/index.d.ts
```

## Build notes

The native CMake file expects OHOS-built third-party libraries under:

```text
$PPP_OHOS_THIRD_PARTY/
  boost/arm64-v8a/
  openssl/arm64-v8a/include
  openssl/arm64-v8a/lib
```

If `PPP_OHOS_THIRD_PARTY` is not set, it falls back to `/root/ohos`.
DevEco/Hvigor should be used for the application build.
