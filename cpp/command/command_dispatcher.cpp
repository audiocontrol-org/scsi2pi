//---------------------------------------------------------------------------
//
// SCSI2Pi, SCSI device emulator and SCSI tools for the Raspberry Pi
//
// Copyright (C) 2022-2026 Uwe Seimet
//
//---------------------------------------------------------------------------

#include "command_dispatcher.h"
#include <chrono>
#include <fstream>
#include <unistd.h>
#include "buses/bus.h"
#include "command_context.h"
#include "command_executor.h"
#include "command_image_support.h"
#include "command_response.h"
#include "base/primary_device.h"
#include "devices/storage_device.h"
#include "devices/disk.h"
#include "devices/cache.h"
#include "controllers/controller_factory.h"
#include "initiator/initiator_executor.h"
#include "protobuf/s2p_interface_util.h"
#include "base/property_handler.h"
#include "shared/s2p_exceptions.h"

using namespace command_response;
using namespace s2p_interface_util;
using namespace s2p_util;

bool CommandDispatcher::DispatchCommand(const CommandContext &context, PbResult &result)
{
    const PbCommand &command = context.GetCommand();
    const PbOperation operation = command.operation();

    if (!PbOperation_IsValid(operation)) {
        s2p_logger.trace("Ignored unknown command with operation opcode {}", static_cast<int>(operation));

        return context.ReturnLocalizedError(LocalizationKey::ERROR_OPERATION, UNKNOWN_OPERATION,
            to_string(static_cast<int>(operation)));
    }

    s2p_logger.trace("Executing {} command", PbOperation_Name(operation));

    switch (operation) {
    case LOG_LEVEL:
        if (const string &log_level = GetParam(command, "level"); !SetLogLevel(log_level)) {
            return context.ReturnLocalizedError(LocalizationKey::ERROR_LOG_LEVEL, log_level);
        }
        else {
            PropertyHandler::GetInstance().AddProperty(PropertyHandler::LOG_LEVEL, log_level);
            return context.ReturnSuccessStatus();
        }

    case DEFAULT_FOLDER: {
        const string &folder = GetParam(command, "folder");
        if (const string &error = CommandImageSupport::GetInstance().SetDefaultFolder(folder); !error.empty()) {
            result.set_msg(error);
            return context.WriteResult(result);
        }
        else {
            s2p_logger.info("Default image folder set to '{}'", folder);
            PropertyHandler::GetInstance().AddProperty(PropertyHandler::IMAGE_FOLDER, folder);
            return context.WriteSuccessResult(result);
        }
    }

    case DEVICES_INFO:
        GetDevicesInfo(controller_factory.GetAllDevices(), result, command);
        return context.WriteSuccessResult(result);

    case DEVICE_TYPES_INFO:
        GetDeviceTypesInfo(*result.mutable_device_types_info(), without_types);
        return context.WriteSuccessResult(result);

    case SERVER_INFO:
        GetServerInfo(*result.mutable_server_info(), command, controller_factory.GetAllDevices(),
            executor.GetReservedIds(), without_types, s2p_logger);
        return context.WriteSuccessResult(result);

    case VERSION_INFO:
        GetVersionInfo(*result.mutable_version_info());
        return context.WriteSuccessResult(result);

    case LOG_LEVEL_INFO:
        GetLogLevelInfo(*result.mutable_log_level_info());
        return context.WriteSuccessResult(result);

    case DEFAULT_IMAGE_FILES_INFO:
        GetImageFilesInfo(*result.mutable_image_files_info(), GetParam(command, "folder_pattern"),
            GetParam(command, "file_pattern"), s2p_logger);
        return context.WriteSuccessResult(result);

    case IMAGE_FILE_INFO:
        if (const string &filename = GetParam(command, "file"); filename.empty()) {
            return context.ReturnLocalizedError(LocalizationKey::ERROR_MISSING_FILENAME);
        }
        else {
            if (const auto &image_file = make_unique<PbImageFile>(); GetImageFile(*image_file.get(), filename)) {
                result.set_allocated_image_file_info(image_file.get());
                result.set_status(true);
                return context.WriteResult(result);
            }
            else {
                return context.ReturnLocalizedError(LocalizationKey::ERROR_IMAGE_FILE_INFO, filename);
            }
        }
        break;

    case NETWORK_INTERFACES_INFO:
        GetNetworkInterfacesInfo(*result.mutable_network_interfaces_info());
        return context.WriteSuccessResult(result);

    case MAPPING_INFO:
        GetMappingInfo(*result.mutable_mapping_info());
        return context.WriteSuccessResult(result);

    case STATISTICS_INFO:
        GetStatisticsInfo(*result.mutable_statistics_info(), controller_factory.GetAllDevices());
        return context.WriteSuccessResult(result);

    case PROPERTIES_INFO:
        GetPropertiesInfo(*result.mutable_properties_info());
        return context.WriteSuccessResult(result);

    case OPERATION_INFO:
        GetOperationInfo(*result.mutable_operation_info());
        return context.WriteSuccessResult(result);

    case RESERVED_IDS_INFO:
        GetReservedIds(*result.mutable_reserved_ids_info(), executor.GetReservedIds());
        return context.WriteSuccessResult(result);

    case SHUT_DOWN:
        return ShutDown(context);

    case CREATE_IMAGE:
        return CommandImageSupport::GetInstance().CreateImage(context);

    case DELETE_IMAGE:
        return CommandImageSupport::GetInstance().DeleteImage(context);

    case RENAME_IMAGE:
        return CommandImageSupport::GetInstance().RenameImage(context);

    case COPY_IMAGE:
        return CommandImageSupport::GetInstance().CopyImage(context);

    case PROTECT_IMAGE:
    case UNPROTECT_IMAGE:
        return CommandImageSupport::GetInstance().SetImagePermissions(context);

    case PERSIST_CONFIGURATION:
        return PropertyHandler::GetInstance().Persist() ?
                context.ReturnSuccessStatus() : context.ReturnLocalizedError(LocalizationKey::ERROR_PERSIST);

    case MIDI_INIT:
    case MIDI_SEND:
    case MIDI_POLL:
    case MIDI_READ:
        return ExecuteMidi(context, result);

    case SCSI_EXEC:
        return ExecuteScsi(context, result);

    case NO_OPERATION:
        return context.ReturnSuccessStatus();

    default:
        // The remaining commands may only be executed when the target is idle, which is ensured by the lock
        return executor.ProcessCmd(context) ? HandleDeviceListChange(context) : false;
    }

    return true;
}

