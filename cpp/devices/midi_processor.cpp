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
//   0x0D (SET_MCAST_ADDR):  Initiator polls target for pending bytes (DATA IN)
//                           Response: 3 bytes, 00 HH LL = queued byte count
//   0x09 (RETRIEVE_STATS):  Initiator reads pending SysEx from target (DATA IN)
//
// The SCMP device relays SysEx bidirectionally between the SCSI bus and
// a Unix domain socket. A bridge daemon connects to the socket and
// controls the MIDI conversation.
//
//---------------------------------------------------------------------------

#include "midi_processor.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#include "controllers/abstract_controller.h"
#include "shared/s2p_exceptions.h"

using namespace spdlog;
using namespace memory_util;
using namespace s2p_util;

MidiProcessor::MidiProcessor(int lun) : PrimaryDevice(SCMP, lun)
{
    SetProductData( { "AKAI", "S3000XL", "2.00" }, true);
    SetScsiLevel(ScsiLevel::SCSI_2);
    SupportsParams(true);
    SetReady(true);
}

string MidiProcessor::SetUp()
{
    AddCommand(ScsiCommand::RETRIEVE_STATS, [this] { RetrieveStats(); });
    AddCommand(ScsiCommand::SET_IFACE_MODE, [this] { SetInterfaceMode(); });
    AddCommand(ScsiCommand::SET_MCAST_ADDR, [this] { SendData(); });
    AddCommand(ScsiCommand::ENABLE_INTERFACE, [this] { EnableInterface(); });

    socket_path = "/tmp/scsi-midi-bridge.sock";

    LogWarn(fmt::format("MIDI Processor: socket={}", socket_path));

    // Try to connect eagerly — the test harness/bridge should already be listening
    if (!ConnectSocket()) {
        LogWarn("MIDI Processor: socket not available yet (will retry on I/O)");
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
// 0x09 — RETRIEVE STATS: Read queued SysEx from response_buffer
//---------------------------------------------------------------------------
void MidiProcessor::RetrieveStats()
{
    if (response_buffer.empty()) {
        StatusPhase();
        return;
    }

    auto &buf = GetController()->GetBuffer();
    const int to_send = static_cast<int>(response_buffer.size());
    memcpy(buf.data(), response_buffer.data(), to_send);

    string hex;
    for (int i = 0; i < min(to_send, 20); ++i)
        hex += fmt::format("{:02x} ", response_buffer[i]);
    if (to_send > 20) hex += "...";
    LogWarn(fmt::format("MIDI 0x09: sending {} byte(s): {}", to_send, hex));

    response_buffer.clear();
    GetController()->SetTransferSize(to_send, to_send);
    DataInPhase(to_send);
}

//---------------------------------------------------------------------------
// 0x0C — SET INTERFACE MODE: Receive MIDI SysEx from initiator
//---------------------------------------------------------------------------
void MidiProcessor::SetInterfaceMode()
{
    const int length = GetCdbByte(4);
    if (length > 0) {
        DataOutPhase(length);
    } else {
        StatusPhase();
    }
}

//---------------------------------------------------------------------------
// 0x0D — SET MCAST ADDR: Poll — return pending byte count
//
// Before responding, drain any pending bytes from the socket into
// response_buffer. This is how the bridge daemon injects SysEx
// messages (dump requests, ACKs, etc.) for the S3000XL to read.
//---------------------------------------------------------------------------
void MidiProcessor::SendData()
{
    // Drain socket into response_buffer
    DrainSocket();

    auto &buf = GetController()->GetBuffer();
    const int pending = static_cast<int>(response_buffer.size());
    buf[0] = 0x00;
    buf[1] = static_cast<uint8_t>((pending >> 8) & 0xff);
    buf[2] = static_cast<uint8_t>(pending & 0xff);

    if (pending > 0) {
        LogWarn(fmt::format("MIDI 0x0D: {} byte(s) pending", pending));
    }

    GetController()->SetTransferSize(3, 3);
    DataInPhase(3);
}

//---------------------------------------------------------------------------
// 0x0E — ENABLE INTERFACE: Accept with GOOD
//---------------------------------------------------------------------------
void MidiProcessor::EnableInterface() const
{
    StatusPhase();
}

//---------------------------------------------------------------------------
// WriteData — Called after DataOutPhase for 0x0C
//
// Receives MIDI SysEx from the S3000XL. Writes it to the socket for
// the bridge daemon. Also auto-generates SDS ACKs for closed-loop
// handshaking (the bridge daemon may not respond fast enough for
// SCSI timing requirements).
//---------------------------------------------------------------------------
int MidiProcessor::WriteData(cdb_t cdb, data_out_t buf, int length)
{
    if (static_cast<ScsiCommand>(cdb[0]) != ScsiCommand::SET_IFACE_MODE) {
        return length;
    }

    const int data_len = static_cast<int>(buf.size());

    // Log received data
    string hex;
    for (int i = 0; i < min(data_len, 24); ++i)
        hex += fmt::format("{:02x} ", static_cast<uint8_t>(buf[i]));
    if (data_len > 24) hex += "...";
    LogWarn(fmt::format("MIDI 0x0C: received {} byte(s): {}", data_len, hex));

    // Write to socket for bridge daemon
    if (sock_fd < 0) ConnectSocket();
    if (sock_fd >= 0) {
        SocketWrite(buf);
    }

    // Auto-generate SDS ACK for closed-loop handshaking
    if (data_len >= 4 && buf[0] == 0xf0 && buf[1] == 0x7e) {
        const uint8_t channel = buf[2];
        const uint8_t command = buf[3];

        if (command == 0x01) {
            // Dump Header → send ACK to S3000XL via initiator command
            LogWarn("MIDI: SDS Dump Header → queuing ACK to target");
            vector<uint8_t> ack = { 0xf0, 0x7e, channel, 0x7f, 0x00, 0xf7 };
            QueueSendToTarget(6, ack);
        } else if (command == 0x02 && data_len >= 5) {
            // Data Packet → send ACK to S3000XL via initiator command
            const uint8_t pkt = buf[4];
            LogWarn(fmt::format("MIDI: SDS Data Packet #{} → queuing ACK to target", pkt));
            vector<uint8_t> ack = { 0xf0, 0x7e, channel, 0x7f, pkt, 0xf7 };
            QueueSendToTarget(6, ack);
        }
    }

    return length;
}

//---------------------------------------------------------------------------
// QueueSdsAck — Put an SDS ACK in the response buffer
//---------------------------------------------------------------------------
void MidiProcessor::QueueSdsAck(uint8_t channel, uint8_t packet_number)
{
    response_buffer.clear();
    response_buffer.push_back(0xf0);
    response_buffer.push_back(0x7e);
    response_buffer.push_back(channel);
    response_buffer.push_back(0x7f);  // ACK
    response_buffer.push_back(packet_number);
    response_buffer.push_back(0xf7);
}

//---------------------------------------------------------------------------
// DrainSocket — Read any pending bytes from socket into response_buffer
//---------------------------------------------------------------------------
void MidiProcessor::DrainSocket()
{
    if (sock_fd < 0) ConnectSocket();
    if (sock_fd < 0) return;

    pollfd pfd = { .fd = sock_fd, .events = POLLIN, .revents = 0 };
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN)) return;

    // Only read if response_buffer is empty (don't overwrite pending ACK)
    if (!response_buffer.empty()) return;

    uint8_t tmp[4096];
    const ssize_t n = recv(sock_fd, tmp, sizeof(tmp), MSG_DONTWAIT);
    if (n > 0) {
        response_buffer.assign(tmp, tmp + n);
        string hex;
        for (int i = 0; i < min(static_cast<int>(n), 20); ++i)
            hex += fmt::format("{:02x} ", tmp[i]);
        LogWarn(fmt::format("MIDI socket: read {} byte(s): {}", n, hex));
    } else if (n == 0) {
        LogWarn("MIDI socket: peer disconnected");
        DisconnectSocket();
    }
}

