//---------------------------------------------------------------------------
//
// SCSI2Pi — MIDI Streaming Server Implementation
//
//---------------------------------------------------------------------------

#ifdef BUILD_SCMP

#include "midi_streaming_server.h"
#include "command/command_dispatcher.h"

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

string MidiStreamingServer::Init(int port, CommandDispatcher &disp)
{
    dispatcher = &disp;

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
    // Step 1: Read sample header to get SLNGTH (sample length)
    // RSDATA: F0 47 cc 0A 48 ss,ss F7 (nibble-encoded sample number)
    vector<uint8_t> rsdata = {
        0xF0, 0x47, static_cast<uint8_t>(channel), 0x0A, 0x48,
        static_cast<uint8_t>(sample_number & 0x0F),
        static_cast<uint8_t>((sample_number >> 4) & 0x0F),
        0xF7
    };

    auto header_response = SendAndReceive(target_id, rsdata);
    if (header_response.empty()) {
        fprintf(stderr, "[midi-stream] SAMPLE_READ: failed to read sample header\n");
        return {};
    }

    // Parse SLNGTH from header response (nibble-encoded, 4 bytes at offset ~24-31)
    // The header is in Akai SysEx format. We need the sample length.
    // For now, request a large number and let the device send what it has.
    // The device will stop sending packets when it runs out of data.
    fprintf(stderr, "[midi-stream] SAMPLE_READ: header received (%zu bytes), requesting sample data\n",
        header_response.size());

    // Step 2: Send RSPACK with 7-bit encoding
    // F0 47 cc 0C 48 ss,ss oo,oo,oo,oo nn,nn,nn,nn ii if F7
    // Request a large count — device will stop at actual sample length
    uint32_t request_count = 1000000; // Request up to 1M samples
    vector<uint8_t> rspack = {
        0xF0, 0x47, static_cast<uint8_t>(channel), 0x0C, 0x48,
        static_cast<uint8_t>(sample_number & 0x7F),
        static_cast<uint8_t>((sample_number >> 7) & 0x7F),
        0x00, 0x00, 0x00, 0x00,  // offset 0
        static_cast<uint8_t>(request_count & 0x7F),
        static_cast<uint8_t>((request_count >> 7) & 0x7F),
        static_cast<uint8_t>((request_count >> 14) & 0x7F),
        static_cast<uint8_t>((request_count >> 21) & 0x7F),
        0x01,  // interval 1
        0x00,  // function 0 (single)
        0xF7
    };

    // Send RSPACK
    auto send_cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_SEND, target_id, rspack);
    {
        unique_lock<mutex> lock(send_cmd->mtx);
        send_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return send_cmd->completed; });
    }
    if (!send_cmd->success) {
        fprintf(stderr, "[midi-stream] SAMPLE_READ: RSPACK send failed\n");
        return {};
    }

    fprintf(stderr, "[midi-stream] SAMPLE_READ: RSPACK sent, waiting for data packets...\n");

    // Step 3: Receive SDS data packets with ACK handshake
    // The device will send: optional Dump Header (F0 7E cc 01), then Data Packets (F0 7E cc 02)
    // We must ACK each one (F0 7E cc 7F pp F7) at SCSI bus speed.
    vector<uint8_t> all_audio_data;
    int packet_count = 0;
    bool transfer_complete = false;

    for (int pkt = 0; pkt < SDS_MAX_PACKETS && !transfer_complete; pkt++) {
        // Poll for incoming data
        vector<uint8_t> incoming;
        bool got_data = false;

        for (int attempt = 0; attempt < SDS_ACK_POLL_ATTEMPTS; attempt++) {
            this_thread::sleep_for(chrono::microseconds(POLL_INTERVAL_US));

            auto poll_cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_POLL, target_id);
            {
                unique_lock<mutex> lock(poll_cmd->mtx);
                poll_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return poll_cmd->completed; });
            }
            if (!poll_cmd->success) continue;

            int pending = poll_cmd->pending_bytes;
            if (pending > 0) {
                auto read_cmd = dispatcher->QueueMidiCommand(
                    PbOperation::MIDI_READ, target_id, {}, pending);
                {
                    unique_lock<mutex> lock(read_cmd->mtx);
                    read_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return read_cmd->completed; });
                }
                if (read_cmd->success && !read_cmd->response_data.empty()) {
                    incoming.insert(incoming.end(),
                        read_cmd->response_data.begin(), read_cmd->response_data.end());
                    got_data = true;
                    // Check if we have a complete SysEx message (ends with F7)
                    if (!incoming.empty() && incoming.back() == 0xF7) {
                        break;
                    }
                }
            } else if (got_data) {
                break; // Had data, no more pending
            }
        }

        if (!got_data) {
            fprintf(stderr, "[midi-stream] SAMPLE_READ: no more data after packet %d\n", packet_count);
            transfer_complete = true;
            break;
        }

        // Parse the incoming SysEx message
        // Check for SDS messages: F0 7E cc ...
        if (incoming.size() >= 4 && incoming[0] == 0xF0 && incoming[1] == 0x7E) {
            uint8_t sub_id = incoming[3];

            if (sub_id == 0x01) {
                // Dump Header — ACK it and continue
                fprintf(stderr, "[midi-stream] SAMPLE_READ: received Dump Header\n");
                vector<uint8_t> ack = {0xF0, 0x7E, static_cast<uint8_t>(channel), 0x7F, 0x00, 0xF7};
                auto ack_cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_SEND, target_id, ack);
                {
                    unique_lock<mutex> lock(ack_cmd->mtx);
                    ack_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return ack_cmd->completed; });
                }
            } else if (sub_id == 0x02) {
                // Data Packet — extract audio data, ACK it
                uint8_t packet_num = incoming[4];
                packet_count++;

                // Extract audio bytes (skip F0 7E cc 02 pp, strip checksum + F7)
                if (incoming.size() > 7) {
                    // SDS data: 120 bytes of 7-bit encoded audio between header and checksum
                    size_t data_start = 5;
                    size_t data_end = incoming.size() - 2; // before checksum and F7
                    for (size_t i = data_start; i < data_end; i++) {
                        all_audio_data.push_back(incoming[i]);
                    }
                }

                // Send ACK
                vector<uint8_t> ack = {
                    0xF0, 0x7E, static_cast<uint8_t>(channel), 0x7F,
                    static_cast<uint8_t>(packet_num & 0x7F), 0xF7
                };
                auto ack_cmd = dispatcher->QueueMidiCommand(PbOperation::MIDI_SEND, target_id, ack);
                {
                    unique_lock<mutex> lock(ack_cmd->mtx);
                    ack_cmd->cv.wait_for(lock, chrono::seconds(5), [&] { return ack_cmd->completed; });
                }

                if (packet_count % 10 == 0) {
                    fprintf(stderr, "[midi-stream] SAMPLE_READ: %d packets, %zu audio bytes\n",
                        packet_count, all_audio_data.size());
                }
            } else {
                fprintf(stderr, "[midi-stream] SAMPLE_READ: unexpected SDS sub_id=0x%02x\n", sub_id);
                transfer_complete = true;
            }
        } else if (incoming.size() >= 4 && incoming[0] == 0xF0 && incoming[1] == 0x47) {
            // Akai SysEx response (might be the initial SDATA header response to RSPACK)
            fprintf(stderr, "[midi-stream] SAMPLE_READ: Akai SysEx opcode=0x%02x len=%zu (skipping)\n",
                incoming[3], incoming.size());
            // Don't ACK Akai messages — they're metadata, not SDS
        } else {
            fprintf(stderr, "[midi-stream] SAMPLE_READ: unknown message len=%zu first=0x%02x\n",
                incoming.size(), incoming.empty() ? 0 : incoming[0]);
        }
    }

    fprintf(stderr, "[midi-stream] SAMPLE_READ: complete. %d packets, %zu audio bytes\n",
        packet_count, all_audio_data.size());

    return all_audio_data;
}

#endif // BUILD_SCMP
