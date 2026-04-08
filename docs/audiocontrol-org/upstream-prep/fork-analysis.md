# scsi2pi Fork Analysis for Upstream Contribution

**Date:** 2026-04-07
**Fork:** `audiocontrol-org/scsi2pi`
**Upstream:** `uweseimet/scsi2pi`
**Branch:** `feature/midi-processor`

---

## Executive Summary

The fork adds **MIDI-over-SCSI** support for communicating with vintage samplers (e.g., Akai S3000XL) that use SCSI as a MIDI transport. The changes add a new device type, initiator mode capability, and a streaming server for real-time MIDI events.

**Total changes:** ~3,935 lines added across 35 files (28 commits)

---

## Change Categories

### 1. New Files (Additive - 978 lines)

| File | Lines | Purpose |
|------|-------|---------|
| `cpp/devices/midi_processor.cpp` | 348 | SCSI device emulating Akai MIDI-over-SCSI protocol |
| `cpp/devices/midi_processor.h` | 103 | Header for MIDI processor device |
| `cpp/s2p/midi_streaming_server.cpp` | 443 | HTTP streaming server (SSE) for real-time MIDI |
| `cpp/s2p/midi_streaming_server.h` | 84 | Header for streaming server |

### 2. Test/Debug Scripts (Can Be Dropped - ~1,668 lines)

| File | Purpose |
|------|---------|
| `scripts/midi-test.py` | Python MIDI testing |
| `scripts/scsi-midi-test.py` | SCSI-MIDI integration tests |
| `scripts/scsi-*.ts` | TypeScript test scripts |
| `scripts/test-scmp.sh` | Shell test script |

**Recommendation:** These are development/debug scripts. Do not submit upstream.

### 3. Modified Core Files

#### Minimal Impact (additive, low risk)

| File | Changes | Description |
|------|---------|-------------|
| `cpp/base/device_factory.cpp` | +8 | Register SCMP device type (guarded by `BUILD_SCMP`) |
| `cpp/base/device_factory.h` | +1 | Add "midi" to device mapping |
| `cpp/base/primary_device.h` | +1 | Add SCMP to device type mapping |
| `cpp/pi/rpi_bus.h` | +4 | Declare override methods |
| `cpp/s2p/s2p_core.h` | +7 | Add streaming server member (guarded) |
| `api/s2p_interface.proto` | +50 | New device type, operations, messages |

#### Moderate Impact (interface extensions)

| File | Changes | Description |
|------|---------|-------------|
| `cpp/buses/bus.h` | +16 | Virtual methods for initiator mode (with default no-ops) |
| `cpp/command/command_dispatcher.h` | +78 | New members and methods for MIDI/SCSI execution |
| `cpp/s2p/s2p_core.cpp` | +59 | Main loop integration (guarded by `BUILD_SCMP`) |

#### Significant Impact (requires careful review)

| File | Changes | Concern |
|------|---------|---------|
| `cpp/command/command_dispatcher.cpp` | +470 | Constructor signature change: now takes `Bus&` reference |
| `cpp/controllers/controller.cpp` | +12 | Special-cases vendor command 0x0D for SCMP - breaks encapsulation |
| `cpp/pi/rpi_bus.cpp` | +93 | Changes `WaitForSelection` from infinite to 100ms timeout; adds debug logging |

---

## Impact Assessment

### Constructor Signature Change (Breaking)

```cpp
// Before
CommandDispatcher(CommandExecutor&, ControllerFactory&, Logger&)

// After
CommandDispatcher(CommandExecutor&, ControllerFactory&, Bus&, Logger&)
```

**Impact:** All code instantiating `CommandDispatcher` must be updated.

### WaitForSelection Timeout Change

```cpp
// Before: infinite wait
epoll_wait(epoll_fd, &epev, 1, -1)

// After: 100ms timeout
epoll_wait(epoll_fd, &epev, 1, 100)
```

**Impact:** Changes timing behavior for all users. May affect power consumption or CPU usage on idle systems.

### Controller Vendor Command Hack

```cpp
// In controller.cpp Status()
if (opcode == ScsiCommand::SET_MCAST_ADDR) {
    const auto device = GetDeviceForLun(GetEffectiveLun());
    if (device && device->GetType() == SCMP) {
        BusFree();  // Skip STATUS/MESSAGE IN phases
        return;
    }
}
```

**Impact:** Breaks SCSI protocol abstraction. Device-specific behavior leaks into generic controller.

### Debug Logging in rpi_bus.cpp

```cpp
fprintf(stderr, "[bus] SEL event: DAT=0x%02x SEL=%d BSY=%d IO=%d\n", ...);
fprintf(stderr, "[bus] GetSelection: 0x%02x\n", selection);
```

