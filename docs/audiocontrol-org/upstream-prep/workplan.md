# Upstream Contribution Preparation - Workplan

**Source PRD:** [prd.md](./prd.md)
**Updated:** 2026-04-07

---

## Overview

Refactor the audiocontrol-org/scsi2pi fork to separate general-purpose SCSI initiator mode support (for upstream contribution) from MIDI-specific functionality (kept in an external service). The goal is to reduce the upstream PR from ~3,935 lines across 35 files to ~250 lines of general-purpose code.

### Current State

- **feature/midi-processor branch**: 28 commits, ~3,935 lines added
- MIDI-specific code embedded throughout s2p core
- MidiProcessor device, MidiStreamingServer, MIDI_* operations, controller hack
- General-purpose initiator mode support mixed in with MIDI logic

### Target State

- **Upstream PR**: ~250 lines of general-purpose initiator mode + SCSI_EXEC
- **scsi-midi-bridge**: Migrated to use only SCSI_EXEC (no MIDI_* ops, no streaming client)
- **Fork simplified**: Rebased on upstream with SCSI_EXEC, no MIDI code in s2p

---

## Phase 1: Clean Upstream PR Branch

**Goal:** Create a minimal branch from upstream/main with only general-purpose initiator mode support.

### Task 1.1: Bus interface extensions

Add virtual methods to `cpp/buses/bus.h` for initiator mode switching. Default implementations are no-ops so non-Pi platforms are unaffected.

- `SetInitiatorMode(bool)` — switch IND/DTD signals
- `SuspendSelectionEvent()` — pause GPIO event monitoring
- `ResumeSelectionEvent()` — resume GPIO event monitoring

**Files:**
- Modify: `cpp/buses/bus.h`

**Acceptance Criteria:**
- [ ] Virtual methods with default no-op implementations
- [ ] No behavioral change for existing code
- [ ] Builds cleanly

### Task 1.2: rpi_bus initiator mode implementation

Implement the three bus interface methods in RpiBus. Port from feature/midi-processor but **without debug fprintf statements**.

**Files:**
- Modify: `cpp/pi/rpi_bus.cpp`
- Modify: `cpp/pi/rpi_bus.h`

**Acceptance Criteria:**
- [ ] SetInitiatorMode uses PinSetSignal for IND/DTD
- [ ] SuspendSelectionEvent properly closes epoll/gpio handles
- [ ] ResumeSelectionEvent re-registers GPIO event
- [ ] No fprintf debug logging
- [ ] Builds cleanly

### Task 1.3: SCSI_EXEC protobuf messages

Add generic SCSI command execution messages to the protobuf API. No MIDI-specific operations or device types.

**Files:**
- Modify: `api/s2p_interface.proto`

**Acceptance Criteria:**
- [ ] `SCSI_EXEC = 210` operation added
- [ ] `PbScsiRequest` message with targetId, lun, cdb, dataOut, expectedDataIn, timeout
- [ ] `PbScsiResponse` message with status, senseData, dataIn, bytesTransferred
- [ ] Fields added to PbCommand and PbResult
- [ ] No MIDI_*, no SCMP device type, no PbMidiRequest/Response

### Task 1.4: CommandDispatcher SCSI_EXEC implementation

Add the command queue and physical bus execution path to CommandDispatcher. Use the existing constructor signature if possible (pass Bus via a setter or optional parameter to avoid a breaking change).

**Files:**
- Modify: `cpp/command/command_dispatcher.cpp`
- Modify: `cpp/command/command_dispatcher.h`

**Acceptance Criteria:**
- [ ] ScsiCommand struct with queue synchronization
- [ ] ExecuteScsi() entry point for protobuf dispatch
- [ ] ProcessScsiQueue() for main loop integration
- [ ] ProcessScsiQueuePhysical() uses InitiatorExecutor
- [ ] Bus mode switching with SuspendSelectionEvent/ResumeSelectionEvent
- [ ] No emulated device routing (physical targets only)
- [ ] Constructor signature change is minimal/non-breaking

