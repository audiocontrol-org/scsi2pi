//---------------------------------------------------------------------------
//
// SCSI2Pi, SCSI device emulator and SCSI tools for the Raspberry Pi
//
// Copyright (C) 2022-2026 Uwe Seimet
//
//---------------------------------------------------------------------------

#include "command_dispatcher.h"
#include <fstream>
#include "buses/bus.h"
#include "command_context.h"
#include "command_executor.h"
#include "command_image_support.h"
#include "command_response.h"
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

    // Acquire the dispatch lock to ensure no target transaction is in progress
    scoped_lock lock(executor.GetDispatchLock());

    // Create an initiator executor with the bus, using board ID 7
    InitiatorExecutor initiator(bus, 7, s2p_logger);
    initiator.SetTarget(target_id, 0, false);

    switch (operation) {
    case MIDI_INIT: {
        s2p_logger.info("MIDI_INIT: Initializing MIDI-via-SCSI session with target {}", target_id);

        // CDB: 09:00:01:01:00:00
        vector<uint8_t> cdb = { 0x09, 0x00, 0x01, 0x01, 0x00, 0x00 };
        vector<uint8_t> buffer(256);
        const int status = initiator.Execute(cdb, buffer, 0, 3, false);

        if (status != 0) {
            return context.ReturnErrorStatus(
                fmt::format("MIDI_INIT failed with status {:#04x}", status));
        }

        auto *midi_response = result.mutable_midi_response();
        midi_response->set_data(buffer.data(), 0);
        return context.WriteSuccessResult(result);
    }

    case MIDI_SEND: {
        const string &sysex = midi_request.sysex_data();
        if (sysex.empty()) {
            return context.ReturnErrorStatus("MIDI_SEND requires sysex_data");
        }

        s2p_logger.info("MIDI_SEND: Sending {} byte(s) to target {}", sysex.size(), target_id);

        // CDB: 0C:00:00:00:LL:00 where LL = data length
        const auto data_len = static_cast<uint8_t>(sysex.size());
        vector<uint8_t> cdb = { 0x0c, 0x00, 0x00, 0x00, data_len, 0x00 };
        vector<uint8_t> data(sysex.begin(), sysex.end());
        const int status = initiator.Execute(cdb, data, static_cast<int>(data.size()), 3, false);

        if (status != 0) {
            return context.ReturnErrorStatus(
                fmt::format("MIDI_SEND failed with status {:#04x}", status));
        }

        auto *midi_response = result.mutable_midi_response();
        midi_response->set_data("", 0);
        return context.WriteSuccessResult(result);
    }

    case MIDI_POLL: {
        s2p_logger.info("MIDI_POLL: Polling target {} for pending bytes", target_id);

        // CDB: 0D:00:00:00:00:00, returns 3 bytes
        vector<uint8_t> cdb = { 0x0d, 0x00, 0x00, 0x00, 0x00, 0x00 };
        vector<uint8_t> buffer(4);
        const int status = initiator.Execute(cdb, buffer, 3, 3, false);

        if (status != 0) {
            return context.ReturnErrorStatus(
                fmt::format("MIDI_POLL failed with status {:#04x}", status));
        }

        // Parse: pending = (buffer[1] << 8) | buffer[2]
        const int pending = (buffer[1] << 8) | buffer[2];
        s2p_logger.info("MIDI_POLL: {} pending byte(s)", pending);

        auto *midi_response = result.mutable_midi_response();
        midi_response->set_pending_bytes(pending);
        midi_response->set_data(buffer.data(), 3);
        return context.WriteSuccessResult(result);
    }

    case MIDI_READ: {
        const int read_length = midi_request.read_length();
        if (read_length <= 0 || read_length > 65535) {
            return context.ReturnErrorStatus("MIDI_READ requires valid read_length (1-65535)");
        }

        s2p_logger.info("MIDI_READ: Reading {} byte(s) from target {}", read_length, target_id);

        // CDB: 0E:00:00:00:LL:00 where LL = read_length
        const auto len_byte = static_cast<uint8_t>(read_length);
        vector<uint8_t> cdb = { 0x0e, 0x00, 0x00, 0x00, len_byte, 0x00 };
        vector<uint8_t> buffer(read_length);
        const int status = initiator.Execute(cdb, buffer, read_length, 3, false);

        if (status != 0) {
            return context.ReturnErrorStatus(
                fmt::format("MIDI_READ failed with status {:#04x}", status));
        }

        const int byte_count = initiator.GetByteCount();
        s2p_logger.info("MIDI_READ: Received {} byte(s)", byte_count);

        auto *midi_response = result.mutable_midi_response();
        midi_response->set_data(buffer.data(), byte_count);
        return context.WriteSuccessResult(result);
    }

    default:
        return context.ReturnErrorStatus("Unknown MIDI operation");
    }
}