bool CommandDispatcher::HandleDeviceListChange(const CommandContext &context) const
{
    // ATTACH, DETACH, INSERT and EJECT return the resulting device list
    if (const PbOperation operation = context.GetCommand().operation(); operation == ATTACH || operation == DETACH
        || operation == INSERT || operation == EJECT) {
        // A command with an empty device list is required here in order to return data for all devices
        PbCommand command;
        PbResult result;
        GetDevicesInfo(controller_factory.GetAllDevices(), result, command);
        return context.WriteResult(result);
    }

    return true;
}

// Shutdown on a remote interface command
bool CommandDispatcher::ShutDown(const CommandContext &context) const
{
    ShutdownMode mode = ShutdownMode::NONE;

    if (const string &m = GetParam(context.GetCommand(), "mode"); m == "rascsi") {
        mode = ShutdownMode::STOP_S2P;
    }
    else if (m == "system") {
        mode = ShutdownMode::STOP_PI;
    }
    else if (m == "reboot") {
        mode = ShutdownMode::RESTART_PI;
    }
    else {
        return context.ReturnLocalizedError(LocalizationKey::ERROR_SHUTDOWN_MODE_INVALID, m);
    }

    // Shutdown modes other than "rascsi" require root permissions
    if (mode != ShutdownMode::STOP_S2P && getuid()) {
        return context.ReturnLocalizedError(LocalizationKey::ERROR_SHUTDOWN_PERMISSION);
    }

    // Report success now because after a shutdown nothing can be reported anymore
    PbResult result;
    context.WriteSuccessResult(result);

    return ShutDown(mode);
}

