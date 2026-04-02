//---------------------------------------------------------------------------
//
// SCSI2Pi, SCSI device emulator and SCSI tools for the Raspberry Pi
//
// MIDI Processor device for MIDI-via-SCSI communication
// (e.g. Akai S3000XL sampler)
//
// Copyright (C) 2026 AudioControl
//
//---------------------------------------------------------------------------

#pragma once

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

    void RetrieveStats();
    void SetInterfaceMode() const;
    void SendData() const;
    void EnableInterface() const;

    vector<PbStatistics> GetStatistics() const override;

private:

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