//---------------------------------------------------------------------------
// Initiator command queue
//---------------------------------------------------------------------------
MidiProcessor::InitiatorCommand MidiProcessor::PopInitiatorCommand()
{
    auto cmd = std::move(initiator_queue.front());
    initiator_queue.erase(initiator_queue.begin());
    return cmd;
}

void MidiProcessor::QueueSendToTarget(int target_id, const vector<uint8_t> &sysex)
{
    InitiatorCommand cmd;
    cmd.target_id = target_id;
    // CDB: 0x0C with length in byte 4
    cmd.cdb = { 0x0c, 0x00, 0x00, 0x00, static_cast<uint8_t>(sysex.size()), 0x00 };
    cmd.data = sysex;

    string hex;
    for (size_t i = 0; i < min(sysex.size(), (size_t)12); ++i)
        hex += fmt::format("{:02x} ", sysex[i]);
    LogWarn(fmt::format("MIDI: queued initiator 0x0C to target {}: {} ({} bytes)",
        target_id, hex, sysex.size()));

    initiator_queue.push_back(std::move(cmd));
}

//---------------------------------------------------------------------------
// Socket helpers
//---------------------------------------------------------------------------
bool MidiProcessor::ConnectSocket()
{
    DisconnectSocket();
    sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) return false;

    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(sock_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock_fd);
        sock_fd = -1;
        return false;
    }
    LogWarn(fmt::format("MIDI socket: connected to {}", socket_path));
    return true;
}

void MidiProcessor::DisconnectSocket()
{
    if (sock_fd >= 0) { close(sock_fd); sock_fd = -1; }
}

int MidiProcessor::SocketRead(span<uint8_t> buf)
{
    if (sock_fd < 0) return -1;
    const ssize_t r = recv(sock_fd, buf.data(), buf.size(), 0);
    if (r <= 0) { DisconnectSocket(); return -1; }
    return static_cast<int>(r);
}

int MidiProcessor::SocketWrite(span<const uint8_t> buf)
{
    if (sock_fd < 0) return -1;
    const ssize_t r = send(sock_fd, buf.data(), buf.size(), 0);
    if (r <= 0) { DisconnectSocket(); return -1; }
    return static_cast<int>(r);
}

vector<PbStatistics> MidiProcessor::GetStatistics() const
{
    vector<PbStatistics> statistics = PrimaryDevice::GetStatistics();
    EnrichStatistics(statistics, CATEGORY_INFO, BYTE_READ_COUNT, byte_read_count);
    EnrichStatistics(statistics, CATEGORY_INFO, BYTE_WRITE_COUNT, byte_write_count);
    return statistics;
}
