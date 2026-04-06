//---------------------------------------------------------------------------
//
// SCSI2Pi — MIDI Streaming Server Implementation
//
//---------------------------------------------------------------------------

#ifdef BUILD_SCMP

#include "midi_streaming_server.h"
#include "command/command_dispatcher.h"
#include "devices/midi_processor.h"

#include <arpa/inet.h>
#include <endian.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>

using namespace std;

MidiStreamingServer::~MidiStreamingServer()
{
    Stop();
}

string MidiStreamingServer::Init(int port, CommandDispatcher &disp, MidiProcessor *midi_proc)
{
    dispatcher = &disp;
    midi_processor = midi_proc;

    server_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return "MIDI streaming server: failed to create socket";
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (::bind(server_fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd);
        server_fd = -1;
        return "MIDI streaming server: failed to bind to port " + to_string(port);
    }

    if (listen(server_fd, 1) < 0) {
        close(server_fd);
        server_fd = -1;
        return "MIDI streaming server: failed to listen";
    }

    fprintf(stderr, "[midi-stream] listening on port %d\n", port);
    return "";
}

void MidiStreamingServer::Start()
{
    running = true;
    server_thread = jthread([this] { Execute(); });
}

void MidiStreamingServer::Stop()
{
    running = false;
    if (server_fd >= 0) {
        close(server_fd);
        server_fd = -1;
    }
    if (server_thread.joinable()) {
        server_thread.join();
    }
}

void MidiStreamingServer::Execute()
{
    while (running) {
        // Accept one client at a time (MIDI bus is single-threaded)
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (client_fd < 0) {
            if (running) {
                fprintf(stderr, "[midi-stream] accept error: %s\n", strerror(errno));
            }
            continue;
        }

        // Disable Nagle's algorithm for low latency
        int flag = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        fprintf(stderr, "[midi-stream] client connected\n");
        HandleClient(client_fd);
        close(client_fd);
        fprintf(stderr, "[midi-stream] client disconnected\n");
    }
}

void MidiStreamingServer::HandleClient(int client_fd)
{
    int target_id = -1;

    while (running) {
        uint8_t msg_type;
        vector<uint8_t> payload;

        if (!ReadMessage(client_fd, msg_type, payload)) {
            break;
        }

        switch (msg_type) {
            case MSG_INIT: {
                if (payload.empty()) {
                    vector<uint8_t> err = {'m', 'i', 's', 's', 'i', 'n', 'g', ' ', 't', 'a', 'r', 'g', 'e', 't'};
                    WriteMessage(client_fd, MSG_ERROR, err);
                    break;
                }
                target_id = payload[0];

                auto cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_INIT, target_id);
                {
                    unique_lock<mutex> lock(cmd->mtx);
                    cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return cmd->completed; });
                }

                vector<uint8_t> response = {static_cast<uint8_t>(cmd->success ? 1 : 0)};
                WriteMessage(client_fd, MSG_INIT, response);
                fprintf(stderr, "[midi-stream] INIT target=%d success=%d\n", target_id, cmd->success);
                break;
            }

            case MSG_SEND: {
                if (target_id < 0) {
                    vector<uint8_t> err = {'n', 'o', 't', ' ', 'i', 'n', 'i', 't'};
                    WriteMessage(client_fd, MSG_ERROR, err);
                    break;
                }

                auto t0 = chrono::steady_clock::now();
                auto response = SendAndReceive(target_id, payload);
                auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                    chrono::steady_clock::now() - t0).count();

                fprintf(stderr, "[midi-stream] SEND %zu bytes -> response %zu bytes in %lld ms\n",
                    payload.size(), response.size(), static_cast<long long>(elapsed));

                WriteMessage(client_fd, MSG_DATA, response);
                break;
            }

            case MSG_SAMPLE_READ: {
                if (target_id < 0) {
                    vector<uint8_t> err = {'n', 'o', 't', ' ', 'i', 'n', 'i', 't'};
                    WriteMessage(client_fd, MSG_ERROR, err);
                    break;
                }
                if (payload.empty()) {
                    vector<uint8_t> err = {'m', 'i', 's', 's', 'i', 'n', 'g', ' ', 's', 'a', 'm', 'p', 'l', 'e', '#'};
                    WriteMessage(client_fd, MSG_ERROR, err);
                    break;
                }

                int sample_number = payload[0];
                int channel = 0; // Default exclusive channel

                auto t0 = chrono::steady_clock::now();
                auto pcm_data = ReceiveSample(target_id, sample_number, channel);
                auto elapsed = chrono::duration_cast<chrono::milliseconds>(
                    chrono::steady_clock::now() - t0).count();

                fprintf(stderr, "[midi-stream] SAMPLE_READ sample=%d -> %zu bytes in %lld ms\n",
                    sample_number, pcm_data.size(), static_cast<long long>(elapsed));

                WriteMessage(client_fd, MSG_DATA, pcm_data);
                break;
            }

            default:
                fprintf(stderr, "[midi-stream] unknown message type: 0x%02x\n", msg_type);
                break;
        }
    }
}

