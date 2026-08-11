// Copyright 2026 Kevin Ahrendt
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/* Unit tests for micro-decoder's orchestration layer.
 *
 * These tests target this library's own logic: file-type detection from
 * Content-Type/URL, the TransferBuffer and RingBuffer plumbing, EventFlags
 * semantics, and the DecoderSource lifecycle (thread orchestration, state
 * machine, listener callback contracts) on both the play_buffer() and
 * play_url() paths. Codec decode math is validated in each codec's own repo.
 *
 * WAV is raw PCM pass-through, so a synthesized in-memory WAV whose payload
 * must come out of on_audio_write() bit-exact is the end-to-end reference:
 * any byte dropped, duplicated, or reordered anywhere in the pipeline
 * (HTTP client -> TransferBuffer -> RingBuffer -> AudioDecoder -> listener,
 * including backpressure retries) shows up as a mismatch. No fixture files
 * are needed; everything is generated in code.
 *
 * The play_url() tests run against a minimal local HTTP server on a loopback
 * socket, which lets them cover the reader-thread paths where several past
 * bugs lived (M3U playlists misdetected as MP3, the reader continuing to
 * download after the decoder thread exited).
 *
 * Usage: test_micro_decoder [test_name]
 */

#include "micro_decoder/decoder_source.h"
#include "micro_decoder/types.h"

// Internal headers (tests link the static library and poke its internals)
#include "md_transfer_buffer.h"
#include "platform/event_flags.h"
#include "ring_buffer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>  // setenv() is POSIX, so not guaranteed by <cstdlib>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace micro_decoder;

// Abort the current test on the first failed condition, reporting the line.
#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("    CHECK failed: %s (line %d)\n", #cond, __LINE__); \
            return false;                                                     \
        }                                                                     \
    } while (0)

// Like CHECK(a == b) but reports both operands' values on failure.
#define CHECK_EQ(a, b)                                                  \
    do {                                                                \
        const long long _va = static_cast<long long>(a);                \
        const long long _vb = static_cast<long long>(b);                \
        if (_va != _vb) {                                               \
            std::printf("    CHECK_EQ failed: %s == %s (%lld vs %lld) " \
                        "(line %d)\n",                                  \
                        #a, #b, _va, _vb, __LINE__);                    \
            return false;                                               \
        }                                                               \
    } while (0)

static uint64_t now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

// ============================================================================
// WAV synthesis
//
// Deterministic 16-bit PCM from an LCG so both ends of a test can regenerate
// the exact byte sequence independently.
// ============================================================================

static void push_u16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}

static void push_u32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}

// 16-bit PCM payload of `frames * channels` samples
static std::vector<uint8_t> make_pcm(uint8_t channels, size_t frames, uint32_t seed = 0x1234567) {
    std::vector<uint8_t> pcm;
    pcm.reserve(frames * channels * 2);
    uint32_t s = seed;
    for (size_t i = 0; i < frames * channels; i++) {
        s = s * 1103515245U + 12345U;
        push_u16le(pcm, static_cast<uint16_t>(s >> 16));
    }
    return pcm;
}

// Canonical RIFF/WAVE header declaring a 16-bit PCM data chunk of data_size bytes
static std::vector<uint8_t> make_wav_header(uint8_t channels, uint32_t sample_rate,
                                            uint32_t data_size) {
    std::vector<uint8_t> wav;
    wav.insert(wav.end(), {'R', 'I', 'F', 'F'});
    push_u32le(wav, 36 + data_size);
    wav.insert(wav.end(), {'W', 'A', 'V', 'E'});
    wav.insert(wav.end(), {'f', 'm', 't', ' '});
    push_u32le(wav, 16);
    push_u16le(wav, 1);  // PCM
    push_u16le(wav, channels);
    push_u32le(wav, sample_rate);
    push_u32le(wav, sample_rate * channels * 2);  // byte rate
    push_u16le(wav, static_cast<uint16_t>(channels * 2));
    push_u16le(wav, 16);  // bits per sample
    wav.insert(wav.end(), {'d', 'a', 't', 'a'});
    push_u32le(wav, data_size);
    return wav;
}

static std::vector<uint8_t> make_wav(uint8_t channels, uint32_t sample_rate, size_t frames,
                                     std::vector<uint8_t>* payload_out = nullptr,
                                     uint32_t seed = 0x1234567) {
    std::vector<uint8_t> payload = make_pcm(channels, frames, seed);
    std::vector<uint8_t> wav =
        make_wav_header(channels, sample_rate, static_cast<uint32_t>(payload.size()));
    wav.insert(wav.end(), payload.begin(), payload.end());
    if (payload_out != nullptr) {
        *payload_out = std::move(payload);
    }
    return wav;
}

// ============================================================================
// Capturing listener + loop pump
// ============================================================================

/// @brief DecoderListener that records everything it receives.
/// PCM and stream infos arrive on the decoder thread (mutex-guarded);
/// state changes arrive on the test thread via loop(), so `states` is
/// accessed without a lock.
class CaptureListener : public DecoderListener {
public:
    void on_stream_info(const AudioStreamInfo& info) override {
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->infos_.push_back(info);
    }

