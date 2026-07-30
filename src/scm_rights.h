/*
 * Copyright 2026 Toyota Connected North America
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Framed message transport over a Unix SEQPACKET socket with SCM_RIGHTS fd
// passing -- the wire mechanics for the ihs-vk-export protocol. One message =
// one datagram: an 8-byte MsgHeader { type, len } followed by `len` payload
// bytes, with any file descriptors carried as ancillary SCM_RIGHTS data on the
// same datagram. The receiver owns (and must close) every fd it is handed.

#ifndef IHS_CARLA_BRIDGE_SCM_RIGHTS_H_
#define IHS_CARLA_BRIDGE_SCM_RIGHTS_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ihs_carla_bridge/wire_protocol.h"

namespace ihs_vke {

// Largest payload the protocol defines (an image table for a full pool), plus
// slack for a Hello name. Sizes the receive buffer; a larger datagram is a
// protocol error (reported as truncated).
inline constexpr size_t kMaxPayloadBytes =
    sizeof(ImageTableHeader) + kMaxSlots * sizeof(ImageDesc) + 256;

// Send one framed message: header{type,len} + payload, with `nfds` descriptors
// as SCM_RIGHTS. Returns true iff the whole datagram was sent. Does not take
// ownership of the fds (the caller keeps and closes its own copies).
bool SendFramed(int sock, MsgType type, const void* payload, size_t payload_len,
                const int* fds, size_t nfds);

// Convenience: send a POD payload with no fds.
template <typename T>
inline bool SendPod(int sock, MsgType type, const T& payload) {
  return SendFramed(sock, type, &payload, sizeof(T), nullptr, 0);
}

// One received datagram.
struct RecvMsg {
  MsgType type{};
  std::vector<uint8_t> payload;
  std::vector<int> fds;    // owned by the caller; close each when done
  bool ok = false;         // a well-formed message was received
  bool closed = false;     // the peer closed the connection (ok == false)
  bool truncated = false;  // payload or fds did not fit (protocol error)
};

// Receive exactly one datagram. Never blocks past one message. On a clean peer
// close returns { ok=false, closed=true }. On a short/oversized datagram
// returns { ok=false, truncated=true } and closes any fds it did receive.
RecvMsg RecvFramed(int sock);

// Close every fd in a RecvMsg (helper for error paths).
void CloseFds(RecvMsg& m);

}  // namespace ihs_vke

#endif  // IHS_CARLA_BRIDGE_SCM_RIGHTS_H_
