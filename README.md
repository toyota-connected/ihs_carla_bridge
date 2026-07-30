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
src/
  scm_rights.{h,cc} framed-message + fd-passing transport over a SEQPACKET socket
test/
  scm_rights_test.cc  fd round-trip over a socketpair (no external deps)
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

Foundational: the wire-protocol and shell-ABI contracts, and the SCM_RIGHTS
transport with a passing fd round-trip test. Still to come: the socket session
state machine + IO thread, the `carla_ivi` Dart package (ffigen bindings), and
the shell-side `ihs_vk_export_get_api` implementation the module binds to.

## Build

```
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Requires a C++23 compiler. Apache-2.0 licensed.
