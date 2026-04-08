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
- **External scsi-midi-server**: All MIDI protocol logic, binary streaming server
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

## Phase 2: External scsi-midi-server

**Goal:** Create a standalone service that implements all MIDI-over-SCSI protocol logic, communicating with s2p via SCSI_EXEC.

### Task 2.1: S2pClient — protobuf client

Implement a client that connects to s2p's protobuf socket and sends SCSI_EXEC commands.

**Files:**
- Create: `src/scsi/S2pClient.ts`
- Create: `src/scsi/ScsiExecutor.ts`

**Acceptance Criteria:**
- [ ] Connects to s2p protobuf socket (default port 6868)
- [ ] Sends PbCommand with SCSI_EXEC operation
- [ ] Receives PbResult with ScsiResponse
- [ ] Connection error handling and reconnection

### Task 2.2: AkaiProtocol — MIDI-via-SCSI commands

Port the Akai MIDI-over-SCSI protocol from MidiStreamingServer.cpp.

**Files:**
- Create: `src/midi/AkaiProtocol.ts`

**Acceptance Criteria:**
- [ ] initSession() — CDB 0x09
- [ ] sendSysEx(data) — CDB 0x0C with 3-byte length encoding
- [ ] poll() — CDB 0x0D, returns pending byte count
- [ ] readData(length) — CDB 0x0E with 3-byte length encoding
- [ ] sendAndReceive() — high-level send + poll + read loop

### Task 2.3: SDS transfer support

Port SDS sample transfer logic from ReceiveSample in MidiStreamingServer.cpp.

**Files:**
- Create: `src/midi/SdsTransfer.ts`

**Acceptance Criteria:**
- [ ] SDS Dump Request generation
- [ ] SDS header parsing (bits-per-sample, sample rate, length)
- [ ] Data packet collection via polling
- [ ] ACK generation for each packet
- [ ] 7-bit decoding of audio data

### Task 2.4: Binary streaming server

Port the TCP streaming server with binary length-delimited protocol.

**Files:**
- Create: `src/server/MidiStreamingServer.ts`
- Create: `src/server/BinaryProtocol.ts`

**Acceptance Criteria:**
- [ ] TCP server on configurable port (default 6870)
- [ ] Binary message protocol: [4-byte LE length][1-byte type][payload]
- [ ] MSG_INIT, MSG_SEND, MSG_DATA, MSG_ERROR, MSG_SAMPLE_READ
- [ ] Client session management
- [ ] Compatible with existing audiocontrol MIDI client

### Task 2.5: Integration and CLI

Wire components together with CLI entry point.

**Files:**
- Create: `src/main.ts`
- Create: `src/config/Config.ts`

**Acceptance Criteria:**
- [ ] CLI with --s2p-host, --s2p-port, --listen-port options
- [ ] Connects to s2p on startup
- [ ] Starts streaming server
- [ ] Graceful shutdown

**Phase 2 Verification:** Service starts, connects to s2p, handles MIDI client connections.

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

Phase 2 (scsi-midi-server) — independent of Phase 1
  2.1 (S2pClient) -> 2.2 (AkaiProtocol) -> 2.3 (SDS)
  2.1 -> 2.4 (streaming server)
  2.2, 2.3, 2.4 -> 2.5 (integration)

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
| 10ms polling latency too slow for SDS | Measure actual latency; fall back to native addon for tight polling if needed |
| Constructor signature break | Use optional Bus* parameter with nullptr default |
| WaitForSelection timeout affects stability | Make timeout configurable; default to infinite when no SCSI queue activity |

---

## Critical Files

| File | Role |
|------|------|
| `cpp/buses/bus.h` | Virtual interface for initiator mode |
| `cpp/pi/rpi_bus.cpp` | GPIO-level initiator mode implementation |
| `cpp/command/command_dispatcher.cpp` | SCSI_EXEC queue and execution |
| `api/s2p_interface.proto` | Protobuf API for SCSI_EXEC |
| `cpp/s2p/s2p_core.cpp` | Main loop integration |