### Task 1.5: Main loop integration

Add ProcessScsiQueue() call to the main loop in s2p_core.cpp. No BUILD_SCMP conditionals.

**Files:**
- Modify: `cpp/s2p/s2p_core.cpp`
- Modify: `cpp/s2p/s2p_core.h`

**Acceptance Criteria:**
- [ ] ProcessScsiQueue() called each iteration of main loop
- [ ] No MIDI-specific code
- [ ] No BUILD_SCMP guards
- [ ] Builds cleanly

### Task 1.6: WaitForSelection timeout

Make the epoll_wait timeout configurable rather than changing from infinite to 100ms for all users.

**Files:**
- Modify: `cpp/pi/rpi_bus.cpp`

**Acceptance Criteria:**
- [ ] Timeout configurable (default: 100ms when SCSI queue is active, infinite otherwise)
- [ ] No debug fprintf logging
- [ ] No behavioral change for users without SCSI_EXEC

### Task 1.7: Tests

Add unit tests for SCSI_EXEC functionality.

**Files:**
- Modify: `cpp/test/command_dispatcher_test.cpp`

**Acceptance Criteria:**
- [ ] Test SCSI_EXEC queue and dispatch
- [ ] Test timeout handling
- [ ] Test data in/out phases
- [ ] Existing tests still pass

**Phase 1 Verification:** Full build passes. All existing tests pass. No MIDI-specific code.

---

## Phase 2: Migrate scsi-midi-bridge to SCSI_EXEC-Only

**Goal:** Remove all MIDI_* protobuf operations and MidiStreamingServer (port 6870) dependency from the existing Rust scsi-midi-bridge. The bridge already has SCSI_EXEC-based equivalents for all MIDI operations (`scsi_midi_send/poll/read` in `s2p_client.rs`), and `scsi_midi.rs` (SDS transfers) already uses them exclusively. This phase is mostly deletion.

### Current Bridge Architecture

The bridge (`services/scsi-midi-bridge/`) has two transport paths to s2p:

| Path | Operations | Port | Used by |
|------|-----------|------|---------|
| Protobuf MIDI_* | MIDI_INIT, MIDI_SEND, MIDI_POLL, MIDI_READ | 6868 | `send_sysex`, `poll`, `read`, `send_and_receive` |
| Streaming client | Binary TCP protocol (MSG_INIT, MSG_SEND, MSG_DATA) | 6870 | `send_and_receive` (primary path) |
| SCSI_EXEC | Generic CDB execution | 6868 | `scsi_midi_*` methods, disk ops, INQUIRY, READ CAPACITY |

After refactor, only SCSI_EXEC remains.

### Task 2.1: Remove MIDI_* operations from s2p_client.rs

Delete the MIDI-specific protobuf message builders and operations. Rewrite `send_and_receive` to use the existing `scsi_midi_send/poll/read` methods (which already use SCSI_EXEC).

**Files:**
- Modify: `services/scsi-midi-bridge/src/s2p_client.rs`

**Remove:**
- Constants: `MIDI_INIT`, `MIDI_SEND`, `MIDI_POLL`, `MIDI_READ` (~4 lines)
- Functions: `build_midi_request`, `build_command`, `build_midi_init`, `build_midi_send`, `build_midi_poll`, `build_midi_read` (~45 lines)
- Methods: `ensure_init()`, `send_sysex()`, `poll()`, `read()` (~60 lines)
- Rewrite: `send_and_receive()` to use `scsi_midi_enable` + `scsi_midi_send` + `scsi_midi_poll` + `scsi_midi_read`

**Keep unchanged:**
- `execute_scsi()` and all `build_scsi_*` functions
- `scsi_midi_enable/disable/send/poll/read` methods (already use SCSI_EXEC)
- `send_command()` TCP client (still needed for SCSI_EXEC)

**Acceptance Criteria:**
- [ ] No MIDI_* constants or message builders remain
- [ ] `send_and_receive` uses `scsi_midi_*` methods via SCSI_EXEC
- [ ] All HTTP endpoints still work (SysEx send/receive, SDS transfer, disk ops)

