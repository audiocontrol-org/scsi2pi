//---------------------------------------------------------------------------
//
// SCSI2Pi, SCSI device emulator and SCSI tools for the Raspberry Pi
//
// MIDI Processor device for MIDI-via-SCSI communication
// (e.g. Akai S3000XL sampler)
//
// Copyright (C) 2026 AudioControl
//
// Protocol (Akai MIDI-via-SCSI):
//   0x0C (SET_IFACE_MODE):  Initiator sends MIDI SysEx to target (DATA OUT)
//   0x0D (SET_MCAST_ADDR):  Initiator polls target for pending bytes (DATA IN, 3 bytes)
//                           Response: 00 HH LL where HHLL = queued byte count
//   0x09 (RETRIEVE_STATS):  Initiator reads pending SysEx from target (DATA IN)
//
//---------------------------------------------------------------------------

#pragma once

#include <vector>
#include "base/primary_device.h"

class MidiProcessor final : public PrimaryDevice
{

public:

    explicit MidiProcessor(int);

    string SetUp() override;
    void CleanUp() override;

    string GetIdentifier() const override
    {
        return "MIDI Processor";
    }

    param_map GetDefaultParams() const override
    {
        return { { SOCKET_PATH_PARAM, DEFAULT_SOCKET_PATH } };
    }

    vector<uint8_t> HandleInquiry() const override;
    int WriteData(cdb_t, data_out_t, int) override;

    // SCSI command handlers
    void RetrieveStats();      // 0x09 — read queued SysEx response
    void SetInterfaceMode();   // 0x0C — receive MIDI SysEx from initiator (not const: modifies buffer)
    void SendData();           // 0x0D — poll: return pending byte count (not const: reads buffer)
    void EnableInterface() const; // 0x0E — accept with GOOD

    vector<PbStatistics> GetStatistics() const override;

    // Initiator command queue — commands to send TO the S3000XL
    struct InitiatorCommand {
        int target_id;
        vector<uint8_t> cdb;
        vector<uint8_t> data;
    };

    bool HasPendingInitiatorCommands() const { return !initiator_queue.empty(); }
    InitiatorCommand PopInitiatorCommand();

private:

    vector<uint8_t> response_buffer;
    vector<InitiatorCommand> initiator_queue;

    void QueueSdsAck(uint8_t channel, uint8_t packet_number);
    void QueueSendToTarget(int target_id, const vector<uint8_t> &sysex);
    void DrainSocket();

    bool ConnectSocket();
    void DisconnectSocket();
    int SocketRead(span<uint8_t> buf);
    int SocketWrite(span<const uint8_t> buf);

    int sock_fd = -1;
    string socket_path;

    uint64_t byte_read_count = 0;
    uint64_t byte_write_count = 0;

    static constexpr const char *SOCKET_PATH_PARAM = "socket_path";
    static constexpr const char *DEFAULT_SOCKET_PATH = "/tmp/scsi-midi-bridge.sock";

    static constexpr const char *BYTE_READ_COUNT = "byte_read_count";
    static constexpr const char *BYTE_WRITE_COUNT = "byte_write_count";
};