    size_t on_audio_write(const uint8_t* data, size_t length, uint32_t timeout_ms) override {
        size_t take = std::min(length, this->max_consume);
        {
            std::lock_guard<std::mutex> lock(this->mtx_);
            this->last_write_timeout_ms_ = timeout_ms;
            this->pcm_.insert(this->pcm_.end(), data, data + take);
        }
        if (this->consume_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(this->consume_delay_ms));
        }
        return take;
    }

    void on_state_change(DecoderState state) override {
        this->states.push_back(state);
        if (this->on_state) {
            this->on_state(state);
        }
    }

    std::vector<uint8_t> pcm() const {
        std::lock_guard<std::mutex> lock(this->mtx_);
        return this->pcm_;
    }

    size_t pcm_size() const {
        std::lock_guard<std::mutex> lock(this->mtx_);
        return this->pcm_.size();
    }

    std::vector<AudioStreamInfo> infos() const {
        std::lock_guard<std::mutex> lock(this->mtx_);
        return this->infos_;
    }

    uint32_t last_write_timeout_ms() const {
        std::lock_guard<std::mutex> lock(this->mtx_);
        return this->last_write_timeout_ms_;
    }

    bool saw_state(DecoderState s) const {
        return std::find(this->states.begin(), this->states.end(), s) != this->states.end();
    }

    void reset_capture() {
        std::lock_guard<std::mutex> lock(this->mtx_);
        this->pcm_.clear();
        this->infos_.clear();
        this->states.clear();
    }

    // Test knobs / test-thread-only capture
    std::function<void(DecoderState)> on_state;  // extra hook, runs on the loop thread
    std::vector<DecoderState> states;            // loop-thread callbacks, in order
    size_t max_consume{SIZE_MAX};                // backpressure: max bytes accepted per write
    uint32_t consume_delay_ms{0};                // throttle: sleep before accepting

private:
    mutable std::mutex mtx_;
    std::vector<uint8_t> pcm_;
    std::vector<AudioStreamInfo> infos_;
    uint32_t last_write_timeout_ms_{0};
};

