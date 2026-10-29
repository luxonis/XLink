# Handoff: XLink pipelining (return after send)

Status on 2026-10-30: works and is tested on loopback and on one RVC2 PoE device (2 h at
880 Mbit/s on a direct link). Not merged. Remove this file before the merge.

## Problem

Before this change, `XLinkWriteData*` waited for the remote `WRITE_RESP`, and `XLinkReleaseData`
(also inside `XLinkReadMoveData`) waited for `READ_REL_RESP`. So each thread could send at most
one message per round trip (RTT), and an RPC call cost 3 RTT. The responses carry no data for the
caller (they are always an ACK). The flow control already uses the stream credit
(`remoteFillLevel`, bytes and `XLINK_MAX_PACKETS_PER_STREAM`), so the wait is not necessary.

## What the change does

- After the dispatcher sends a `WRITE_REQ`, `WRITE_FD_REQ`, `READ_REL_REQ` or `READ_REL_SPEC_REQ`,
  it releases the caller at once and frees the queue slot. The response only lowers a counter
  (`pipelineInFlight`). The caller can reuse its buffer: the data is in the transport when the
  call returns.
- The number of such requests in flight on one link is limited by the window
  (`pipelineWindow`). When the window is full, the request waits for its response (old mode).
- Order on one stream does not change: a caller adds its next event only after its previous
  event is sent, the link is ordered, and the receiver keeps a FIFO for each stream. A write
  that waits for credit still blocks its caller.

### Negotiation (wire)

- The wire format of all events does not change. A new flag bit `pipelining` (bit 9 of
  `xLinkEventHeader_t.flags`) is used only in `PING_REQ` and `PING_RESP`. When it is set,
  `header.size` is the window that the sender accepts.
- The client sends it in `PING_REQ` (in `XLinkConnect`); the server answers in `PING_RESP`.
  Each side then uses `min(local window, peer window)`.
- Old peers send `flags.raw = 0` plus ACK and ignore unknown bits, so a link to an old peer
  (old firmware, bootloader) stays in the old mode. Do not read `header.size` of a PING without
  the flag: old peers do not set it.

### Configuration

- `XLINK_PIPELINE_WINDOW_DEFAULT` = 16, `XLINK_PIPELINE_WINDOW_MAX` = `MAX_EVENTS / 2` = 32.
- Env var `XLINK_PIPELINE_WINDOW` (read in `DispatcherInitialize`): 0 turns pipelining off on
  this side (and so on the link). A smaller value on one side limits both directions.
- Pipelining is on by default when both ends support it.

### Other fixes in the same change (found during the work)

1. Remote events: the reader thread dropped an incoming event when the remote queue was full
   (only a log line). A lost `READ_REL_REQ` lost stream credit for ever, a lost `WRITE_REQ` left
   the peer waiting. Now the reader waits for a free slot.
