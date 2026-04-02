//---------------------------------------------------------------------------
//
// SCSI2Pi, SCSI device emulator and SCSI tools for the Raspberry Pi
//
// MIDI Processor device for MIDI-via-SCSI communication
// (e.g. Akai S3000XL sampler)
//
// Copyright (C) 2026 AudioControl
//
// The Akai S3000XL uses SCSI processor device commands to send and receive
// MIDI SysEx data over SCSI. The opcodes match DaynaPort SCSI/Link:
//   0x09 RETRIEVE_STATS  - read data from this device (MIDI IN)
//   0x0C SET_IFACE_MODE  - configuration data (accept and ignore)
//   0x0D SET_MCAST_ADDR  - send data to this device (MIDI OUT)
//   0x0E ENABLE_INTERFACE - enable interface (accept with GOOD)
//
// Instead of a TAP network interface, this device relays data through a
// Unix domain socket to a MIDI bridge process.
//
//---------------------------------------------------------------------------

#include "midi_processor.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include "controllers/abstract_controller.h"
#include "shared/s2p_exceptions.h"

using namespace spdlog;
using namespace memory_util;
using namespace s2p_util;

MidiProcessor::MidiProcessor(int lun) : PrimaryDevice(SCMP, lun)
{
    SetProductData( { "ACORG", "MIDI Processor", "1.0" }, true);
    SetScsiLevel(ScsiLevel::SCSI_2);
    SupportsParams(true);
    SetReady(true);
}

string MidiProcessor::SetUp()
{
    AddCommand(ScsiCommand::RETRIEVE_STATS, [this]
        {
            RetrieveStats();
        });
    AddCommand(ScsiCommand::SET_IFACE_MODE, [this]
        {
            SetInterfaceMode();
        });
    AddCommand(ScsiCommand::SET_MCAST_ADDR, [this]
        {
            SendData();
        });
    AddCommand(ScsiCommand::ENABLE_INTERFACE, [this]
        {
            EnableInterface();
        });

    // Get socket path from params, or use default
    if (const auto &it = GetParams().find(SOCKET_PATH_PARAM); it != GetParams().end()) {
        socket_path = it->second;
    }
    else {
        socket_path = DEFAULT_SOCKET_PATH;
    }

    LogDebug(fmt::format("MIDI Processor configured with socket path: {}", socket_path));

    if (!ConnectSocket()) {
        LogWarn(fmt::format("MIDI Processor: could not connect to socket '{}' (will retry on I/O)", socket_path));
    }

    return "";
}

void MidiProcessor::CleanUp()
{
    DisconnectSocket();
}

vector<uint8_t> MidiProcessor::HandleInquiry() const
{
    return PrimaryDevice::HandleInquiry();
}

//---------------------------------------------------------------------------
//
// RetrieveStats (0x09) - Read data from the MIDI bridge (MIDI IN)
//
// The S3000XL sends: 09 00 00 00 LL 00
// Transfer length is in CDB byte 4 (1 byte) for DaynaPort-style,
// but for a processor SEND/RECEIVE the length field is bytes 2-4.
// We read what's available from the socket and return it.
//
//---------------------------------------------------------------------------
void MidiProcessor::RetrieveStats()
{
    // The S3000XL sends RETRIEVE STATS (0x09) during initialization.
    // For now, return GOOD status. When the socket bridge is connected,
    // this will return queued MIDI data.
    // TODO: implement socket read for MIDI IN path
    LogDebug("MIDI Processor: RetrieveStats - returning GOOD (no data)");
    StatusPhase();
}

//---------------------------------------------------------------------------
//
// SendData (0x0D / SET_MCAST_ADDR) - Write data to the MIDI bridge (MIDI OUT)
//
// The S3000XL sends: 0D 00 LL LL LL 00
// Transfer length is CDB bytes 2-4 (3 bytes, big-endian) for a 6-byte
// processor SEND command.
//
// For a 0-byte probe (0D 00 00 00 00 00), return GOOD status without
// throwing an error. DaynaPort throws ILLEGAL_REQUEST for length==0,
// but the S3000XL expects GOOD.
//
//---------------------------------------------------------------------------
void MidiProcessor::SendData() const
{
    const int length = GetCdbInt24(2);

    if (length == 0) {
        // Probe: S3000XL sends 0-byte SET_MCAST_ADDR to test if device is present
        StatusPhase();
        return;
    }

    GetController()->SetTransferSize(length, length);

    DataOutPhase(length);
}

