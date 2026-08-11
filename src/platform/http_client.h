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

/// @file http_client.h
/// @brief Abstract HTTP client interface used by AudioReader

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace micro_decoder {

/// @brief Metadata returned after a successful HTTP connection
struct HttpResponse {
    int status_code{0};
    std::string content_type;
};

/// @brief Polled while open() blocks; returns true to abandon the request
/// @note Runs on the thread that called open(), and must not block.
/// @param context The cancel_context supplied in HttpRequest
using HttpCancelCheck = bool (*)(void* context);

/**
 * @brief Everything an HttpClient needs to open a streaming request
 *
 * @note The two timeouts are separate because the phases want different bounds. Connecting
 * and collecting headers tolerates a long wait, while a body read must return often enough
 * for the caller to notice a stop request between reads.
 */
struct HttpRequest {
    /// @brief HTTP or HTTPS URL of the stream
    std::string url;

    /// @brief User-Agent header value; empty uses the platform client's default
    std::string user_agent;

    /// @brief PEM-encoded CA certificate(s) used to verify HTTPS servers
    /// Empty falls back to the platform default trust store (certificate bundle on ESP-IDF,
    /// system trust store on host). Ignored for plain HTTP.
    std::string ca_certificate;

    /// @brief Polled while open() blocks; nullptr disables cancellation
    HttpCancelCheck cancel_check{nullptr};

    /// @brief Opaque argument passed to cancel_check
    void* cancel_context{nullptr};

    /// @brief Size of the platform HTTP receive buffer in bytes (ESP-IDF only)
    size_t rx_buffer_size{2048};  // NOLINT(readability-magic-numbers)

    /// @brief Total budget for connecting and fetching headers, in milliseconds
    uint32_t connect_timeout_ms{5000};  // NOLINT(readability-magic-numbers)

    /// @brief Maximum time a single socket read may block, in milliseconds
    uint32_t read_timeout_ms{250};  // NOLINT(readability-magic-numbers)
};

/**
 * @brief Abstract streaming HTTP client
 *
 * Implementations live in src/esp/http_client.cpp and src/host/http_client.cpp.
 *
 * Typical usage:
 *   1. Call open() with a fully populated HttpRequest.
 *   2. Call response_info() to inspect the HTTP status and Content-Type.
 *   3. Loop calling read() until is_complete() returns true.
 *   4. Call close() to release resources.
 *
 * @code
 *   HttpRequest request;
 *   request.url = url;
 *   client.open(request);
 *   auto info = client.response_info();
 *   while (!client.is_complete()) {
 *       int n = client.read(buf, sizeof(buf));
 *       if (n < 0) { break; }  // error
 *       if (n == 0) { continue; }  // no data yet, loop until is_complete()
 *   }
 *   client.close();
 * @endcode
 */
class HttpClient {
public:
    virtual ~HttpClient() = default;

    /// @brief Opens the URL and fetches headers
    /// Blocks until the headers arrive, the request fails, request.cancel_check returns
    /// true, or request.connect_timeout_ms elapses. Redirects are followed internally.
    /// @note The read timeout applies from the first header read onward, so implementations
    /// that block do so in slices bounded by request.read_timeout_ms.
    /// @param request Connection settings, timeouts, and cancellation hook
    /// @return true on success (2xx status)
    virtual bool open(const HttpRequest& request) = 0;

    /// @brief Returns response metadata (status code, Content-Type header)
    /// @note Valid after a successful open() and before close()
    /// @return Reference to the HttpResponse with the HTTP status code and Content-Type string
    virtual const HttpResponse& response_info() const = 0;

    /// @brief Reads up to max_length bytes of body data into buffer
    /// @param[out] buffer      Destination buffer for received bytes
    /// @param max_length       Maximum number of bytes to read
    /// @return Bytes read (>0), 0 when no data is available (check is_complete() for EOF), -1 on
    /// error
    virtual int read(uint8_t* buffer, size_t max_length) = 0;

    /// @brief Returns true when all body data has been received
    /// @return true when all body data has been received, false otherwise
    virtual bool is_complete() const = 0;

    /// @brief Closes the connection and frees resources
    virtual void close() = 0;
};

/// @brief Creates a platform-specific HttpClient instance
/// @note Defined in src/esp/http_client.cpp or src/host/http_client.cpp.
/// @return Owned HttpClient instance
std::unique_ptr<HttpClient> create_http_client();

}  // namespace micro_decoder
