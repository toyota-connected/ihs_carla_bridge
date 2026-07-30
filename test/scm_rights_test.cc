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

// Self-contained transport test: a framed message and an fd sent through a
// socketpair arrive intact, and the received fd is a live handle to the same
// underlying object (proving SCM_RIGHTS fd passing). No external deps.

#include "scm_rights.h"

#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

using namespace ihs_vke;

namespace {
int g_failures = 0;
void Check(bool cond, const char* what) {
  if (!cond) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++g_failures;
  }
}
}  // namespace

int main() {
  int sv[2];
  Check(::socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) == 0, "socketpair");

  // 1) A message carrying one fd (a pipe read end with a marker written in).
  int p[2];
  Check(::pipe(p) == 0, "pipe");
  const char kMark[4] = {'M', 'A', 'R', 'K'};
  Check(::write(p[1], kMark, sizeof(kMark)) ==
            static_cast<ssize_t>(sizeof(kMark)),
        "write marker");

  FrameRelease sent{/*slot=*/2, /*generation=*/7};
  Check(
      SendFramed(sv[0], MsgType::kFrameRelease, &sent, sizeof(sent), &p[0], 1),
      "SendFramed with fd");

  RecvMsg r = RecvFramed(sv[1]);
  Check(r.ok, "recv ok");
  Check(!r.closed && !r.truncated, "recv not closed/truncated");
  Check(r.type == MsgType::kFrameRelease, "recv type");
  Check(r.payload.size() == sizeof(FrameRelease), "recv payload size");
  if (r.payload.size() == sizeof(FrameRelease)) {
    FrameRelease got{};
    std::memcpy(&got, r.payload.data(), sizeof(got));
    Check(got.slot == 2 && got.generation == 7, "recv payload contents");
  }
  Check(r.fds.size() == 1, "recv fd count");
  if (r.fds.size() == 1) {
    // The passed fd is a different number but the same underlying pipe: reading
    // it yields the marker written to the original write end.
    Check(r.fds[0] != p[0], "received fd is a distinct number");
    char buf[4] = {0};
    const ssize_t rd = ::read(r.fds[0], buf, sizeof(buf));
    Check(rd == static_cast<ssize_t>(sizeof(buf)), "read passed fd");
    Check(std::memcmp(buf, kMark, sizeof(kMark)) == 0,
          "passed fd is same pipe");
  }
  CloseFds(r);
  ::close(p[0]);
  ::close(p[1]);

  // 2) A fd-less POD message round-trips with no ancillary data.
  Bye bye{static_cast<uint32_t>(ByeReason::kNormal)};
  Check(SendPod(sv[0], MsgType::kBye, bye), "SendPod no fd");
  RecvMsg r2 = RecvFramed(sv[1]);
  Check(r2.ok && r2.type == MsgType::kBye, "recv Bye");
  Check(r2.payload.size() == sizeof(Bye) && r2.fds.empty(),
        "Bye payload/no fds");

  // 3) An orderly peer close is reported as closed, not an error.
  ::close(sv[0]);
  RecvMsg r3 = RecvFramed(sv[1]);
  Check(!r3.ok && r3.closed, "peer close detected");
  ::close(sv[1]);

  if (g_failures == 0) {
    std::printf("scm_rights_test: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "scm_rights_test: %d failure(s)\n", g_failures);
  return 1;
}
