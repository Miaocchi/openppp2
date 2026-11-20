# OpenPPP2 Android client

> [中文](README_CN.md) · [Technical guide](debug.md) · [Rule assets](android/app/src/main/assets/rules/README.md)

**Status:** Experimental

**Type:** Platform-specific Flutter, Android VPN, and JNI client surface

**Last verified:** 2026-07-22

This directory is the Android client that is shipped with this OpenPPP2 tree. It is a Flutter application backed by an Android `VpnService` and the bundled native `libopenppp2.so`; it is not a standalone SDK or a replacement for the native command-line runtime.
>
> **Audience:** Flutter/Android developers, JNI/native maintainers, and evaluators testing the client on a disposable device.
> **Supported status:** Experimental platform surface; the checked-in native artifact is arm64-v8a only, and no universal device or release support is claimed.
> **Build/runtime boundary:** This directory builds the Flutter app, Android `VpnService`, and JNI bridge around `libopenppp2.so`; a successful build does not establish a working VPN session or replace the native command-line runtime.
> **Canonical next page:** [Technical guide](debug.md) for the current lifecycle, JNI boundary, and troubleshooting signals.

## What is here

- Flutter UI and profile/settings code in `lib/`.
- Android activity, VPN service, state mirror, and JNI declarations in `android/app/src/main/`.
- A packaged native library at `android/app/src/main/jniLibs/arm64-v8a/libopenppp2.so`.
- Native Android CMake sources and the ABI-oriented `build.sh` helper at this directory level.

The UI currently has Home, launch-options, profiles, and settings areas. The profile manager and selector support search and recovery from empty results. The editor puts common fields first, then encryption, local proxy and explicit Raw JSON; leaving with unsaved changes is guarded. Launch options, per-app proxy, settings and telemetry distinguish saved values, next-connection applicability and privacy/diagnostic scope. Treat the client as an experimental platform surface: validate a real connection and the device's VPN behavior before relying on it operationally.

Home separates connection status from the selected profile's saved settings. The per-app list controls which packages are eligible for VPN routing; it does not mean every request from those apps uses the remote server. With an empty package list, Android does not install an app filter. Expand the connection options for LAN listeners, QUIC blocking and the routing preset.

## Startup and runtime path

```text
Flutter VpnService.connect(configJson, vpnOptions)
  -> MethodChannel "supersocksr.ppp/vpn"
  -> MainActivity requests Android VPN permission when needed
  -> PppVpnService in the private :vpn process
  -> VpnService.Builder establishes a TUN interface
  -> JNI configures and runs libopenppp2.so
  -> native callbacks and a service poller mirror runtime state to app files
  -> Flutter polls the mirror while the app is visible
```

`PppVpnService` is deliberately declared in a separate `:vpn` process. The UI therefore does not read native state directly: it asks the activity for a mirrored runtime snapshot, link state, heartbeat, or last error. See [the technical guide](debug.md) before changing that lifecycle or any JNI signature.

## Work on the Flutter application

From this directory, with a compatible Flutter/Android toolchain installed:

```sh
flutter pub get
flutter test
```

A device run/build also needs a native library whose ABI matches the active Android Gradle configuration. The checked-in `jniLibs` directory currently contains an `arm64-v8a` library; do not assume that this also covers every emulator or build variant.

```sh
flutter run
```

Use a disposable development device and a non-production configuration. Do not put credentials or private endpoints in documentation, screenshots, or committed test profiles.

### Rebuild the native library (maintainers)

`CMakeLists.txt` builds `libopenppp2.so` from the shared C/C++ runtime and writes it under `bin/android/<ABI>/`. `build.sh` selects `x86`, `x64`, `arm`, `arm64`, or `all`; it requires an Android NDK plus matching prebuilt Boost and OpenSSL libraries.

A path-independent template for an arm64 build is:

```sh
cd android
NDK_ROOT=/path/to/android-ndk \
OTHER_ARGS="-DTHIRD_PARTY_LIBRARY_DIR=/path/to/android-third-party" \
./build.sh arm64

cp ../bin/android/arm64-v8a/libopenppp2.so \
  android/app/src/main/jniLibs/arm64-v8a/libopenppp2.so
```

The helper removes its temporary `build/` directory. Check the active Gradle scripts and package each required ABI deliberately before using the result. The local bootstrap/WSL helper scripts contain overwrite behavior or machine-specific paths, so they are not general onboarding commands.

## Documentation boundary

- [Technical guide](debug.md) documents the current Flutter/Kotlin/JNI implementation and troubleshooting signals.
- [Rule assets](android/app/src/main/assets/rules/README.md) documents the bundled GeoIP/GeoSite fallback files.
- [Work status](WORK_STATUS.md) is a status-bound maintenance note, not a record of current build or device-test success.

Configuration field semantics, protocol behavior, and cross-platform runtime guarantees belong to the canonical project documentation, not to this platform wrapper.