// Shutdown on a SCSI command
bool CommandDispatcher::ShutDown(ShutdownMode mode) const
{
    switch (mode) {
    case ShutdownMode::STOP_S2P:
        s2p_logger.info("s2p shutdown requested");
        return true;

    case ShutdownMode::STOP_PI:
        s2p_logger.info("Pi shutdown requested");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
        system("init 0");
#pragma GCC diagnostic pop
        s2p_logger.error("Pi shutdown failed");
        break;

    case ShutdownMode::RESTART_PI:
        s2p_logger.info("Pi restart requested");
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
        system("init 6");
#pragma GCC diagnostic pop
        s2p_logger.error("Pi restart failed");
        break;

    default:
        s2p_logger.error("Invalid shutdown mode {}", static_cast<int>(mode));
        break;
    }

    return false;
}

bool CommandDispatcher::SetLogLevel(const string &log_level)
{
    int id = -1;
    int lun = -1;
    string level = log_level;

    if (const auto &components = Split(log_level, COMPONENT_SEPARATOR, 2); !components.empty()) {
        level = components[0];

        if (components.size() > 1) {
            if (const string &error = ParseIdAndLun(components[1], id, lun); !error.empty()) {
                s2p_logger.warn("Error setting log level: {}", error);
                return false;
            }
        }
    }

    const level::level_enum l = level::from_str(level);
    // Compensate for spdlog using 'off' for unknown levels
    if (to_string_view(l) != level) {
        s2p_logger.warn("Invalid log level '{}'", level);
        return false;
    }

    s2p_logger.set_level(l);
    controller_factory.SetLogLevel(id, lun, l);

    if (id != -1) {
        if (lun == -1) {
            s2p_logger.info("Set log level for device {} to '{}'", id, level);
        }
        else {
            s2p_logger.info("Set log level for device {}:{} to '{}'", id, lun, level);
        }
    }
    else {
        s2p_logger.info("Set log level to '{}'", level);
    }

    return true;
}

bool CommandDispatcher::SetWithoutTypes(const string &types)
{
    return ranges::all_of(Split(types, ','), [this](const auto &t) {
        if(const auto type = ParseDeviceType(Trim(t)); type != UNDEFINED) {
            without_types.emplace(type);
            return true;
        }

        return false;
    }
    );
}

bool CommandDispatcher::ExecuteMidi(const CommandContext &context, PbResult &result)
{
    const PbCommand &command = context.GetCommand();
    const PbOperation operation = command.operation();
    const PbMidiRequest &midi_request = command.midi_request();
    const int target_id = midi_request.target_id();

    if (target_id < 0 || target_id > 7) {
        return context.ReturnErrorStatus("Invalid SCSI target ID: " + to_string(target_id));
    }

    // Queue the command for main loop execution (the main loop has bus access)
    vector<uint8_t> sysex;
    if (operation == MIDI_SEND) {
        const string &data = midi_request.sysex_data();
        sysex.assign(data.begin(), data.end());
    }

    auto cmd = QueueMidiCommand(operation, target_id, sysex, midi_request.read_length());

    // Wait for the main loop to execute it (up to 5 seconds)
    {
        unique_lock lock(cmd->mtx);
        if (!cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return cmd->completed; })) {
            return context.ReturnErrorStatus("MIDI command timed out waiting for bus access");
        }
    }

    if (!cmd->success) {
        return context.ReturnErrorStatus("MIDI command failed");
    }

    auto *midi_response = result.mutable_midi_response();
    midi_response->set_data(cmd->response_data.data(), cmd->response_data.size());
    midi_response->set_pending_bytes(cmd->pending_bytes);
    return context.WriteSuccessResult(result);
}

shared_ptr<CommandDispatcher::MidiCommand> CommandDispatcher::QueueMidiCommand(
    PbOperation op, int target_id, const vector<uint8_t> &sysex, int read_length)
{
    auto cmd = make_shared<MidiCommand>();
    cmd->operation = op;
    cmd->target_id = target_id;
    cmd->sysex_data = sysex;
    cmd->read_length = read_length;

    {
        lock_guard lock(midi_queue_mutex);
        midi_queue.push_back(cmd);
    }

    return cmd;
}

