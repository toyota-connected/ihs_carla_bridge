# ihs_carla_bridge

A dart:ffi plugin that bridges an **ivi-homescreen** `headless-vulkan` backend to a
**CARLA** (Unreal Engine 5, Vulkan RHI) process: it hands CARLA the Flutter UI as a
zero-copy Vulkan image pool over a Unix socket and pumps pointer input back. The
Flutter app opts into CARLA by loading this module; the shell stays free of any
simulator knowledge.

The shell renders and exports; **this module transports.** It resolves the shell's
in-process export ABI (`ihs_vk_export_get_api`) via `dlsym`, marshals the pool's
memory + semaphore file descriptors out to the consumer with `SCM_RIGHTS`, and
relays presentation and input over the `ihs-vk-export` wire protocol.

## Layout

```
include/ihs_carla_bridge/
  wire_protocol.h   ihs-vk-export protocol v1 — the bridge <-> CARLA contract
                    (message framing + payloads; fds ride SCM_RIGHTS)
  vk_export.h       the shell's in-process C ABI this module consumes
                    (ihs_vk_export_get_api function table + pool descriptors)
  bridge_c_api.h    the flat C ABI this module exports for dart:ffi
                    (create / start / stop / destroy, Dart event port)
src/
  scm_rights.{h,cc}           framed-message + fd-passing transport over a
                              SEQPACKET socket
  export_source.h             the abstract image source a session pulls from
                              (mocked in tests)
  shell_export_source.{h,cc}  the production source: ihs_vk_export_get_api
                              resolved with dlsym(RTLD_DEFAULT)
  session.{h,cc}              one consumer: handshake, frame present/release,
                              input, resize, bye
  server.{h,cc}               listen socket and IO thread; one consumer at a
                              time, listens again after a disconnect
  bridge.{h,cc}               the coordinator: owns the source and server, and
                              implements the C ABI
  module_exports.map          linker script exporting only ihs_carla_bridge_*
test/
  scm_rights_test.cc  fd round-trip over a socketpair
  session_test.cc     protocol session against a mock source
  server_test.cc      accept, serve and reconnect
  bridge_test.cc      the whole module over a real socket, against a fake
                      shell ABI
carla_ivi/
  lib/                Dart FFI bindings (ffigen) and the BridgeState stream
  test/               lifecycle and state stream tests
  example/            Flutter app that runs the bridge in the shell
unreal/
  IhsVkExport/        the CARLA-side consumer: an Unreal Engine 5 plugin
                      (source only; early, see Status)
```

## Design notes

- **Transport lives here, not in the shell.** The shell exposes exactly one
  default-visibility symbol, `ihs_vk_export_get_api`; the bridge never links it.
  This keeps the shell simulator-agnostic and lets the bridge rev independently.
- **OPAQUE_FD is the portable floor**; a dma-buf + DRM-format-modifier path is
  carried for cross-driver / KMS / GStreamer consumers. The consumer must match
  the advertised Vulkan `deviceUUID` for an OPAQUE_FD import to be valid.
- **Binary semaphores** (`render_done` / `consumer_done`) per pool image provide
  the cross-process GPU sync; the `FRAME_RELEASE` message is the CPU-side pacing
  edge that drives the backend's consumer-paced vsync.
- **Wire framing:** one SEQPACKET datagram per message = an 8-byte
  `{ type, len }` header + payload, with fds as ancillary `SCM_RIGHTS`. See
  `wire_protocol.h`.

## Status

Working:

- The wire protocol and shell ABI contracts, and the SCM_RIGHTS transport.
- The listening server: accept, serve one consumer, and listen again when it
  disconnects.
- The session: the `ihs-vk-export` handshake (hello, caps, image table), frame
  present and release, pointer input, resize, and bye. Readable input is
  drained before a hangup is honored.
- The bridge module: binds the shell's `ihs_vk_export_get_api` (in
  ivi-homescreen's headless-vulkan backend) and exposes a C ABI
  (`bridge_c_api.h`) for create, start, stop and destroy. State transitions are
  posted to a Dart native port.
- `carla_ivi`: the Dart FFI package that drives the bridge and streams its
  state, with an example app.
- Tests for the transport, session, server and bridge (`test/`), and for
  `carla_ivi` (`carla_ivi/test/`).

Validated on a Raspberry Pi 4 (V3D) with the example app: the bridge goes
`idle → listening → consumerConnected → listening` as a consumer connects and
disconnects.

Not done yet:

- Key input. `INPUT_KEY` is reserved in protocol v1 and ignored.
- An end-to-end run with CARLA importing and drawing the frames. The Unreal
  side is the `IhsVkExport` plugin in `unreal/`. So far it only requests the
  Vulkan external-memory and semaphore device extensions and logs the device
  UUID; the wire-protocol client and the texture import are not written yet.

## Build

```
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++23 compiler. Apache-2.0 licensed.

The Unreal plugin is not part of the CMake build. Link it into a CARLA
(Unreal Engine 5) checkout and build CARLA as usual:

```
ln -s "$PWD/unreal/IhsVkExport" <carla>/Unreal/CarlaUnreal/Plugins/IhsVkExport
```

Only the plugin's own source is kept here. It contains no Unreal Engine code,
and its build output (`Binaries/`, `Intermediate/`) is not committed.