// Pump src.loop() until pred() holds or timeout_ms elapses.
static bool pump_until(DecoderSource& src, const std::function<bool()>& pred,
                       uint32_t timeout_ms = 15000) {
    uint64_t deadline = now_ms() + timeout_ms;
    while (now_ms() < deadline) {
        src.loop();
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

// Pump until the listener has observed a terminal state (IDLE after PLAYING,
// or FAILED at any point).
static bool pump_to_completion(DecoderSource& src, const CaptureListener& listener,
                               uint32_t timeout_ms = 15000) {
    return pump_until(
        src,
        [&]() {
            return listener.saw_state(DecoderState::FAILED) ||
                   (listener.saw_state(DecoderState::PLAYING) &&
                    listener.saw_state(DecoderState::IDLE));
        },
        timeout_ms);
}

// ============================================================================
// Minimal loopback HTTP server for the play_url() tests
// ============================================================================

/// @brief Single-threaded HTTP/1.1 server bound to 127.0.0.1:<ephemeral>.
/// Serves the same configured response to every connection, optionally
/// throttled or endless, and records what it observed for assertions.
class TestHttpServer {
public:
    struct Response {
        std::string status{"200 OK"};
        std::string content_type{"audio/wav"};  // empty string: omit the header
        std::vector<uint8_t> body;
        std::vector<uint8_t> infinite_pattern;  // non-empty: after body, loop this forever
        size_t chunk_size{0};                   // 0: send everything in one call
        uint32_t chunk_delay_ms{0};
    };

    ~TestHttpServer() {
        this->stop();
    }

    bool start(Response response) {
        this->response_ = std::move(response);
        this->listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (this->listen_fd_ < 0) {
            return false;
        }
        int one = 1;
        ::setsockopt(this->listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(this->listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(this->listen_fd_, 4) != 0) {
            ::close(this->listen_fd_);
            this->listen_fd_ = -1;
            return false;
        }
        socklen_t addr_len = sizeof(addr);
        if (::getsockname(this->listen_fd_, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
            ::close(this->listen_fd_);
            this->listen_fd_ = -1;
            return false;
        }
        this->port_ = ntohs(addr.sin_port);

        this->running_.store(true);
        this->thread_ = std::thread([this]() { this->serve_loop(); });
        return true;
    }

    void stop() {
        this->running_.store(false);
        // serve_loop() only observes running_ between blocking syscalls, so a thread parked
        // in send() to a peer that stopped reading but has not hung up would never see the
        // stop and join() would hang forever. Shutting the accepted socket down from here
        // makes that send() fail immediately, so a failing test fails fast instead of
        // wedging until the ctest timeout.
        int conn = this->conn_fd_.load();
        if (conn >= 0) {
            ::shutdown(conn, SHUT_RDWR);
        }
        if (this->thread_.joinable()) {
            this->thread_.join();
        }
        if (this->listen_fd_ >= 0) {
            ::close(this->listen_fd_);
            this->listen_fd_ = -1;
        }
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(this->port_) + path;
    }

    // True once a send() failed because the client hung up mid-body
    bool client_disconnected() const {
        return this->client_disconnected_.load();
    }

private:
    void serve_loop() {
        while (this->running_.load()) {
            pollfd pfd{};
            pfd.fd = this->listen_fd_;
            pfd.events = POLLIN;
            int r = ::poll(&pfd, 1, 50);
            if (r <= 0) {
                continue;
            }
            int conn = ::accept(this->listen_fd_, nullptr, nullptr);
            if (conn < 0) {
                continue;
            }
            this->connections_.fetch_add(1);
#ifdef SO_NOSIGPIPE
            int one = 1;
            ::setsockopt(conn, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            // Published so stop() can unblock a send() that is stuck on this connection
            this->conn_fd_.store(conn);
            this->handle(conn);
            this->conn_fd_.store(-1);
            ::close(conn);
        }
    }

    // Send everything or report the client hung up
    bool send_all(int conn, const uint8_t* data, size_t len) {
        size_t off = 0;
        while (off < len && this->running_.load()) {
#ifdef MSG_NOSIGNAL
            ssize_t n = ::send(conn, data + off, len - off, MSG_NOSIGNAL);
#else
            ssize_t n = ::send(conn, data + off, len - off, 0);
#endif
            if (n <= 0) {
                this->client_disconnected_.store(true);
                return false;
            }
            off += static_cast<size_t>(n);
            this->bytes_sent_.fetch_add(static_cast<size_t>(n));
        }
        return off == len;
    }

    void handle(int conn) {
        // Read the request head (discarded; every request gets the same response)
        char buf[2048];
        std::string request;
        uint64_t deadline = now_ms() + 5000;
        while (this->running_.load() && now_ms() < deadline &&
               request.find("\r\n\r\n") == std::string::npos) {
            pollfd pfd{};
            pfd.fd = conn;
            pfd.events = POLLIN;
            if (::poll(&pfd, 1, 50) <= 0) {
                continue;
            }
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                return;
            }
            request.append(buf, static_cast<size_t>(n));
        }

        const Response& resp = this->response_;
        const bool endless = !resp.infinite_pattern.empty();

        std::string head = "HTTP/1.1 " + resp.status + "\r\n";
        if (!resp.content_type.empty()) {
            head += "Content-Type: " + resp.content_type + "\r\n";
        }
        if (!endless) {
            head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
        }
        head += "Connection: close\r\n\r\n";
        if (!this->send_all(conn, reinterpret_cast<const uint8_t*>(head.data()), head.size())) {
            return;
        }

        // Finite body, optionally throttled
        size_t chunk = resp.chunk_size == 0 ? resp.body.size() : resp.chunk_size;
        size_t off = 0;
        while (off < resp.body.size() && this->running_.load()) {
            size_t n = std::min(chunk, resp.body.size() - off);
            if (!this->send_all(conn, resp.body.data() + off, n)) {
                return;
            }
            off += n;
            if (resp.chunk_delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(resp.chunk_delay_ms));
            }
        }

        // Endless tail: keep streaming until the client disconnects or stop()
        while (endless && this->running_.load()) {
            if (!this->send_all(conn, resp.infinite_pattern.data(),
                                resp.infinite_pattern.size())) {
                return;
            }
            if (resp.chunk_delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(resp.chunk_delay_ms));
            }
        }
    }

    // Struct fields
    Response response_;
    std::thread thread_;

    // size_t / atomic fields
    std::atomic<size_t> bytes_sent_{0};
    std::atomic<int> connections_{0};
    /// @brief Accepted connection currently being served, or -1; see stop()
    std::atomic<int> conn_fd_{-1};
    std::atomic<bool> client_disconnected_{false};
    std::atomic<bool> running_{false};

    // 32-bit and smaller fields
    int listen_fd_{-1};
    uint16_t port_{0};
};

// ============================================================================
// File type detection
// ============================================================================

static bool test_detect_content_type() {
    struct Case {
        const char* content_type;
        const char* url;
        AudioFileType expected;
    };
    const Case cases[] = {
        // One accepted MIME type per codec: FLAC, MP3, Opus, Vorbis, WAV
        {"audio/flac", nullptr, AudioFileType::FLAC},
        {"audio/x-flac", nullptr, AudioFileType::FLAC},
        {"audio/mpeg", nullptr, AudioFileType::MP3},
        {"audio/mp3", nullptr, AudioFileType::MP3},
        {"audio/x-mpeg", nullptr, AudioFileType::MP3},
        {"audio/opus", nullptr, AudioFileType::OPUS},
        {"audio/vorbis", nullptr, AudioFileType::VORBIS},
        {"audio/wav", nullptr, AudioFileType::WAV},
        {"audio/x-wav", nullptr, AudioFileType::WAV},
        {"audio/wave", nullptr, AudioFileType::WAV},

        // Parameters and case-insensitivity
        {"audio/mpeg; charset=UTF-8", nullptr, AudioFileType::MP3},
        {"Audio/MPEG", nullptr, AudioFileType::MP3},
        {"AUDIO/FLAC", nullptr, AudioFileType::FLAC},

        // M3U playlist types must NOT be detected as MP3 even though they
        // contain the substring "audio/mpeg" (regression: playlists were
        // fed to the MP3 decoder)
        {"audio/mpegurl", nullptr, AudioFileType::NONE},
        {"audio/x-mpegurl", nullptr, AudioFileType::NONE},
        {"application/vnd.apple.mpegurl", nullptr, AudioFileType::NONE},
        {"audio/mpegurl", "http://radio.example/listen.m3u", AudioFileType::NONE},

        // Ogg container classification: an explicit opus token means Opus,
        // any other Ogg type is Vorbis. "notopus" must not match the token.
        {"audio/ogg;codecs=opus", nullptr, AudioFileType::OPUS},
        {"application/ogg; codecs=\"opus\"", nullptr, AudioFileType::OPUS},
        {"AUDIO/OGG;CODECS=OPUS", nullptr, AudioFileType::OPUS},
        {"audio/ogg;codecs=notopus", nullptr, AudioFileType::VORBIS},
        {"audio/ogg;codecs=vorbis", nullptr, AudioFileType::VORBIS},
        {"audio/ogg", nullptr, AudioFileType::VORBIS},
        {"application/ogg", nullptr, AudioFileType::VORBIS},

        // Unknown types with no usable URL
        {"text/html", nullptr, AudioFileType::NONE},
        {"application/octet-stream", nullptr, AudioFileType::NONE},
        {"", nullptr, AudioFileType::NONE},
        {nullptr, nullptr, AudioFileType::NONE},

        // Content-Type wins over the URL extension when both are usable
        {"audio/flac", "http://x.example/song.mp3", AudioFileType::FLAC},
    };

    for (const Case& c : cases) {
        AudioFileType got = detect_audio_file_type(c.content_type, c.url);
        if (got != c.expected) {
            std::printf("    content_type='%s' url='%s': got %s, expected %s (line %d)\n",
                        c.content_type != nullptr ? c.content_type : "(null)",
                        c.url != nullptr ? c.url : "(null)", audio_file_type_to_string(got),
                        audio_file_type_to_string(c.expected), __LINE__);
            return false;
        }
    }
    return true;
}

static bool test_detect_url_extension() {
    struct Case {
        const char* content_type;
        const char* url;
        AudioFileType expected;
    };
    const Case cases[] = {
        // Extension fallback per codec: FLAC, MP3, Opus, Vorbis, WAV
        {nullptr, "http://x.example/song.flac", AudioFileType::FLAC},
        {nullptr, "http://x.example/song.mp3", AudioFileType::MP3},
        {nullptr, "http://x.example/song.opus", AudioFileType::OPUS},
        {nullptr, "http://x.example/song.ogg", AudioFileType::VORBIS},
        {nullptr, "http://x.example/song.wav", AudioFileType::WAV},

        // Unusable Content-Type falls back to the extension
        {"application/octet-stream", "http://x.example/song.wav", AudioFileType::WAV},

        // Case-insensitive extensions
        {nullptr, "http://x.example/SONG.MP3", AudioFileType::MP3},
        {nullptr, "http://x.example/song.Flac", AudioFileType::FLAC},

        // Query strings are excluded from extension matching, even when the
        // query itself looks like a filename
        {nullptr, "http://x.example/song.mp3?session=1", AudioFileType::MP3},
        {nullptr, "http://x.example/a.flac?next=b.mp3", AudioFileType::FLAC},
        {nullptr, "http://x.example/stream?file=song.mp3", AudioFileType::NONE},

        // No usable extension
        {nullptr, "http://x.example/stream", AudioFileType::NONE},
        {nullptr, "http://x.example/song.aac", AudioFileType::NONE},
        {nullptr, "http://x.example/playlist.m3u8", AudioFileType::NONE},
        {nullptr, "", AudioFileType::NONE},
    };

    for (const Case& c : cases) {
        AudioFileType got = detect_audio_file_type(c.content_type, c.url);
        if (got != c.expected) {
            std::printf("    content_type='%s' url='%s': got %s, expected %s\n",
                        c.content_type != nullptr ? c.content_type : "(null)", c.url,
                        audio_file_type_to_string(got), audio_file_type_to_string(c.expected));
            return false;
        }
    }
    return true;
}

// ============================================================================
// AudioStreamInfo
// ============================================================================

static bool test_stream_info() {
    // Default: 16-bit mono 16 kHz
    AudioStreamInfo def;
    CHECK_EQ(def.get_bits_per_sample(), 16);
    CHECK_EQ(def.get_channels(), 1);
    CHECK_EQ(def.get_sample_rate(), 16000u);

    // 16-bit stereo: a frame is 4 bytes
    AudioStreamInfo cd(16, 2, 44100);
    CHECK_EQ(cd.frames_to_bytes(1), 4u);
    CHECK_EQ(cd.frames_to_bytes(441), 1764u);
    CHECK_EQ(cd.samples_to_bytes(3), 6u);

    // 24-bit rounds up to 3 bytes per sample
    AudioStreamInfo hires(24, 2, 96000);
    CHECK_EQ(hires.samples_to_bytes(10), 30u);
    CHECK_EQ(hires.frames_to_bytes(10), 60u);

    // Equality covers every field
    CHECK(cd == AudioStreamInfo(16, 2, 44100));
    CHECK(cd != AudioStreamInfo(24, 2, 44100));
    CHECK(cd != AudioStreamInfo(16, 1, 44100));
    CHECK(cd != AudioStreamInfo(16, 2, 48000));
    return true;
}

// ============================================================================
// TransferBuffer
// ============================================================================

static bool test_transfer_buffer() {
    TransferBuffer buf;
    CHECK(buf.allocate(64));
    CHECK_EQ(buf.capacity(), 64u);
    CHECK_EQ(buf.available(), 0u);
    CHECK_EQ(buf.free(), 64u);
    CHECK(buf.get_buffer_start() == buf.get_buffer_end());

    // Commit 10 bytes
    uint8_t pattern[10];
    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = static_cast<uint8_t>(i + 1);
    }
    std::memcpy(buf.get_buffer_end(), pattern, sizeof(pattern));
    buf.increase_length(sizeof(pattern));
    CHECK_EQ(buf.available(), 10u);
    CHECK_EQ(buf.free(), 54u);

    // Partial consume advances the start cursor without reclaiming space:
    // free() stays fixed until the buffer fully drains (no memmove compaction)
    buf.decrease_length(4);
    CHECK_EQ(buf.available(), 6u);
    CHECK_EQ(buf.free(), 54u);
    CHECK_EQ(buf.get_buffer_start()[0], 5);
    CHECK(std::memcmp(buf.get_buffer_start(), pattern + 4, 6) == 0);

    // Full drain resets the start pointer, reclaiming the whole allocation
    buf.decrease_length(6);
    CHECK_EQ(buf.available(), 0u);
    CHECK_EQ(buf.free(), 64u);

    // decrease_length clamps to the available length
    buf.increase_length(5);
    buf.decrease_length(100);
    CHECK_EQ(buf.available(), 0u);
    CHECK_EQ(buf.free(), 64u);

    // reallocate preserves unconsumed data and the start-cursor offset
    std::memcpy(buf.get_buffer_end(), pattern, sizeof(pattern));
    buf.increase_length(sizeof(pattern));
    buf.decrease_length(3);  // start cursor now at offset 3
    CHECK(buf.reallocate(128));
    CHECK_EQ(buf.capacity(), 128u);
    CHECK_EQ(buf.available(), 7u);
    CHECK(std::memcmp(buf.get_buffer_start(), pattern + 3, 7) == 0);
    CHECK_EQ(buf.free(), 128u - 3u - 7u);
    return true;
}

// ============================================================================
// RingBuffer (host SPSC implementation)
// ============================================================================

static bool test_ring_buffer_basic() {
    RingBuffer rb;
    CHECK(rb.create(64));
    CHECK_EQ(rb.available(), 0u);

    // Empty acquire with zero timeout returns no data immediately
    const uint8_t* data = nullptr;
    size_t len = 0;
    rb.receive_acquire(&data, &len, 64, 0);
    CHECK(data == nullptr);
    CHECK_EQ(len, 0u);

    // Simple write / acquire round trip
    uint8_t pattern[48];
    for (size_t i = 0; i < sizeof(pattern); i++) {
        pattern[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    CHECK_EQ(rb.write(pattern, 48, 100), 48u);
    CHECK_EQ(rb.available(), 48u);

    rb.receive_acquire(&data, &len, 64, 100);
    CHECK(data != nullptr);
    CHECK_EQ(len, 48u);
    CHECK(std::memcmp(data, pattern, 48) == 0);
    rb.receive_release();
    CHECK_EQ(rb.available(), 0u);

    // Oversized write is truncated to the free space
    const uint8_t big[100] = {0};
    CHECK_EQ(rb.write(big, 100, 100), 64u);
    CHECK_EQ(rb.available(), 64u);

    // Full buffer: zero-timeout write returns 0
    CHECK_EQ(rb.write(pattern, 1, 0), 0u);

    // Drain, possibly in two acquires (data may wrap)
    size_t drained = 0;
    while (drained < 64) {
        rb.receive_acquire(&data, &len, 64, 100);
        CHECK(data != nullptr);
        CHECK(len > 0);
        drained += len;
        rb.receive_release();
    }
    CHECK_EQ(drained, 64u);

    // Wrap-around: write offset sits at 48, so 40 bytes wrap. The two
    // acquires must reconstruct the pattern in order.
    CHECK_EQ(rb.write(pattern, 40, 100), 40u);
    size_t checked = 0;
    while (checked < 40) {
        rb.receive_acquire(&data, &len, 40 - checked, 100);
        CHECK(data != nullptr);
        CHECK(len > 0);
        CHECK(std::memcmp(data, pattern + checked, len) == 0);
        checked += len;
        rb.receive_release();
    }
    CHECK_EQ(checked, 40u);
    return true;
}

static bool test_ring_buffer_threaded() {
    // Producer/consumer stress across a small buffer to force many wraps.
    // Both sides generate the same LCG byte sequence; any dropped, duplicated,
    // or reordered byte breaks the comparison.
    static constexpr size_t TOTAL = 256 * 1024;
    static constexpr size_t RB_SIZE = 4096;

    RingBuffer rb;
    CHECK(rb.create(RB_SIZE));

    std::atomic<bool> producer_failed{false};
    std::thread producer([&]() {
        uint32_t lcg = 0xC0FFEE;
        std::vector<uint8_t> chunk;
        size_t sent = 0;
        uint32_t chunk_rng = 1;
        uint64_t deadline = now_ms() + 30000;
        while (sent < TOTAL && now_ms() < deadline) {
            chunk_rng = chunk_rng * 1103515245U + 12345U;
            size_t want = 1 + (chunk_rng >> 16) % 1500;
            want = std::min(want, TOTAL - sent);
            chunk.clear();
            for (size_t i = 0; i < want; i++) {
                lcg = lcg * 1103515245U + 12345U;
                chunk.push_back(static_cast<uint8_t>(lcg >> 16));
            }
            size_t off = 0;
            while (off < chunk.size() && now_ms() < deadline) {
                off += rb.write(chunk.data() + off, chunk.size() - off, 100);
            }
            sent += chunk.size();
        }
        if (sent != TOTAL) {
            producer_failed.store(true);
        }
    });

    uint32_t lcg = 0xC0FFEE;
    size_t received = 0;
    bool mismatch = false;
    uint64_t deadline = now_ms() + 30000;
    while (received < TOTAL && now_ms() < deadline) {
        const uint8_t* data = nullptr;
        size_t len = 0;
        rb.receive_acquire(&data, &len, 999, 100);
        if (data == nullptr) {
            continue;
        }
        for (size_t i = 0; i < len; i++) {
            lcg = lcg * 1103515245U + 12345U;
            if (data[i] != static_cast<uint8_t>(lcg >> 16)) {
                mismatch = true;
                break;
            }
        }
        received += len;
        rb.receive_release();
        if (mismatch) {
            break;
        }
    }
    producer.join();

    CHECK(!producer_failed.load());
    CHECK(!mismatch);
    CHECK_EQ(received, TOTAL);
    CHECK_EQ(rb.available(), 0u);
    return true;
}

// ============================================================================
// EventFlags (host implementation)
// ============================================================================

static bool test_event_flags() {
    EventFlags flags;
    CHECK(flags.create());
    CHECK_EQ(flags.get(), 0u);

    // set / get / clear
    flags.set(0x5);
    CHECK_EQ(flags.get(), 0x5u);
    flags.clear(0x4);
    CHECK_EQ(flags.get(), 0x1u);

    // Already-set bit: wait returns immediately without clearing
    CHECK_EQ(flags.wait(0x1, false, false, 1000) & 0x1u, 0x1u);
    CHECK_EQ(flags.get(), 0x1u);

    // clear_on_exit clears only the waited bits
    flags.set(0x2);
    uint32_t bits = flags.wait(0x2, false, true, 1000);
    CHECK_EQ(bits & 0x2u, 0x2u);
    CHECK_EQ(flags.get(), 0x1u);

    // wait-any returns when one of several bits is set
    flags.clear(0xFF);
    flags.set(0x8);
    CHECK_EQ(flags.wait(0xC, false, false, 1000) & 0xCu, 0x8u);

    // Timeout: an unset bit expires after ~timeout_ms with the bit still unset
    flags.clear(0xFF);
    uint64_t start = now_ms();
    bits = flags.wait(0x10, false, false, 50);
    uint64_t elapsed = now_ms() - start;
    CHECK_EQ(bits & 0x10u, 0u);
    CHECK(elapsed >= 40);
    CHECK(elapsed < 5000);

    // Zero timeout returns immediately
    start = now_ms();
    bits = flags.wait(0x10, false, false, 0);
    CHECK_EQ(bits & 0x10u, 0u);
    CHECK(now_ms() - start < 1000);

    // wait-all blocks until every bit is set (second bit arrives from
    // another thread) and clear_on_exit then clears both
    flags.clear(0xFF);
    flags.set(0x1);
    std::thread setter([&flags]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        flags.set(0x2);
    });
    bits = flags.wait(0x3, true, true, 10000);
    setter.join();
    CHECK_EQ(bits & 0x3u, 0x3u);
    CHECK_EQ(flags.get() & 0x3u, 0u);
    return true;
}

// ============================================================================
// DecoderSource: play_buffer() path
// ============================================================================

static bool test_play_buffer_validation() {
    std::vector<uint8_t> wav = make_wav(1, 16000, 256);

    // No listener: play_*() must fail immediately (regression: they used to
    // spawn threads that decoded into a null listener)
    {
        DecoderSource src;
        CHECK(!src.play_buffer(wav.data(), wav.size(), AudioFileType::WAV));
        CHECK(!src.play_url("http://127.0.0.1:9/nothing.mp3"));
        CHECK(src.state() == DecoderState::IDLE);
    }

    // Invalid arguments with a listener set: rejected synchronously, no state
    // change, no callbacks on a later loop()
    {
        CaptureListener listener;
        DecoderSource src;
        src.set_listener(&listener);
        CHECK(!src.play_buffer(nullptr, wav.size(), AudioFileType::WAV));
        CHECK(!src.play_buffer(wav.data(), 0, AudioFileType::WAV));
        CHECK(!src.play_buffer(wav.data(), wav.size(), AudioFileType::NONE));
        CHECK(src.state() == DecoderState::IDLE);
        for (int i = 0; i < 10; i++) {
            src.loop();
        }
        CHECK(listener.states.empty());
        CHECK_EQ(listener.pcm_size(), 0u);
    }
    return true;
}

static bool test_play_buffer_passthrough() {
    // WAV in, identical PCM out: exercises the full buffer-path pipeline
    // (decoder thread, output staging, listener delivery, state machine)
    struct Case {
        uint8_t channels;
        uint32_t sample_rate;
        size_t frames;
    };
    const Case cases[] = {
        {2, 44100, 4096},
        {1, 22050, 3333},  // odd frame count, mono
    };

    for (const Case& c : cases) {
        std::vector<uint8_t> payload;
        std::vector<uint8_t> wav = make_wav(c.channels, c.sample_rate, c.frames, &payload);

        CaptureListener listener;
        DecoderConfig config;
        config.audio_write_timeout_ms = 7;  // recognizable value, checked below
        DecoderSource src(config);
        src.set_listener(&listener);

        CHECK(src.play_buffer(wav.data(), wav.size(), AudioFileType::WAV));
        CHECK(pump_to_completion(src, listener));

        // Exactly one PLAYING then one IDLE, in order
        CHECK_EQ(listener.states.size(), 2u);
        CHECK(listener.states[0] == DecoderState::PLAYING);
        CHECK(listener.states[1] == DecoderState::IDLE);
        CHECK(src.state() == DecoderState::IDLE);

        // Stream info fired exactly once with the WAV header's format
        std::vector<AudioStreamInfo> infos = listener.infos();
        CHECK_EQ(infos.size(), 1u);
        CHECK(infos[0] == AudioStreamInfo(16, c.channels, c.sample_rate));

        // Bit-exact PCM pass-through
        std::vector<uint8_t> pcm = listener.pcm();
        CHECK_EQ(pcm.size(), payload.size());
        CHECK(pcm == payload);

        // on_audio_write() received the configured timeout
        CHECK_EQ(listener.last_write_timeout_ms(), 7u);
    }
    return true;
}

static bool test_play_buffer_backpressure() {
    // A listener that accepts only a sliver per call forces the decoder to
    // retry with the unconsumed remainder; output must still be bit-exact.
    std::vector<uint8_t> payload;
    std::vector<uint8_t> wav = make_wav(2, 48000, 8192, &payload);

    CaptureListener listener;
    listener.max_consume = 97;  // deliberately odd and small
    DecoderSource src;
    src.set_listener(&listener);

    CHECK(src.play_buffer(wav.data(), wav.size(), AudioFileType::WAV));
    CHECK(pump_to_completion(src, listener, 30000));

    CHECK(src.state() == DecoderState::IDLE);
    std::vector<uint8_t> pcm = listener.pcm();
    CHECK_EQ(pcm.size(), payload.size());
    CHECK(pcm == payload);
    return true;
}

static bool test_play_buffer_stop_deferred() {
    // stop() mid-playback: state() flips to IDLE synchronously, but the
    // on_state_change(IDLE) callback is deferred to the next loop() call.
    std::vector<uint8_t> payload;
    std::vector<uint8_t> wav = make_wav(2, 44100, 131072, &payload);  // 512 KiB of PCM

    CaptureListener listener;
    listener.max_consume = 2048;
    listener.consume_delay_ms = 1;  // keep playback alive while we stop it
    DecoderSource src;
    src.set_listener(&listener);

    CHECK(src.play_buffer(wav.data(), wav.size(), AudioFileType::WAV));
    CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::PLAYING); }));

    src.stop();
    CHECK(src.state() == DecoderState::IDLE);
    CHECK(!listener.saw_state(DecoderState::IDLE));  // callback not fired yet

    src.loop();
    CHECK_EQ(listener.states.size(), 2u);
    CHECK(listener.states[1] == DecoderState::IDLE);

    // Playback genuinely stopped early
    CHECK(listener.pcm_size() < payload.size());

    // Redundant stop() while already IDLE stays IDLE with no extra callback
    src.stop();
    src.loop();
    CHECK_EQ(listener.states.size(), 2u);
    return true;
}