void CommandDispatcher::ProcessMidiQueue()
{
    shared_ptr<MidiCommand> cmd;

    {
        lock_guard lock(midi_queue_mutex);
        if (midi_queue.empty()) return;
        cmd = midi_queue.front();
        midi_queue.erase(midi_queue.begin());
    }

    // Suspend SEL event monitoring — the kernel gpioevent handler holds PIN_SEL
    // as INPUT, which conflicts with asserting SEL during initiator selection
    bus.SuspendSelectionEvent();

    // Switch bus to initiator mode with settling delay
    bus.SetInitiatorMode(true);
    usleep(10000);  // 10ms settle time

    // Execute on the main thread where the bus is free
    InitiatorExecutor initiator(bus, 7, s2p_logger);
    initiator.SetTarget(cmd->target_id, 0, false);

    int status = 0;
    switch (cmd->operation) {
    case MIDI_INIT: {
        s2p_logger.info("MIDI_INIT: target {}", cmd->target_id);
        vector<uint8_t> cdb = { 0x09, 0x00, 0x01, 0x01, 0x00, 0x00 };
        vector<uint8_t> buffer(256);
        status = initiator.Execute(cdb, buffer, 0, 3, false);
        break;
    }
    case MIDI_SEND: {
        s2p_logger.info("MIDI_SEND: {} byte(s) to target {}", cmd->sysex_data.size(), cmd->target_id);
        // CDB 0x0C (SEND 6): bytes 2-4 are the 3-byte transfer length (MSB first)
        const auto len = static_cast<int>(cmd->sysex_data.size());
        vector<uint8_t> cdb = { 0x0c, 0x00,
            static_cast<uint8_t>((len >> 16) & 0xff),
            static_cast<uint8_t>((len >> 8) & 0xff),
            static_cast<uint8_t>(len & 0xff),
            0x00 };
        status = initiator.Execute(cdb, cmd->sysex_data, len, 3, false);
        break;
    }
    case MIDI_POLL: {
        vector<uint8_t> cdb = { 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00 };
        vector<uint8_t> buffer(4);
        status = initiator.Execute(cdb, buffer, 3, 3, false);
        if (status == 0) {
            cmd->pending_bytes = (buffer[1] << 8) | buffer[2];
            cmd->response_data.assign(buffer.begin(), buffer.begin() + 3);
            s2p_logger.info("MIDI_POLL: {} pending byte(s)", cmd->pending_bytes);
        }
        break;
    }
    case MIDI_READ: {
        const int rlen = cmd->read_length;
        s2p_logger.info("MIDI_READ: {} byte(s) from target {}", rlen, cmd->target_id);
        // CDB 0x0E: bytes 2-4 are the 3-byte transfer length (MSB first)
        vector<uint8_t> cdb = { 0x0e, 0x00,
            static_cast<uint8_t>((rlen >> 16) & 0xff),
            static_cast<uint8_t>((rlen >> 8) & 0xff),
            static_cast<uint8_t>(rlen & 0xff),
            0x00 };
        vector<uint8_t> buffer(rlen);
        status = initiator.Execute(cdb, buffer, rlen, 3, false);
        if (status == 0) {
            const int byte_count = initiator.GetByteCount();
            cmd->response_data.assign(buffer.begin(), buffer.begin() + byte_count);
            s2p_logger.info("MIDI_READ: received {} byte(s)", byte_count);
        }
        break;
    }
    default:
        break;
    }

    // Switch bus back to target mode
    bus.SetInitiatorMode(false);

    // Resume SEL event monitoring for target mode
    bus.ResumeSelectionEvent();

    // Signal completion
    {
        lock_guard lock(cmd->mtx);
        cmd->success = (status == 0);
        cmd->completed = true;
    }
    cmd->cv.notify_one();
}