### Task 2.2: Remove MidiStreamClient from s2p_client.rs

Delete the entire streaming client (binary TCP protocol to port 6870).

**Files:**
- Modify: `services/scsi-midi-bridge/src/s2p_client.rs`

**Remove:**
- Constants: `MSG_INIT`, `MSG_SEND`, `MSG_DATA`, `MSG_ERROR` (~4 lines)
- Functions: `write_frame`, `read_frame` (~25 lines)
- Struct: `MidiStreamClient` and all its methods (~170 lines)

**Acceptance Criteria:**
- [ ] No `MidiStreamClient` struct or binary protocol code remains
- [ ] ~200 lines removed

### Task 2.3: Remove streaming client from main.rs and routes.rs

Remove MidiStreamClient from application state and route handlers.

**Files:**
- Modify: `services/scsi-midi-bridge/src/main.rs`
- Modify: `services/scsi-midi-bridge/src/routes.rs`
- Modify: `services/scsi-midi-bridge/src/config.rs` (if `midi_port` config exists)

**Changes in main.rs:**
- Remove `MidiStreamClient::new()` instantiation
- Remove `midi_stream` from `AppState`
- Remove `config.midi_port` usage

**Changes in routes.rs:**
- Remove `midi_stream` from `AppState` struct
- `sds_send`: Remove streaming client as primary path; use `send_and_receive` directly (now SCSI_EXEC-based)
- `sds_poll`: Rewrite to use `scsi_midi_poll/read` instead of `poll/read` (MIDI_* based)
- `handle_ws_send`: Remove streaming client fallback; use `send_and_receive` directly

**Acceptance Criteria:**
- [ ] No `MidiStreamClient` references in any file
- [ ] No `midi_port` configuration
- [ ] All routes use SCSI_EXEC path exclusively
- [ ] Bridge connects to s2p on port 6868 only

### Task 2.4: Update scsi_midi.rs send_and_receive calls

Verify `scsi_midi.rs` needs no changes. It already calls `scsi_midi_send/poll/read` on `S2pClient`, which use SCSI_EXEC. Confirm the `S2pClient` method signatures haven't changed in a way that breaks it.

**Files:**
- Review: `services/scsi-midi-bridge/src/scsi_midi.rs` (expect no changes)

**Acceptance Criteria:**
- [ ] `scsi_midi.rs` compiles without changes
- [ ] SDS download and upload still work via SCSI_EXEC

### Task 2.5: Build and test

Build the bridge and verify all endpoints work.

**Acceptance Criteria:**
- [ ] `cargo build` succeeds
- [ ] `cargo test` passes (existing unit tests in scsi_midi.rs)
- [ ] `/health`, `/status` endpoints respond
- [ ] `/sds/send` sends SysEx via SCSI_EXEC
- [ ] `/sds/stream` WebSocket handles sample-download/upload via SCSI_EXEC
- [ ] `/scsi/exec`, `/scsi/inquiry`, `/scsi/read`, `/scsi/write` unchanged

**Phase 2 Verification:** Bridge builds, tests pass, no connection to port 6870.

---

## Phase 3: Fork Cleanup

**Goal:** Simplify audiocontrol-org/scsi2pi fork by removing all MIDI-specific code and rebasing on upstream with SCSI_EXEC.

### Task 3.1: Remove MIDI-specific code

Remove all MIDI-specific files and code from the fork.

**Files to delete:**
- `cpp/devices/midi_processor.cpp`
- `cpp/devices/midi_processor.h`
- `cpp/s2p/midi_streaming_server.cpp`
- `cpp/s2p/midi_streaming_server.h`