static bool test_stop_from_callback() {
    // The header documents that calling stop() from on_state_change() is safe;
    // the re-entrancy guard must prevent deadlock or recursion.
    std::vector<uint8_t> wav = make_wav(2, 44100, 131072);

    CaptureListener listener;
    listener.max_consume = 2048;
    listener.consume_delay_ms = 1;
    DecoderSource src;
    src.set_listener(&listener);
    listener.on_state = [&src](DecoderState s) {
        if (s == DecoderState::PLAYING) {
            src.stop();  // re-entrant: called from inside loop()
        }
    };

    CHECK(src.play_buffer(wav.data(), wav.size(), AudioFileType::WAV));
    CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::IDLE); }));
    CHECK(src.state() == DecoderState::IDLE);
    CHECK_EQ(listener.states.size(), 2u);
    CHECK(listener.states[0] == DecoderState::PLAYING);
    CHECK(listener.states[1] == DecoderState::IDLE);
    return true;
}

static bool test_play_buffer_failure_and_reuse() {
    // One DecoderSource across several plays: success, garbage failure,
    // then success again. Each stream must get its own on_stream_info and
    // bit-exact PCM; the FAILED transition must be recoverable.
    CaptureListener listener;
    DecoderSource src;
    src.set_listener(&listener);

    std::vector<uint8_t> payload_a;
    std::vector<uint8_t> wav_a = make_wav(2, 44100, 2048, &payload_a, 0xAAAA1111);
    CHECK(src.play_buffer(wav_a.data(), wav_a.size(), AudioFileType::WAV));
    CHECK(pump_to_completion(src, listener));
    CHECK(src.state() == DecoderState::IDLE);
    CHECK(listener.infos().size() == 1 && listener.infos()[0] == AudioStreamInfo(16, 2, 44100));
    CHECK(listener.pcm() == payload_a);

    // Garbage that is not a RIFF file: the decoder gives up and reports FAILED
    listener.reset_capture();
    std::vector<uint8_t> garbage(8192, 0xA5);
    CHECK(src.play_buffer(garbage.data(), garbage.size(), AudioFileType::WAV));
    CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::FAILED); }));
    CHECK(src.state() == DecoderState::FAILED);
    CHECK_EQ(listener.pcm_size(), 0u);  // no audio invented from garbage

    // Reuse after FAILED: a different stream on the same source plays fine
    listener.reset_capture();
    std::vector<uint8_t> payload_b;
    std::vector<uint8_t> wav_b = make_wav(1, 8000, 1000, &payload_b, 0xBBBB2222);
    CHECK(src.play_buffer(wav_b.data(), wav_b.size(), AudioFileType::WAV));
    CHECK(pump_to_completion(src, listener));
    CHECK(src.state() == DecoderState::IDLE);
    std::vector<AudioStreamInfo> infos = listener.infos();
    CHECK_EQ(infos.size(), 1u);
    CHECK(infos[0] == AudioStreamInfo(16, 1, 8000));
    CHECK(listener.pcm() == payload_b);
    return true;
}

