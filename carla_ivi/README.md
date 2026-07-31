# carla_ivi

Dart FFI bindings for the `ihs_carla_bridge` native module.

The bridge is a shared object loaded into the ivi-homescreen shell process; it
resolves the shell's exported `ihs_vk_export_get_api` symbol, hands the
headless-vulkan backend's exported frame pool to a CARLA (Unreal Engine, Vulkan)
consumer over a Unix socket, and pumps input back. This package lets a
Flutter/Dart app drive that module's lifecycle instead of the shell's env-gated
native loader.

## Usage

```dart
import 'package:carla_ivi/carla_ivi.dart';

final bridge = CarlaBridge.open('/run/user/1000/ihs-vk-export.sock');
if (!bridge.start()) {
  // Not running inside the headless-vulkan shell, or the socket is busy.
}
// ... later ...
bridge.stop();
bridge.dispose();
```

`CarlaBridge.open` loads `libihs_carla_bridge.so` from the platform search path
(override with `libraryName:` or pass an already-open `library:`). `start()`
returns `false` when the host is not the headless-vulkan shell (no export API is
resolved) or the socket cannot be bound.

## Bindings

`lib/src/carla_ivi_bindings.g.dart` is generated from
`../include/ihs_carla_bridge/bridge_c_api.h`. Regenerate after an ABI change:

```sh
dart run ffigen --config ffigen.yaml
```

## Status

Lifecycle (create / start / stop / dispose) is wired and covered by
`test/carla_ivi_test.dart` (set `IHS_CB_LIB` to a built `libihs_carla_bridge.so`).
The `BridgeState` vocabulary in `lib/src/bridge_state.dart` is in place; streaming
live transitions over a Dart native port is the next step and needs a matching
native event extension to the C ABI.