vector<uint8_t> MidiStreamingServer::SendAndReceive(int target_id, const vector<uint8_t> &sysex)
{
    // Step 1: Send the SysEx to the device
    auto send_cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_SEND, target_id, sysex);
    {
        unique_lock<mutex> lock(send_cmd->mtx);
        send_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return send_cmd->completed; });
    }
    if (!send_cmd->success) {
        return {};
    }

    // Step 2: Tight-poll internally at high frequency until response is ready.
    // This runs at microsecond intervals with no network overhead — the poll
    // and read commands go directly through the dispatcher to the SCSI bus.
    vector<uint8_t> result;
    int empty_polls = 0;

    for (int attempt = 0; attempt < POLL_MAX_ATTEMPTS; attempt++) {
        this_thread::sleep_for(chrono::microseconds(POLL_INTERVAL_US));

        auto poll_cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_POLL, target_id);
        {
            unique_lock<mutex> lock(poll_cmd->mtx);
            poll_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return poll_cmd->completed; });
        }

        if (!poll_cmd->success) {
            break;
        }

        int pending = poll_cmd->pending_bytes;

        if (pending > 0) {
            auto read_cmd = dispatcher->QueueMidiCommand(
                PbOperation::MIDI_READ, target_id, {}, pending);
            {
                unique_lock<mutex> lock(read_cmd->mtx);
                read_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return read_cmd->completed; });
            }

            if (read_cmd->success && !read_cmd->response_data.empty()) {
                result.insert(result.end(),
                    read_cmd->response_data.begin(), read_cmd->response_data.end());
            }
            empty_polls = 0;
        } else if (!result.empty()) {
            empty_polls++;
            if (empty_polls >= EMPTY_POLL_THRESHOLD) {
                break;
            }
        }
    }

    return result;
}

bool MidiStreamingServer::WriteMessage(int fd, uint8_t msg_type, const vector<uint8_t> &payload)
{
    uint32_t total_len = static_cast<uint32_t>(1 + payload.size());
    uint32_t le_len = htole32(total_len);

    if (write(fd, &le_len, 4) != 4) return false;
    if (write(fd, &msg_type, 1) != 1) return false;
    if (!payload.empty()) {
        ssize_t written = 0;
        while (written < static_cast<ssize_t>(payload.size())) {
            ssize_t n = write(fd, payload.data() + written, payload.size() - written);
            if (n <= 0) return false;
            written += n;
        }
    }
    return true;
}

bool MidiStreamingServer::ReadMessage(int fd, uint8_t &msg_type, vector<uint8_t> &payload)
{
    uint32_t le_len;
    ssize_t total = 0;
    while (total < 4) {
        ssize_t n = read(fd, reinterpret_cast<uint8_t*>(&le_len) + total, 4 - total);
        if (n <= 0) return false;
        total += n;
    }

    uint32_t msg_len = le32toh(le_len);
    if (msg_len == 0 || msg_len > 1024 * 1024) return false;

    if (read(fd, &msg_type, 1) != 1) return false;

    uint32_t payload_len = msg_len - 1;
    payload.resize(payload_len);
    total = 0;
    while (total < static_cast<ssize_t>(payload_len)) {
        ssize_t n = read(fd, payload.data() + total, payload_len - total);
        if (n <= 0) return false;
        total += n;
    }

    return true;
}

