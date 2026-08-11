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

/// @file thread.h
/// @brief Platform-abstracted worker thread with a failure-returning start()
///
/// Backed by pthreads on both ESP-IDF and host. std::thread is deliberately avoided:
/// its constructor throws on task-creation failure, and ESP-IDF builds routinely
/// disable exceptions (CONFIG_COMPILER_CXX_EXCEPTIONS unset), which turns a
/// recoverable out-of-memory condition into an abort() and a device reboot.

#pragma once

#include "logging.h"
#include <pthread.h>

#include <cstddef>

#ifdef ESP_PLATFORM

#include <esp_heap_caps.h>
#include <esp_pthread.h>

#endif  // ESP_PLATFORM

namespace micro_decoder {

// ============================================================================
// ThreadConfig
// ============================================================================

/// @brief Creation-time settings for a PlatformThread
/// @note Every field except name is ignored on host, where the OS defaults are used.
struct ThreadConfig {
    /// @brief Thread name, used for FreeRTOS task naming (ESP-IDF only)
    const char* name{nullptr};

    /// @brief Stack size in bytes (ESP-IDF only)
    size_t stack_size{0};

    /// @brief FreeRTOS priority (ESP-IDF only)
    int priority{0};

    /// @brief Allocate the stack in SPIRAM instead of internal RAM (ESP-IDF only)
    bool stack_in_psram{false};
};

// ============================================================================
// PlatformThread
// ============================================================================

/**
 * @brief Joinable worker thread that reports creation failure instead of aborting
 *
 * Usage:
 * 1. Call start() with the thread settings, entry point, and argument.
 * 2. Check the return value; on false, no thread was created.
 * 3. Call join() before destroying the object or calling start() again.
 *
 * @code
 * PlatformThread thread;
 * if (thread.start({"worker", 4096, 2, false}, &worker_entry, this)) {
 *     thread.join();
 * }
 * @endcode
 */
class PlatformThread {
public:
    PlatformThread() = default;

    /// @brief Destroys the object; the thread must already have been joined
    ~PlatformThread() = default;

    PlatformThread(const PlatformThread&) = delete;
    PlatformThread& operator=(const PlatformThread&) = delete;

    /// @brief Creates and starts the thread
    /// On ESP-IDF, applies config via esp_pthread_set_cfg() and restores the calling
    /// thread's previous configuration afterwards, so the caller's environment is left
    /// exactly as it was found.
    /// @note Never aborts on failure, unlike constructing a std::thread.
    /// @param config Creation-time thread settings
    /// @param entry Thread entry point
    /// @param arg Opaque argument passed to entry
    /// @return true if the thread was created; false if a thread is already running on
    /// this object or the platform could not create one
    bool start(const ThreadConfig& config, void* (*entry)(void*), void* arg) {
        if (this->started_) {
            return false;
        }

        if (!this->apply_thread_config(config)) {
            return false;
        }

        int err = pthread_create(&this->handle_, nullptr, entry, arg);

        this->restore_thread_config();

        if (err != 0) {
            MD_LOGE("micro_decoder.thread", "Failed to create thread '%s': error %d",
                    config.name != nullptr ? config.name : "?", err);
            return false;
        }

        this->started_ = true;
        return true;
    }

    /// @brief Returns whether a started thread is still waiting to be joined
    /// @return true if join() must still be called
    bool joinable() const {
        return this->started_;
    }

    /// @brief Waits for the thread to finish; a no-op if it was never started
    void join() {
        if (!this->started_) {
            return;
        }
        pthread_join(this->handle_, nullptr);
        this->started_ = false;
    }

private:
#ifdef ESP_PLATFORM

    /// @brief Applies config to the calling thread, saving whatever was configured before
    /// @param config Creation-time thread settings
    /// @return true if the configuration was accepted by esp_pthread_set_cfg()
    bool apply_thread_config(const ThreadConfig& config) {
        this->had_previous_cfg_ = (esp_pthread_get_cfg(&this->previous_cfg_) == ESP_OK);

        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.stack_size = config.stack_size;
        cfg.prio = config.priority;
        cfg.thread_name = config.name;
        if (config.stack_in_psram) {
            cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
        }

        esp_err_t err = esp_pthread_set_cfg(&cfg);
        if (err != ESP_OK) {
            MD_LOGE("micro_decoder.thread", "Rejected thread config for '%s': %s",
                    config.name != nullptr ? config.name : "?", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    /// @brief Restores the pthread configuration the calling thread had before start()
    /// esp_pthread_set_cfg() applies to every subsequent pthread_create() on the calling
    /// thread, so leaving our settings in place would silently change the stack size,
    /// priority, task name, and stack memory capabilities of unrelated threads the caller
    /// creates later. When no configuration was set before, restoring the defaults is
    /// equivalent: an unset configuration is what esp_pthread_get_default_config() returns.
    void restore_thread_config() {
        esp_pthread_cfg_t restored =
            this->had_previous_cfg_ ? this->previous_cfg_ : esp_pthread_get_default_config();
        esp_err_t err = esp_pthread_set_cfg(&restored);
        if (err != ESP_OK) {
            MD_LOGW("micro_decoder.thread", "Failed to restore caller thread config: %s",
                    esp_err_to_name(err));
        }
    }

#else  // Host

    /// @brief No-op on host; threads use the OS-default stack and scheduling
    /// @param config Creation-time thread settings (unused)
    /// @return Always true
    bool apply_thread_config(const ThreadConfig& /*config*/) {
        return true;
    }

    /// @brief No-op on host; nothing global is modified by apply_thread_config()
    void restore_thread_config() {}

#endif  // ESP_PLATFORM

    // Struct fields
#ifdef ESP_PLATFORM
    esp_pthread_cfg_t previous_cfg_{};
#endif

    // Pointer fields
    pthread_t handle_{};

    // 8-bit fields
#ifdef ESP_PLATFORM
    bool had_previous_cfg_{false};
#endif
    bool started_{false};
};

}  // namespace micro_decoder
