# RDP Performance Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reduce RDP bandwidth and latency by adding frame pacing/backpressure first, then adding a SurfaceBits graphics path with NSCodec when the client advertises support.

**Architecture:** Keep the existing protocol module boundaries. `rf-rdp-core` owns wire-format parsing/writing and small testable helpers, `rf-rdp-server` owns per-client state and send policy, and `rf-config` exposes optional tuning keys. BitmapUpdate remains the fallback for clients or configurations that cannot use SurfaceBits/NSCodec.

**Tech Stack:** C11, GLib/GIO, Meson tests, native RDP slow-path PDUs, FreeRDP used only as protocol reference.

---

### Task 1: Frame Pacing And Backpressure

**Files:**
- Modify: `reframe-server/rf-rdp-core.[ch]`
- Modify: `reframe-server/rf-rdp-server.c`
- Test: `tests/test-rdp-core.c`

- [x] Add a pure helper that decides whether a frame should be sent, skipped, or force-sent based on elapsed time, configured max fps, and whether a client still needs a full frame.
- [x] Verify the helper fails before implementation with cases for first frame, too-soon delta frame, forced full frame, and delayed delta frame.
- [x] Use the helper in `rf-rdp-server.c` before expensive per-client conversion, and record skipped/sent frame counters.
- [x] Log periodic RDP stats: frame bytes, sent frames, skipped frames, full-frame clients, and send time.

### Task 2: SurfaceBits Wire Format And Capability Parsing

**Files:**
- Modify: `reframe-server/rf-rdp-core.[ch]`
- Modify: `reframe-server/rf-rdp-server.c`
- Test: `tests/test-rdp-core.c`

- [x] Parse Confirm Active capability sets enough to detect `CAPSET_TYPE_SURFACE_COMMANDS`, `CAPSET_TYPE_BITMAP_CODECS`, NSCodec codec id, and RemoteFX codec id.
- [x] Add `rf_rdp_core_write_surface_bits()` for `CMDTYPE_SET_SURFACE_BITS` and `CMDTYPE_STREAM_SURFACE_BITS`.
- [x] Verify generated SurfaceBits packets include the Update Data PDU wrapper, command type, destination rect, bitmap data header, codec id, dimensions, payload length, and payload bytes.
- [x] Route `graphics=bitmap|surface-nsc|auto` so unsupported SurfaceBits falls back to BitmapUpdate.

### Task 3: Minimal NSCodec Encoder

**Files:**
- Create: `reframe-server/rf-rdp-nsc.[ch]`
- Modify: `reframe-server/meson.build`
- Modify: `reframe-server/rf-rdp-server.c`
- Test: `tests/test-rdp-nsc.c`

- [x] Add a minimal NSCodec encoder context for 32bpp RGBA input, using FreeRDP's NSCodec layout as the protocol reference.
- [x] Write tests that encode small solid-color and mixed-color rectangles and verify packet structure, dimensions, and deterministic output.
- [x] Encode the RDP damage rect to NSCodec payload and send it through SurfaceBits when negotiated.
- [x] Keep BitmapUpdate fallback for unsupported client capabilities, encoder failure, or oversized payload.

### Task 4: Remote Deployment Verification

**Files:**
- Remote only: `/home/kogeki/reframe-rdp-backend`

- [x] Run `meson compile -C build-rdp`.
- [x] Run `meson test -C build-rdp --print-errorlogs`.
- [x] Install to `/opt/reframe-rdp-test` and restart `reframe-server@card2-DP-7.service`.
- [ ] Verify logs show negotiated graphics mode, frame pacing counters, and lower bytes per update for NSCodec clients.