vector<uint8_t> MidiStreamingServer::ReceiveSample(int target_id, int sample_number, int channel)
{
    if (!midi_processor) {
        fprintf(stderr, "[midi-stream] SAMPLE_READ: no MidiProcessor available\n");
        return {};
    }

    // Clear any stale data in the event queue
    midi_processor->ClearSysExQueue();

    // Send standard SDS Dump Request: F0 7E cc 03 ss ss F7
    vector<uint8_t> dump_request = {
        0xF0, 0x7E, static_cast<uint8_t>(channel), 0x03,
        static_cast<uint8_t>(sample_number & 0x7F),
        static_cast<uint8_t>((sample_number >> 7) & 0x7F),
        0xF7
    };

    auto send_cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_SEND, target_id, dump_request);
    {
        unique_lock<mutex> lock(send_cmd->mtx);
        send_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return send_cmd->completed; });
    }
    if (!send_cmd->success) {
        fprintf(stderr, "[midi-stream] SAMPLE_READ: SDS Dump Request send failed\n");
        return {};
    }

    fprintf(stderr, "[midi-stream] SAMPLE_READ: SDS Dump Request sent for sample %d\n", sample_number);

    // Event-driven receive: wait on MidiProcessor's event queue instead of polling.
    // The main loop handles target transactions (device writes) in WaitForSelection().
    // MidiProcessor.WriteData() pushes received SysEx to the queue and signals us.
    // The auto-ACK in WriteData handles ACKs for SDS packets automatically.
    //
    // We do NOT poll with MIDI_POLL — that would block the main loop from
    // accepting the device's target-mode writes.

    vector<uint8_t> all_audio_data;
    int packet_count = 0;
    int bits_per_sample = 16;

    for (int pkt = 0; pkt < SDS_MAX_PACKETS; pkt++) {
        vector<uint8_t> incoming;
        midi_processor->WaitForSysEx(incoming, 10000); // 10 second timeout per message

        if (incoming.empty()) {
            if (packet_count > 0) {
                fprintf(stderr, "[midi-stream] SAMPLE_READ: transfer complete after %d packets\n", packet_count);
            } else {
                fprintf(stderr, "[midi-stream] SAMPLE_READ: no response to Dump Request\n");
            }
            break;
        }

        // Parse the SDS message
        if (incoming.size() < 4 || incoming[0] != 0xF0 || incoming[1] != 0x7E) {
            fprintf(stderr, "[midi-stream] SAMPLE_READ: non-SDS message len=%zu (0x%02x)\n",
                incoming.size(), incoming.empty() ? 0 : incoming[0]);
            continue;
        }

        uint8_t sub_id = incoming[3];

        if (sub_id == 0x01) {
            // Dump Header
            bits_per_sample = incoming.size() > 6 ? incoming[6] : 16;
            int period = (incoming.size() > 9) ?
                (incoming[7] | (incoming[8] << 7) | (incoming[9] << 14)) : 0;
            int length = (incoming.size() > 12) ?
                (incoming[10] | (incoming[11] << 7) | (incoming[12] << 14)) : 0;
            int sample_rate = period > 0 ? static_cast<int>(1000000000.0 / period) : 0;

            fprintf(stderr, "[midi-stream] SAMPLE_READ: Dump Header — bits=%d rate=%dHz length=%d\n",
                bits_per_sample, sample_rate, length);
            // ACK is handled automatically by MidiProcessor::WriteData

        } else if (sub_id == 0x02) {
            // Data Packet: F0 7E cc 02 pp [120 bytes] checksum F7
            uint8_t packet_num = incoming[4];
            packet_count++;

            // Extract 7-bit encoded audio bytes (between header and checksum)
            if (incoming.size() > 7) {
                size_t data_start = 5;
                size_t data_end = incoming.size() - 2;
                for (size_t i = data_start; i < data_end; i++) {
                    all_audio_data.push_back(incoming[i]);
                }
            }
            // ACK is handled automatically by MidiProcessor::WriteData

            if (packet_count % 10 == 0 || packet_count <= 3) {
                fprintf(stderr, "[midi-stream] SAMPLE_READ: packet %d (pkt#%d), %zu audio bytes total\n",
                    packet_count, packet_num, all_audio_data.size());
            }
        } else {
            fprintf(stderr, "[midi-stream] SAMPLE_READ: unexpected SDS sub_id=0x%02x\n", sub_id);
            break;
        }
    }

    fprintf(stderr, "[midi-stream] SAMPLE_READ: done. %d packets, %zu raw bytes (%d-bit)\n",
        packet_count, all_audio_data.size(), bits_per_sample);

    return all_audio_data;
}

#endif // BUILD_SCMP