**Files to modify (remove MIDI code):**
- `cpp/base/device_factory.cpp` — remove SCMP registration
- `cpp/base/device_factory.h` — remove "midi" mapping
- `cpp/base/primary_device.h` — remove SCMP from type mapping
- `cpp/command/command_dispatcher.cpp` — remove ExecuteMidi, MidiCommand queue
- `cpp/command/command_dispatcher.h` — remove MidiCommand struct
- `cpp/controllers/controller.cpp` — remove 0x0D SCMP hack
- `cpp/s2p/s2p_core.cpp` — remove BUILD_SCMP conditionals
- `cpp/s2p/s2p_core.h` — remove MidiStreamingServer member
- `api/s2p_interface.proto` — remove SCMP, MIDI_*, PbMidiRequest/Response
- `cpp/Makefile` — remove BUILD_SCMP flag and midi_processor targets

### Task 3.2: Rebase on upstream

Once upstream accepts the initiator mode PR, rebase the fork to use upstream's SCSI_EXEC.

**Acceptance Criteria:**
- [ ] Fork is minimal diff from upstream (only audiocontrol-specific config if any)
- [ ] SCSI_EXEC works via upstream code
- [ ] No MIDI-specific code in fork

### Task 3.3: End-to-end verification

Test the full stack: audiocontrol web editor → scsi-midi-server → s2p → S3000XL.

**Acceptance Criteria:**
- [ ] SysEx round-trip works
- [ ] SDS sample transfer works
- [ ] Latency acceptable for interactive use

---

## Phase 4: Submit Upstream PR

**Goal:** Submit clean PR(s) to uweseimet/scsi2pi.

### Task 4.1: Prepare PR

- Clean commit history (squash WIP commits)
- Write clear PR description explaining use case
- Reference scsi2pi's existing initiator mode infrastructure

### Task 4.2: Address review feedback

Iterate on upstream maintainer feedback.

---

## Dependency Graph

```
Phase 1 (upstream PR branch)
  1.1 (bus.h) -> 1.2 (rpi_bus)
  1.3 (proto) -> 1.4 (dispatcher) -> 1.5 (main loop)
  1.2, 1.4 -> 1.6 (timeout)
  1.5 -> 1.7 (tests)

Phase 2 (bridge migration) — independent of Phase 1
  2.1 (remove MIDI_*) -> 2.2 (remove MidiStreamClient) -> 2.3 (routes cleanup)
  2.3 -> 2.4 (verify scsi_midi.rs) -> 2.5 (build + test)

Phase 3 (fork cleanup) — after Phase 1 upstream PR is merged
  3.1 -> 3.2 -> 3.3

Phase 4 (submit) — after Phase 1 is complete
  4.1 -> 4.2
```

---

## Risk Mitigation

| Risk | Mitigation |
|------|------------|
| Upstream maintainer rejects initiator mode | Design is general-purpose; emphasize value for SCSI debugging, bridge use cases |
| Constructor signature break | Use optional Bus* parameter with nullptr default |
| WaitForSelection timeout affects stability | Make timeout configurable; default to infinite when no SCSI queue activity |
| Bridge latency regression after removing streaming client | `scsi_midi_*` methods already work at same speed; streaming client was a fallback optimization, not a requirement |

---

## Critical Files

### scsi2pi (upstream PR)

| File | Role |
|------|------|
| `cpp/buses/bus.h` | Virtual interface for initiator mode |
| `cpp/pi/rpi_bus.cpp` | GPIO-level initiator mode implementation |
| `cpp/command/command_dispatcher.cpp` | SCSI_EXEC queue and execution |
| `api/s2p_interface.proto` | Protobuf API for SCSI_EXEC |
| `cpp/s2p/s2p_core.cpp` | Main loop integration |

### scsi-midi-bridge (Phase 2 migration)

| File | Role |
|------|------|
| `services/scsi-midi-bridge/src/s2p_client.rs` | Remove MIDI_* ops and MidiStreamClient; keep SCSI_EXEC |
| `services/scsi-midi-bridge/src/routes.rs` | Remove streaming client fallback from handlers |
| `services/scsi-midi-bridge/src/main.rs` | Remove MidiStreamClient from app state |
| `services/scsi-midi-bridge/src/scsi_midi.rs` | No changes expected — already uses SCSI_EXEC |