//---------------------------------------------------------------------------
//
// WriteData - Called after DataOutPhase completes for SET_MCAST_ADDR
// and SET_IFACE_MODE.
//
//---------------------------------------------------------------------------
int MidiProcessor::WriteData(cdb_t cdb, data_out_t buf, int length)
{
    const auto opcode = static_cast<ScsiCommand>(cdb[0]);

    if (opcode == ScsiCommand::SET_MCAST_ADDR) {
        // MIDI OUT: write the received data to the socket
        if (sock_fd < 0) {
            ConnectSocket();
        }

        if (sock_fd >= 0) {
            const int written = SocketWrite(buf);
            if (written > 0) {
                byte_write_count += written;
                LogDebug(fmt::format("MIDI Processor: wrote {} byte(s) to socket", written));
            }
            else {
                LogWarn("MIDI Processor: failed to write data to socket");
            }
        }
        else {
            LogWarn("MIDI Processor: socket not connected, dropping MIDI data");
        }
    }
    else if (opcode == ScsiCommand::SET_IFACE_MODE) {
        // Configuration data: accept and ignore
        LogDebug(fmt::format("MIDI Processor: accepted {} byte(s) of config data", length));
    }

    GetController()->SetTransferSize(0, 0);

    return length;
}

//---------------------------------------------------------------------------
//
// SetInterfaceMode (0x0C) - Accept configuration data
//
// The S3000XL sends 84 bytes of configuration data via this command.
// We accept it and ignore the contents.
//
//---------------------------------------------------------------------------
void MidiProcessor::SetInterfaceMode() const
{
    // The S3000XL sends config data via 0x0C but we don't need it.
    // Accept with GOOD status regardless of transfer length.
    // Attempting DataOutPhase causes transfer size errors with the S3000XL.
    StatusPhase();
}

//---------------------------------------------------------------------------
//
// EnableInterface (0x0E) - Accept with GOOD status
//
//---------------------------------------------------------------------------
void MidiProcessor::EnableInterface() const
{
    LogDebug("MIDI Processor: EnableInterface");
    StatusPhase();
}

//---------------------------------------------------------------------------
//
// Socket helpers - Unix domain socket connection to the MIDI bridge
//
//---------------------------------------------------------------------------
bool MidiProcessor::ConnectSocket()
{
    DisconnectSocket();

    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LogWarn(fmt::format("MIDI Processor: failed to create socket: {}", strerror(errno)));
        return false;
    }

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LogWarn(fmt::format("MIDI Processor: failed to connect to '{}': {}", socket_path, strerror(errno)));
        close(sock_fd);
        sock_fd = -1;
        return false;
    }

    LogDebug(fmt::format("MIDI Processor: connected to socket '{}'", socket_path));
    return true;
}

void MidiProcessor::DisconnectSocket()
{
    if (sock_fd >= 0) {
        close(sock_fd);
        sock_fd = -1;
    }
}

int MidiProcessor::SocketRead(span<uint8_t> buf)
{
    if (sock_fd < 0) {
        return -1;
    }

    const ssize_t result = recv(sock_fd, buf.data(), buf.size(), 0);
    if (result < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            LogWarn(fmt::format("MIDI Processor: socket read error: {}", strerror(errno)));
            DisconnectSocket();
        }
        return -1;
    }
    if (result == 0) {
        // Connection closed
        LogWarn("MIDI Processor: socket connection closed by peer");
        DisconnectSocket();
        return -1;
    }

    return static_cast<int>(result);
}

int MidiProcessor::SocketWrite(span<const uint8_t> buf)
{
    if (sock_fd < 0) {
        return -1;
    }

    const ssize_t result = send(sock_fd, buf.data(), buf.size(), 0);
    if (result < 0) {
        LogWarn(fmt::format("MIDI Processor: socket write error: {}", strerror(errno)));
        DisconnectSocket();
        return -1;
    }

    return static_cast<int>(result);
}

vector<PbStatistics> MidiProcessor::GetStatistics() const
{
    vector<PbStatistics> statistics = PrimaryDevice::GetStatistics();

    EnrichStatistics(statistics, CATEGORY_INFO, BYTE_READ_COUNT, byte_read_count);
    EnrichStatistics(statistics, CATEGORY_INFO, BYTE_WRITE_COUNT, byte_write_count);

    return statistics;
}
