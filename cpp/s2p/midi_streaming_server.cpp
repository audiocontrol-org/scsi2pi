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

#endif // BUILD_SCMP
