//---------------------------------------------------------------------------
//
// SCSI2Pi, SCSI device emulator and SCSI tools for the Raspberry Pi
//
// Copyright (C) 2022-2025 Uwe Seimet
//
//---------------------------------------------------------------------------

#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <spdlog/spdlog.h>
#include "shared/s2p_defs.h"
#include "generated/s2p_interface.pb.h"

class Bus;
class CommandContext;
class CommandExecutor;
class ControllerFactory;

using namespace spdlog;
using namespace s2p_interface;

class CommandDispatcher final
{

public:

    CommandDispatcher(CommandExecutor &e, ControllerFactory &f, Bus &b, logger &l)
    : executor(e), controller_factory(f), bus(b), s2p_logger(l)
    {
    }
    ~CommandDispatcher() = default;

    bool DispatchCommand(const CommandContext&, PbResult&);

    bool ShutDown(ShutdownMode) const;

    bool SetLogLevel(const string&);

    bool SetWithoutTypes(const string&);

private:

    bool HandleDeviceListChange(const CommandContext&) const;
    bool ShutDown(const CommandContext&) const;

    bool ExecuteMidi(const CommandContext&, PbResult&);
    bool ExecuteScsi(const CommandContext&, PbResult&);

public:

    // SCSI generic command queue for main loop execution
    struct ScsiCommand {
        int target_id = 0;
        int target_lun = 0;
        vector<uint8_t> cdb;
        vector<uint8_t> data_out;
        int expected_data_in = 0;
        int timeout_seconds = 3;

        // Results
        bool completed = false;
        bool success = false;
        int status = -1;
        vector<uint8_t> sense_data;
        vector<uint8_t> data_in;
        int bytes_transferred = 0;

        mutex mtx;
        condition_variable cv;
    };

    // Queue a generic SCSI command and wait for the main loop to execute it
    shared_ptr<ScsiCommand> QueueScsiCommand(int target_id, int target_lun,
        const vector<uint8_t> &cdb, const vector<uint8_t> &data_out,
        int expected_data_in, int timeout);

    // Execute pending SCSI commands (called from main loop when bus is free)
    void ProcessScsiQueue();

    // MIDI command queue for main loop execution
    struct MidiCommand {
        PbOperation operation;
        int target_id;
        vector<uint8_t> sysex_data;
        int read_length;
        // Result — set by main loop
        bool completed = false;
        bool success = false;
        vector<uint8_t> response_data;
        int pending_bytes = 0;
        mutex mtx;
        condition_variable cv;
    };

    // Queue a MIDI command and wait for the main loop to execute it
    shared_ptr<MidiCommand> QueueMidiCommand(PbOperation op, int target_id,
        const vector<uint8_t> &sysex = {}, int read_length = 0);

    // Execute pending MIDI commands (called from main loop when bus is free)
    void ProcessMidiQueue();

private:

    deque<shared_ptr<ScsiCommand>> scsi_queue;
    mutex scsi_queue_mutex;

    vector<shared_ptr<MidiCommand>> midi_queue;
    mutex midi_queue_mutex;

    CommandExecutor &executor;

    ControllerFactory &controller_factory;

    Bus &bus;

    logger &s2p_logger;

    unordered_set<PbDeviceType> without_types;
};