**Impact:** Noisy output for all users. Must be removed or made conditional.

---

## Recommendations for Upstream Submission

### 1. Split into Multiple PRs

**PR 1: Bus Interface Extensions (Low Risk)**
- Add `SetInitiatorMode()`, `SuspendSelectionEvent()`, `ResumeSelectionEvent()` to `bus.h`
- Virtual with default no-op implementations
- No behavioral change to existing code

**PR 2: Initiator Mode for rpi_bus (Moderate Risk)**
- Implement the three methods in `rpi_bus.cpp`
- Requires: PR 1
- Does not change target-mode behavior

**PR 3: SCSI_EXEC Generic Command (Moderate Risk)**
- Add `SCSI_EXEC` operation to protobuf API
- Implement in command_dispatcher
- General-purpose initiator command execution
- Requires: PR 1, PR 2

**PR 4: SCMP Device Type (Largest, Depends on PR 1-3)**
- Add `MidiProcessor` device
- Add `midi_streaming_server`
- MIDI-specific protobuf operations
- S2p core integration

### 2. Reduce Invasiveness

| Issue | Current State | Recommended Fix |
|-------|---------------|-----------------|
| Constructor signature | Breaking change | Add optional `Bus*` parameter with nullptr default |
| WaitForSelection timeout | Always 100ms | Make timeout configurable (default infinite) |
| Controller 0x0D hack | Device-specific in generic code | Add `SkipStatusPhase()` virtual to `PrimaryDevice` |
| Debug fprintf | Always prints | Remove, or use logger with debug level |

### 3. Code to NOT Submit

- All files in `scripts/` (test/debug scripts)
- Debug `fprintf` statements in `rpi_bus.cpp`
- `Dockerfile` additions (unless useful to upstream)

### 4. Clean Up Before Submission

1. **Remove debug logging:**
   ```cpp
   fprintf(stderr, "[bus] SEL event: ...");  // Remove these
   ```

2. **Make timeout configurable:**
   ```cpp
   // Add to bus.h
   virtual int GetSelectionTimeoutMs() const { return -1; }  // -1 = infinite

   // In rpi_bus.cpp
   epoll_wait(epoll_fd, &epev, 1, GetSelectionTimeoutMs());
   ```

3. **Fix controller encapsulation:**
   ```cpp
   // In primary_device.h
   virtual bool SkipStatusPhaseAfterCommand(ScsiCommand) const { return false; }

   // In midi_processor.cpp
   bool SkipStatusPhaseAfterCommand(ScsiCommand cmd) const override {
       return cmd == ScsiCommand::SET_MCAST_ADDR;
   }
   ```

4. **Optional bus reference:**
   ```cpp
   CommandDispatcher(CommandExecutor&, ControllerFactory&, Logger&, Bus* = nullptr)
   ```

---

## PR Strategy

| PR | Scope | Risk | Dependencies |
|----|-------|------|--------------|
| 1 | Bus interface extensions | Low | None |
| 2 | rpi_bus initiator mode | Moderate | PR 1 |
| 3 | SCSI_EXEC operation | Moderate | PR 1, PR 2 |
| 4 | SCMP device + streaming | Larger | PR 1-3 |

Submit PRs 1-3 first. They provide general-purpose initiator mode capability that may be useful for other projects. PR 4 is the Akai-specific implementation.

---

## Files Summary

### Submit Upstream (with cleanup)

```
api/s2p_interface.proto          (+50 lines, new operations/messages)
cpp/base/device_factory.cpp      (+8 lines, device registration)
cpp/base/device_factory.h        (+1 line, device mapping)
cpp/base/primary_device.h        (+1 line, device type)
cpp/buses/bus.h                  (+16 lines, virtual interface)
cpp/command/command_dispatcher.cpp (+470 lines, MIDI/SCSI execution)
cpp/command/command_dispatcher.h (+78 lines, new methods)
cpp/controllers/controller.cpp   (+12 lines, needs refactor)
cpp/devices/midi_processor.cpp   (+348 lines, new device)
cpp/devices/midi_processor.h     (+103 lines, new device header)
cpp/pi/rpi_bus.cpp               (+93 lines, needs debug removal)
cpp/pi/rpi_bus.h                 (+4 lines, method declarations)
cpp/s2p/midi_streaming_server.cpp (+443 lines, new server)
cpp/s2p/midi_streaming_server.h  (+84 lines, new server header)
cpp/s2p/s2p_core.cpp             (+59 lines, integration)
cpp/s2p/s2p_core.h               (+7 lines, streaming server member)
cpp/Makefile                     (+21 lines, BUILD_SCMP flag)
```

### Do Not Submit

```
scripts/*                        (test/debug scripts)
Dockerfile                       (unless useful to upstream)
.gitignore additions             (review individually)
```
