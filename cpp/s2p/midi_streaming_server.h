//---------------------------------------------------------------------------
//
// SCSI2Pi — MIDI Streaming Server
//
// A dedicated TCP server for low-latency MIDI-via-SCSI communication.
// Uses persistent connections and event-based push: the client sends SysEx,
// the server internally polls the SCSI bus and pushes the response back.
//
// Protocol: symmetric length-delimited binary messages.
//   [4-byte LE length] [1-byte message type] [payload]
//
// Message types:
//   0x01 MIDI_STREAM_INIT  — Client sends target_id. Server responds with status.
//   0x02 MIDI_STREAM_SEND  — Client sends SysEx. Server sends, polls, pushes response.
//   0x03 MIDI_STREAM_DATA  — Server pushes response/unsolicited device data.
//   0x04 MIDI_STREAM_ERROR — Server pushes error message.
//
//---------------------------------------------------------------------------

#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

class CommandDispatcher;

using namespace std;

// OpenBSD does not support modern jthreads
#ifdef __OpenBSD__
#define jthread thread
#endif

class MidiStreamingServer {

public:
    MidiStreamingServer() = default;
    ~MidiStreamingServer();

    // Initialize the server on the given port. Returns empty string on success.
    string Init(int port, CommandDispatcher &dispatcher);

    // Start the server thread.
    void Start();

    // Stop the server and close connections.
    void Stop();

    bool IsRunning() const { return running; }

private:
    // Message type constants
    static constexpr uint8_t MSG_INIT        = 0x01;
    static constexpr uint8_t MSG_SEND        = 0x02;
    static constexpr uint8_t MSG_DATA        = 0x03;
    static constexpr uint8_t MSG_ERROR       = 0x04;
    static constexpr uint8_t MSG_SAMPLE_READ = 0x05;

    // Internal polling parameters
    static constexpr int POLL_INTERVAL_US = 500;     // Microseconds between polls
    static constexpr int POLL_MAX_ATTEMPTS = 6000;   // 500us * 6000 = 3 second timeout
    static constexpr int EMPTY_POLL_THRESHOLD = 2;   // Empty polls after data to confirm done

    // SDS sample receive parameters
    static constexpr int SDS_ACK_POLL_ATTEMPTS = 20000;  // 500us * 20000 = 10 second timeout per packet
    static constexpr int SDS_MAX_PACKETS = 10000;        // Safety limit

    void Execute();
    void HandleClient(int client_fd);
    vector<uint8_t> SendAndReceive(int target_id, const vector<uint8_t> &sysex);
    vector<uint8_t> ReceiveSample(int target_id, int sample_number, int channel);
    bool WriteMessage(int fd, uint8_t msg_type, const vector<uint8_t> &payload);
    bool ReadMessage(int fd, uint8_t &msg_type, vector<uint8_t> &payload);

    int server_fd = -1;
    atomic<bool> running{false};
    jthread server_thread;
    CommandDispatcher *dispatcher = nullptr;
};