static bool test_destructor_while_playing() {
    // Destroying an actively playing DecoderSource must join its threads
    // cleanly (no crash, no leak -- run under sanitizers to verify).
    std::vector<uint8_t> wav = make_wav(2, 44100, 131072);

    CaptureListener listener;
    listener.max_consume = 2048;
    listener.consume_delay_ms = 1;
    {
        DecoderSource src;
        src.set_listener(&listener);
        CHECK(src.play_buffer(wav.data(), wav.size(), AudioFileType::WAV));
        CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::PLAYING); }));
        // ~DecoderSource() runs here, mid-decode
    }
    return true;
}

// ============================================================================
// DecoderSource: play_url() path
// ============================================================================

static bool test_play_url_stream() {
    // Full HTTP pipeline with the file type coming from Content-Type alone
    // (the URL path has no extension): PCM must arrive bit-exact and the
    // stream must end in IDLE.
    std::vector<uint8_t> payload;
    std::vector<uint8_t> wav = make_wav(2, 44100, 8192, &payload);

    TestHttpServer::Response resp;
    resp.content_type = "audio/wav";
    resp.body = wav;
    resp.chunk_size = 4096;  // stream in pieces so the ring buffer matters
    TestHttpServer server;
    CHECK(server.start(resp));

    CaptureListener listener;
    DecoderSource src;
    src.set_listener(&listener);
    CHECK(src.play_url(server.url("/stream")));
    CHECK(pump_to_completion(src, listener));

    CHECK(!listener.saw_state(DecoderState::FAILED));
    CHECK(src.state() == DecoderState::IDLE);
    std::vector<AudioStreamInfo> infos = listener.infos();
    CHECK_EQ(infos.size(), 1u);
    CHECK(infos[0] == AudioStreamInfo(16, 2, 44100));
    std::vector<uint8_t> pcm = listener.pcm();
    CHECK_EQ(pcm.size(), payload.size());
    CHECK(pcm == payload);

    src.stop();
    server.stop();
    return true;
}