bool CommandDispatcher::ExecuteScsi(const CommandContext &context, PbResult &result)
{
    const PbCommand &command = context.GetCommand();
    const PbScsiRequest &scsi_request = command.scsi_request();
    const int target_id = scsi_request.target_id();
    const int target_lun = scsi_request.target_lun();

    if (target_id < 0 || target_id > 7) {
        return context.ReturnErrorStatus("Invalid SCSI target ID: " + to_string(target_id));
    }

    if (target_lun < 0 || target_lun > 31) {
        return context.ReturnErrorStatus("Invalid SCSI LUN: " + to_string(target_lun));
    }

    const string &cdb_str = scsi_request.cdb();
    if (cdb_str.empty()) {
        return context.ReturnErrorStatus("CDB must not be empty");
    }

    vector<uint8_t> cdb(cdb_str.begin(), cdb_str.end());
    vector<uint8_t> data_out(scsi_request.data_out().begin(), scsi_request.data_out().end());
    const int expected_data_in = scsi_request.expected_data_in();
    int timeout = scsi_request.timeout_seconds();
    if (timeout <= 0) {
        timeout = 3;
    }

    s2p_logger.info("SCSI_EXEC: CDB {} byte(s) to target {}:{}", cdb.size(), target_id, target_lun);

    auto cmd = QueueScsiCommand(target_id, target_lun, cdb, data_out, expected_data_in, timeout);

    // Wait for the main loop to execute it (timeout + 5 seconds grace)
    {
        unique_lock lock(cmd->mtx);
        if (!cmd->cv.wait_for(lock, chrono::seconds(timeout + 5), [&] { return cmd->completed; })) {
            return context.ReturnErrorStatus("SCSI command timed out waiting for bus access");
        }
    }

    auto *scsi_response = result.mutable_scsi_response();
    scsi_response->set_status(cmd->status);
    scsi_response->set_sense_data(cmd->sense_data.data(), cmd->sense_data.size());
    scsi_response->set_data_in(cmd->data_in.data(), cmd->data_in.size());
    scsi_response->set_bytes_transferred(cmd->bytes_transferred);

    if (!cmd->success) {
        result.set_status(false);
        result.set_msg("SCSI command failed with status " + to_string(cmd->status));
        return context.WriteResult(result);
    }

    return context.WriteSuccessResult(result);
}

shared_ptr<CommandDispatcher::ScsiCommand> CommandDispatcher::QueueScsiCommand(
    int target_id, int target_lun, const vector<uint8_t> &cdb,
    const vector<uint8_t> &data_out, int expected_data_in, int timeout)
{
    auto cmd = make_shared<ScsiCommand>();
    cmd->target_id = target_id;
    cmd->target_lun = target_lun;
    cmd->cdb = cdb;
    cmd->data_out = data_out;
    cmd->expected_data_in = expected_data_in;
    cmd->timeout_seconds = timeout;

    {
        lock_guard lock(scsi_queue_mutex);
        scsi_queue.push_back(cmd);
    }

    return cmd;
}

void CommandDispatcher::ProcessScsiQueue()
{
    shared_ptr<ScsiCommand> cmd;

    {
        lock_guard lock(scsi_queue_mutex);
        if (scsi_queue.empty()) return;
        cmd = scsi_queue.front();
        scsi_queue.pop_front();
    }

    // Check if target is an emulated device
    auto device = controller_factory.GetDeviceForIdAndLun(cmd->target_id, cmd->target_lun);
    if (device) {
        // Route to emulated device instead of physical bus
        ProcessScsiQueueEmulated(cmd, device);
    } else {
        // Physical bus target
        ProcessScsiQueuePhysical(cmd);
    }

    // Signal completion
    {
        lock_guard lock(cmd->mtx);
        cmd->completed = true;
    }
    cmd->cv.notify_one();
}

