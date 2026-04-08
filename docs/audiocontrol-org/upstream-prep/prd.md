# Upstream Contribution Preparation - Product Requirements Document

**Created:** 2026-04-07
**Status:** Draft
**Owner:** audiocontrol-org

## Problem Statement

The audiocontrol-org/scsi2pi fork adds MIDI-over-SCSI support (~3,935 lines across 35 files) with significant impact on core s2p code. The changes are tightly coupled: MIDI-specific logic is embedded in the command dispatcher, controller, and main loop. This makes upstream contribution difficult — the maintainer would need to accept domain-specific MIDI code to get general-purpose initiator mode support.

## User Stories

- As a **scsi2pi maintainer**, I want initiator mode contributions to be general-purpose so they benefit all users, not just MIDI use cases
- As a **scsi2pi user**, I want to execute arbitrary SCSI commands as an initiator so I can communicate with SCSI targets on the bus
- As an **audiocontrol developer**, I want to separate MIDI protocol logic from s2p core so we can iterate on MIDI features without forking s2p

## Success Criteria

- [ ] Upstream PR adds only general-purpose SCSI initiator mode (~250 lines)
- [ ] No MIDI-specific code in upstream PR (no SCMP device, no MidiStreamingServer)
- [ ] All MIDI functionality preserved in external scsi-midi-server
- [ ] External service communicates with s2p via existing protobuf socket using SCSI_EXEC
- [ ] Existing audiocontrol MIDI workflows continue to work end-to-end

## Scope

### In Scope

- Refactor s2p changes into minimal general-purpose initiator mode support
- Create SCSI_EXEC protobuf operation for generic initiator command execution
- Extract MIDI-specific code to external scsi-midi-server
- Port binary streaming protocol to external service
- End-to-end integration testing

### Out of Scope

- New MIDI features or protocol improvements
- Performance optimization of polling intervals
- Target-mode MIDI support (MidiProcessor device — not needed)
- Changes to the audiocontrol web editor MIDI client

## Dependencies

- Upstream scsi2pi must accept the initiator mode PR before we can simplify our fork
- scsi-midi-server requires s2p running with SCSI_EXEC support

## Open Questions

- [ ] Should SCSI_EXEC support emulated device routing, or only physical bus targets?
- [ ] Is 10ms polling latency (protobuf overhead) acceptable for all SDS transfer scenarios?
- [ ] Should the WaitForSelection timeout (100ms vs infinite) be configurable or hardcoded?
- [ ] Does the upstream maintainer prefer a single PR or a series of smaller PRs?