static bool test_play_url_extension_fallback() {
    // No Content-Type header at all: the file type must come from the URL
    // extension.
    std::vector<uint8_t> payload;
    std::vector<uint8_t> wav = make_wav(1, 16000, 4000, &payload);

    TestHttpServer::Response resp;
    resp.content_type = "";  // omit the header entirely
    resp.body = wav;
    TestHttpServer server;
    CHECK(server.start(resp));

    CaptureListener listener;
    DecoderSource src;
    src.set_listener(&listener);
    CHECK(src.play_url(server.url("/tone.wav")));
    CHECK(pump_to_completion(src, listener));

    CHECK(!listener.saw_state(DecoderState::FAILED));
    CHECK(src.state() == DecoderState::IDLE);
    CHECK(listener.pcm() == payload);

    src.stop();
    server.stop();
    return true;
}

static bool test_play_url_error_states() {
    // Connection and detection failures must surface asynchronously as a
    // FAILED state via loop(), never hang, and never invent audio.

    // HTTP 404
    {
        TestHttpServer::Response resp;
        resp.status = "404 Not Found";
        resp.content_type = "text/plain";
        resp.body = {'n', 'o', 'p', 'e'};
        TestHttpServer server;
        CHECK(server.start(resp));

        CaptureListener listener;
        DecoderSource src;
        src.set_listener(&listener);
        CHECK(src.play_url(server.url("/gone.mp3")));
        CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::FAILED); }));
        CHECK(src.state() == DecoderState::FAILED);
        CHECK(!listener.saw_state(DecoderState::PLAYING));
        CHECK_EQ(listener.pcm_size(), 0u);
        src.stop();
        server.stop();
    }

    // M3U playlist: served successfully but undecodable, so the reader must
    // reject it (regression: audio/mpegurl was misdetected as MP3 and the
    // playlist text was fed to the MP3 decoder)
    {
        const char* m3u = "#EXTM3U\nhttp://radio.example/real-stream\n";
        TestHttpServer::Response resp;
        resp.content_type = "audio/mpegurl";
        resp.body.assign(m3u, m3u + std::strlen(m3u));
        TestHttpServer server;
        CHECK(server.start(resp));

        CaptureListener listener;
        DecoderSource src;
        src.set_listener(&listener);
        CHECK(src.play_url(server.url("/listen.m3u")));
        CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::FAILED); }));
        CHECK(src.state() == DecoderState::FAILED);
        CHECK(!listener.saw_state(DecoderState::PLAYING));
        CHECK_EQ(listener.pcm_size(), 0u);
        src.stop();
        server.stop();
    }

    // Connection refused (server stopped; nothing listens on the port)
    {
        TestHttpServer server;
        CHECK(server.start(TestHttpServer::Response{}));
        std::string dead_url = server.url("/song.wav");
        server.stop();

        CaptureListener listener;
        DecoderSource src;
        src.set_listener(&listener);
        CHECK(src.play_url(dead_url));
        CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::FAILED); }));
        CHECK(src.state() == DecoderState::FAILED);
        src.stop();
    }
    return true;
}