void CommandDispatcher::ProcessScsiQueueEmulated(shared_ptr<ScsiCommand> cmd,
    shared_ptr<PrimaryDevice> device)
{
    if (cmd->cdb.empty()) {
        cmd->status = -1;
        cmd->success = false;
        return;
    }

    const uint8_t opcode = cmd->cdb[0];
    s2p_logger.info("SCSI_EXEC emulated: target {}:{} CDB {:02x}", cmd->target_id, cmd->target_lun, opcode);

    switch (opcode) {
    case 0x00: // TEST UNIT READY
        cmd->status = 0;
        cmd->success = true;
        break;

    case 0x12: { // INQUIRY
        // Build standard INQUIRY response from device metadata
        vector<uint8_t> inquiry_data(36, 0);
        // Byte 0: device type (from PbDeviceType mapping)
        switch (device->GetType()) {
            case SCHD: case SAHD: inquiry_data[0] = 0x00; break; // Direct access (disk)
            case SCCD: inquiry_data[0] = 0x05; break; // CD-ROM
            case SCMO: inquiry_data[0] = 0x07; break; // Optical
            case SCTP: inquiry_data[0] = 0x01; break; // Sequential (tape)
            default: inquiry_data[0] = 0x00; break;
        }
        inquiry_data[1] = 0x00; // Not removable
        inquiry_data[2] = 0x02; // SCSI-2
        inquiry_data[3] = 0x02; // Response format 2
        inquiry_data[4] = 31;   // Additional length
        inquiry_data[5] = 0x00;
        inquiry_data[6] = 0x00;
        inquiry_data[7] = 0x00;
        // Bytes 8-35: vendor(8) + product(16) + revision(4)
        auto name = device->GetPaddedName();
        for (int j = 0; j < 28 && j < static_cast<int>(name.size()); j++) {
            inquiry_data[8 + j] = static_cast<uint8_t>(name[j]);
        }
        const int alloc_len = cmd->expected_data_in > 0 ? cmd->expected_data_in : 36;
        const int copy_len = min(static_cast<int>(inquiry_data.size()), alloc_len);
        cmd->data_in.assign(inquiry_data.begin(), inquiry_data.begin() + copy_len);
        cmd->bytes_transferred = copy_len;
        cmd->status = 0;
        cmd->success = true;
        break;
    }

    case 0x25: { // READ CAPACITY(10)
        auto storage = dynamic_pointer_cast<StorageDevice>(device);
        if (!storage || !storage->GetBlockCount()) {
            cmd->status = 2;
            cmd->success = false;
            break;
        }
        vector<uint8_t> resp(8, 0);
        uint32_t last_lba = static_cast<uint32_t>(
            min(storage->GetBlockCount() - 1, (uint64_t)0xFFFFFFFF));
        uint32_t block_size = storage->GetBlockSize();
        resp[0] = (last_lba >> 24) & 0xff;
        resp[1] = (last_lba >> 16) & 0xff;
        resp[2] = (last_lba >> 8) & 0xff;
        resp[3] = last_lba & 0xff;
        resp[4] = (block_size >> 24) & 0xff;
        resp[5] = (block_size >> 16) & 0xff;
        resp[6] = (block_size >> 8) & 0xff;
        resp[7] = block_size & 0xff;
        cmd->data_in = resp;
        cmd->bytes_transferred = 8;
        cmd->status = 0;
        cmd->success = true;
        s2p_logger.info("SCSI_EXEC emulated: READ CAPACITY last_lba={} block_size={}", last_lba, block_size);
        break;
    }

    case 0x28: case 0x08: { // READ(10), READ(6)
        auto disk = dynamic_pointer_cast<Disk>(device);
        if (!disk) { cmd->status = 2; cmd->success = false; break; }

        uint32_t lba;
        uint32_t count;
        if (opcode == 0x28) { // READ(10)
            lba = ((uint32_t)cmd->cdb[2] << 24) | ((uint32_t)cmd->cdb[3] << 16) |
                  ((uint32_t)cmd->cdb[4] << 8) | cmd->cdb[5];
            count = ((uint32_t)cmd->cdb[7] << 8) | cmd->cdb[8];
        } else { // READ(6)
            lba = ((uint32_t)(cmd->cdb[1] & 0x1f) << 16) |
                  ((uint32_t)cmd->cdb[2] << 8) | cmd->cdb[3];
            count = cmd->cdb[4] ? cmd->cdb[4] : 256;
        }

        uint32_t block_size = disk->GetBlockSize();
        uint32_t byte_count = count * block_size;
        vector<uint8_t> buffer(byte_count);

        // DiskCache only supports 1 sector at a time — loop
        bool read_ok = true;
        for (uint32_t i = 0; i < count; i++) {
            span<uint8_t> sec_span(buffer.data() + i * block_size, block_size);
            if (!disk->GetCache()->ReadSectors(sec_span, lba + i, 1)) {
                s2p_logger.error("SCSI_EXEC emulated: READ failed at LBA {}", lba + i);
                read_ok = false;
                break;
            }
        }
        if (read_ok) {
            cmd->data_in = buffer;
            cmd->bytes_transferred = byte_count;
            cmd->status = 0;
            cmd->success = true;
        } else {
            cmd->status = 2;
            cmd->success = false;
        }
        break;
    }

    case 0x2A: case 0x0A: { // WRITE(10), WRITE(6)
        auto disk = dynamic_pointer_cast<Disk>(device);
        if (!disk) { cmd->status = 2; cmd->success = false; break; }

        uint32_t lba;
        uint32_t count;
        if (opcode == 0x2A) { // WRITE(10)
            lba = ((uint32_t)cmd->cdb[2] << 24) | ((uint32_t)cmd->cdb[3] << 16) |
                  ((uint32_t)cmd->cdb[4] << 8) | cmd->cdb[5];
            count = ((uint32_t)cmd->cdb[7] << 8) | cmd->cdb[8];
        } else { // WRITE(6)
            lba = ((uint32_t)(cmd->cdb[1] & 0x1f) << 16) |
                  ((uint32_t)cmd->cdb[2] << 8) | cmd->cdb[3];
            count = cmd->cdb[4] ? cmd->cdb[4] : 256;
        }

        if (!cmd->data_out.empty()) {
            uint32_t block_size = disk->GetBlockSize();
            // DiskCache only supports 1 sector at a time — loop
            bool write_ok = true;
            for (uint32_t i = 0; i < count; i++) {
                span<const uint8_t> sec_span(cmd->data_out.data() + i * block_size, block_size);
                if (!disk->GetCache()->WriteSectors(sec_span, lba + i, 1)) {
                    s2p_logger.error("SCSI_EXEC emulated: WRITE failed at LBA {}", lba + i);
                    write_ok = false;
                    break;
                }
            }
            if (write_ok) {
                cmd->status = 0;
                cmd->success = true;
            } else {
                cmd->status = 2;
                cmd->success = false;
            }
        } else {
            cmd->status = 2;
            cmd->success = false;
        }
        break;
    }

    default:
        // For unsupported commands, return CHECK CONDITION
        s2p_logger.warn("SCSI_EXEC emulated: unsupported CDB {:02x} for target {}", opcode, cmd->target_id);
        cmd->status = 2; // CHECK CONDITION
        cmd->success = false;
        break;
    }
}