2. `DispatcherAddEvent_` posted the dispatcher semaphore also when the add failed. Then
   `dispatcherGetNextEvent` returned NULL and `sendEvents` stopped ("Dispatcher received NULL
   event!"), so the link went down. Now it posts only for an added event.
3. `getNextQueueElemToProc` scanned from the last processed slot. When slots are freed out of
   order, it could take a newer event first and then miss the older one (and return NULL).
   Now it takes the oldest allocated event (sequence number `seq`).
4. The four write/release responses are served in the reader thread. They do not use the
   remote queue and do not wait while the scheduler sends a large packet.

## Files

| File | Change |
|---|---|
| `include/XLink/XLinkPrivateDefines.h` | `pipelining` flag bit, window defines |
| `include/XLink/XLinkDispatcher.h` | `DispatcherLocalPipelineWindow`, `DispatcherSetPeerPipelineWindow` |
| `src/shared/XLinkDispatcher.c` | early release, counter, reader-thread responses, no drop, no extra post, oldest-first pick |
| `src/shared/XLinkDispatcherImpl.c` | PING handlers announce and apply the window |
| `include/XLink/XLink.h` | doc notes on `XLinkWriteData` and `XLinkReleaseData` |

## Meaning of a successful write

A successful write now means "sent", not "the remote has it". The loss is small: the response
was always an ACK, and on link loss the old code could also return success.

## Results

### Loopback with a delay proxy (host library on both ends)

| Case | Old | New |
|---|---|---|
| RTT 1.6 ms, 64 B | 549 msg/s | 8,658 msg/s (window 32: 13,372) |
| RTT 1.6 ms, 64 KB | 34 MiB/s | 529 MiB/s |
| RTT 1.6 ms, 1 MB | 496 MiB/s | 3,129 MiB/s |
| RTT 1.6 ms, ping-pong (one RPC) | 5.5 ms | 1.76 ms |
| RTT 0.13 ms, 64 B | 3,705 msg/s | 42,090 msg/s |
| RTT 0.13 ms, ping-pong | 818 us | 283 us |

- Old and new ends together (both directions): old mode, about 568 msg/s, 5.3 ms ping-pong.
- Stress test (26 streams, sequence numbers checked, slow readers, 3 MiB frames): 0 errors.
- With a 16-slot queue (test build): the old code dropped events, logged "Dispatcher received
  NULL event!" and hung; the new code waited for free slots and passed.

### RVC2 PoE device (1 GbE, ping RTT 0.13 ms), old host + old firmware vs new host + new firmware

The RVC2 firmware uses a separate device-side XLink; a matching change was made there (not in
this repo).

| Case | Old | New |
|---|---|---|
| device to host, 64 B | 1,332 msg/s | 1,596 msg/s |
| host to device, 64 B | 1,324 msg/s | 1,603 msg/s |
| device to host, 64 KB | 47 MiB/s | 79 MiB/s |
| device to host, 1 MB | 96 MiB/s | 106 MiB/s |
| RPC (`getChipTemperature`) | 2.03 ms | 1.53 ms |

- On this device the CPU cost for each message (about 0.6 ms) is larger than the RTT, so small
  messages gain only about 20%. The largest gain is for messages of about 16-256 KB.
- Cameras (1920x1080 YUV420 + 2 x 1280x800 mono, 15 fps, 74 MiB/s): the same bandwidth; median
  latency of the mono streams 68 -> 41 ms, color stream 69 -> 74 ms. At 30 fps the device CPU is
  the limit in both versions (about 82 -> 88 MiB/s).
- Fixed-rate load with the same message sizes: target 600 Mbit/s equal; target 1000 Mbit/s:
  838 and 852 -> 917 and 917 Mbit/s (TCP maximum about 941 Mbit/s).
- Compatibility: the link to the RVC2 bootloader stays in the old mode (window 0) and the boot
  works. New host + old firmware and old host + new firmware work in the old mode.
- depthai-core on-device tests `benchmark_test`, `xlink_test`, `message_group_test`,
  `regression_message_group_test` pass with new host + new firmware.

RVC4 and USB links are not tested yet.

## Stalls at the link limit: resolved (2026-10-30)

The 1-4 s stalls seen at a 900 Mbit/s load came from the network path (switch and cables), not
from this change: packets were lost between the device NIC and the host NIC (device NIC TX OK
equal to the frames handed over, 0 TX errors, PHY idle errors 0, host NIC and TCP error counters
0, holes in the TCP sequence at the host). On a direct cable the loss is gone. The device side
only made each loss expensive: with ~97% line load its TCP stack falls back to retransmission
timeouts, which is the same with and without this change.

### Direct link, RVC2 PoE device, 30 fps camera pipeline (~885 Mbit/s), 24 runs of 5 minutes

Both ends with pipelining (window 16). Latency per frame, capture to host receive, median of the
per-run values (worst run in brackets):

| Stream | mean | p75 | p95 | p99 | max frame |
|---|---|---|---|---|---|
| 1280x720 RGB | 44.1 (45.5) | 46.5 (48.4) | 50.0 (52.8) | 52.3 (55.4) | 72.7 ms |
| 1280x720 mono L | 23.2 (25.2) | 26.1 (29.4) | 33.5 (36.7) | 42.8 (46.5) | 54.8 ms |
| 1280x720 mono R | 28.6 (30.0) | 31.6 (33.5) | 38.5 (43.7) | 45.1 (48.9) | 59.4 ms |
| 640x360 depth | 55.2 (57.3) | 61.8 (64.1) | 70.2 (77.6) | 79.0 (82.0) | 92.7 ms |

880 Mbit/s mean, 2 seconds below 90% of the median in 7,104 s, 0 out-of-order packets, 0 device
RX misses. With the old protocol on the same device the mono latency was about 15 ms higher
(stop-and-wait keeps a small frame behind a large one on the same link).

## Next steps

1. Test on RVC4 (network and USB) and on RVC2 USB.
2. Build and test on Windows and macOS.
3. Decide if pipelining is on by default.

## Merge plan

The negotiation makes each side safe alone, so the order is not strict:

1. This XLink PR.
2. depthai-core: bump the XLink `GIT_TAG` (host, and RVC4 firmware that builds XLink through
   depthai-core).
3. Device firmware with the matching change, then the firmware pins in depthai-core.

## Tests

The test programs (loopback bench, delay proxy, stress test, device benchmarks) are kept outside
this repo, because XLink CI does not run tests. The results above come from them.