static bool test_play_url_stop_mid_stream() {
    // Endless live stream: stop() must interrupt both threads promptly and
    // the reader must close its HTTP connection.
    TestHttpServer::Response resp;
    resp.content_type = "audio/wav";
    resp.body = make_wav_header(2, 44100, 0x7FFFFFF0);  // claims a ~2 GiB payload
    resp.infinite_pattern = make_pcm(2, 512);
    resp.chunk_delay_ms = 1;
    TestHttpServer server;
    CHECK(server.start(resp));

    CaptureListener listener;
    DecoderSource src;
    src.set_listener(&listener);
    CHECK(src.play_url(server.url("/live")));
    CHECK(pump_until(src, [&]() {
        return listener.saw_state(DecoderState::PLAYING) && listener.pcm_size() > 0;
    }));

    uint64_t start = now_ms();
    src.stop();
    uint64_t stop_duration = now_ms() - start;
    CHECK(src.state() == DecoderState::IDLE);
    CHECK(stop_duration < 5000);

    src.loop();
    CHECK(listener.saw_state(DecoderState::IDLE));

    // The reader hung up on the server
    uint64_t deadline = now_ms() + 5000;
    while (!server.client_disconnected() && now_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(server.client_disconnected());
    server.stop();
    return true;
}

static bool test_play_url_reader_stops_on_decoder_exit() {
    // Endless stream of undecodable bytes: the decoder thread gives up and
    // exits, and the reader must notice and stop downloading (regression:
    // the reader kept streaming into a ring buffer nobody drained until the
    // stream ended or stop() was called).
    TestHttpServer::Response resp;
    resp.content_type = "audio/wav";
    resp.infinite_pattern.assign(1024, 0xA5);  // never a RIFF header
    resp.chunk_delay_ms = 1;
    TestHttpServer server;
    CHECK(server.start(resp));

    CaptureListener listener;
    DecoderSource src;
    src.set_listener(&listener);
    CHECK(src.play_url(server.url("/live")));
    CHECK(pump_until(src, [&]() { return listener.saw_state(DecoderState::FAILED); }));
    CHECK(src.state() == DecoderState::FAILED);
    CHECK_EQ(listener.pcm_size(), 0u);

    // Without any stop() call, the reader must hang up on its own
    uint64_t deadline = now_ms() + 10000;
    while (!server.client_disconnected() && now_ms() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(server.client_disconnected());

    src.stop();
    server.stop();
    return true;
}

// ============================================================================
// Runner
// ============================================================================

struct TestCase {
    const char* name;
    bool (*fn)();
};

static const TestCase TESTS[] = {
    {"detect_content_type", test_detect_content_type},
    {"detect_url_extension", test_detect_url_extension},
    {"stream_info", test_stream_info},
    {"transfer_buffer", test_transfer_buffer},
    {"ring_buffer_basic", test_ring_buffer_basic},
    {"ring_buffer_threaded", test_ring_buffer_threaded},
    {"event_flags", test_event_flags},
    {"play_buffer_validation", test_play_buffer_validation},
    {"play_buffer_passthrough", test_play_buffer_passthrough},
    {"play_buffer_backpressure", test_play_buffer_backpressure},
    {"play_buffer_stop_deferred", test_play_buffer_stop_deferred},
    {"stop_from_callback", test_stop_from_callback},
    {"play_buffer_failure_and_reuse", test_play_buffer_failure_and_reuse},
    {"destructor_while_playing", test_destructor_while_playing},
    {"play_url_stream", test_play_url_stream},
    {"play_url_extension_fallback", test_play_url_extension_fallback},
    {"play_url_error_states", test_play_url_error_states},
    {"play_url_stop_mid_stream", test_play_url_stop_mid_stream},
    {"play_url_reader_stops_on_decoder_exit", test_play_url_reader_stops_on_decoder_exit},
};

int main(int argc, char* argv[]) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;
    set_log_level(LOG_LEVEL_ERROR);  // keep expected-failure noise out of test output

    // libcurl does not special-case loopback: with http_proxy or ALL_PROXY set it sends
    // the play_url() requests to the proxy instead of the local fixture, which fails five
    // tests for reasons that look like library bugs. Done here rather than as a CTest
    // property so it also covers running this binary directly. Safe before any test
    // spawns a thread; libcurl reads the proxy environment per transfer.
    setenv("no_proxy", "*", 1);

    int ran = 0;
    int failed = 0;
    for (const TestCase& t : TESTS) {
        if (filter && std::strcmp(filter, t.name) != 0) {
            continue;
        }
        ran++;
        std::printf("[ RUN  ] %s\n", t.name);
        bool ok = t.fn();
        std::printf("[ %s ] %s\n", ok ? "PASS" : "FAIL", t.name);
        if (!ok) {
            failed++;
        }
    }
    if (ran == 0) {
        std::fprintf(stderr, "no test matches '%s'\n", filter);
        return 2;
    }
    std::printf("%d/%d tests passed\n", ran - failed, ran);
    return failed == 0 ? 0 : 1;
}