void CommandDispatcher::ProcessScsiQueuePhysical(shared_ptr<ScsiCommand> cmd)
{
    // Suspend SEL event monitoring — the kernel gpioevent handler holds PIN_SEL
    // as INPUT, which conflicts with asserting SEL during initiator selection
    bus.SuspendSelectionEvent();

    // Switch bus to initiator mode with settling delay
    bus.SetInitiatorMode(true);
    usleep(10000);  // 10ms settle time

    // Execute on the main thread where the bus is free
    InitiatorExecutor initiator(bus, 7, s2p_logger);
    initiator.SetTarget(cmd->target_id, cmd->target_lun, false);

    int status = -1;

    if (!cmd->data_out.empty()) {
        // DATA OUT phase (write to target)
        status = initiator.Execute(cmd->cdb, cmd->data_out,
            static_cast<int>(cmd->data_out.size()), cmd->timeout_seconds, false);
    } else if (cmd->expected_data_in > 0) {
        // DATA IN phase (read from target)
        vector<uint8_t> buffer(cmd->expected_data_in);
        status = initiator.Execute(cmd->cdb, buffer,
            cmd->expected_data_in, cmd->timeout_seconds, false);
        if (status == 0) {
            const int byte_count = initiator.GetByteCount();
            cmd->data_in.assign(buffer.begin(), buffer.begin() + byte_count);
            cmd->bytes_transferred = byte_count;
        }
    } else {
        // No data transfer
        vector<uint8_t> buffer;
        status = initiator.Execute(cmd->cdb, buffer, 0, cmd->timeout_seconds, false);
    }

    cmd->status = status;
    cmd->success = (status == 0);

    // Switch bus back to target mode
    bus.SetInitiatorMode(false);

    // Resume SEL event monitoring for target mode
    bus.ResumeSelectionEvent();
}
