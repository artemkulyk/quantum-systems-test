#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace udp_sender {

/**
 * UDP sender with immediate and scheduled (delayed / periodic) sends.
 *
 * Notes:
 * - Scheduling resolution is 1 second (tick-based).
 * - Scheduled sends are executed by a single worker thread.
 * - send_after/send_every do not report delivery success synchronously.
 *   Use set_error_callback() to observe send failures from the worker thread.
 */
class UdpSender final {
 public:
  using TaskId = std::uint64_t;

  /**
   * Called when a scheduled send (send_after/send_every) fails.
   *
   * - Invoked from the worker thread.
   * - err is an errno value (EHOSTUNREACH, ENETUNREACH, EAGAIN, ...).
   * - user is the pointer passed to set_error_callback().
   *
   * Requirements for callback:
   * - Must be fast and non-blocking where possible.
   * - Must not call back into this UdpSender instance (avoid re-entrancy).
   */
  using ErrorCallback = void (*)(TaskId id, int err, void* user);

  /**
   * Create sockets, initialize timing base, start worker thread.
   *
   * Throws std::runtime_error on socket/fcntl failures.
   */
  UdpSender();

  /**
   * Stop worker thread and release resources.
   *
   * Blocks until the worker thread exits.
   * Any pending scheduled tasks are discarded.
   */
  ~UdpSender();

  UdpSender(const UdpSender&) = delete;
  UdpSender& operator=(const UdpSender&) = delete;

  /**
   * Configure optional error callback for scheduled sends.
   *
   * Thread-safe.
   * Callback is invoked without holding internal locks.
   * Passing nullptr disables callbacks.
   */
  void set_error_callback(ErrorCallback cb, void* user = nullptr) noexcept {
    err_cb_.store(cb, std::memory_order_release);
    err_cb_user_.store(user, std::memory_order_release);
  }

  /**
   * Send a UDP packet immediately.
   *
   * Returns true on success, false on failure.
   * If out_errno != nullptr:
   * - set to 0 on success
   * - set to errno (or EINVAL on parse failure) on error
   */
  bool send_now(const std::string& ip, std::uint16_t port,
                const void* data, std::size_t size,
                int* out_errno = nullptr);

  /**
   * Convenience overload for vector payload.
   *
   * Equivalent to send_now(ip, port, payload.data(), payload.size(), ...).
   */
  bool send_now(const std::string& ip, std::uint16_t port,
                const std::vector<std::uint8_t>& payload,
                int* out_errno = nullptr) {
    return send_now(ip, port, payload.data(), payload.size(), out_errno);
  }

  /**
   * Schedule a single send after delay_seconds.
   *
   * delay_seconds must be in [1, 255].
   * Returns TaskId used for cancellation.
   *
   * Throws std::invalid_argument if delay_seconds is out of range.
   */
  TaskId send_after(std::uint8_t delay_seconds,
                    const std::string& ip,
                    std::uint16_t port,
                    std::vector<std::uint8_t> payload);

  /**
   * Schedule periodic sends every interval_seconds.
   *
   * interval_seconds must be in [1, 255].
   * First send occurs after interval_seconds.
   *
   * Returns TaskId used for cancellation.
   * Throws std::invalid_argument if interval_seconds is out of range.
   */
  TaskId send_every(std::uint8_t interval_seconds,
                    const std::string& ip,
                    std::uint16_t port,
                    std::vector<std::uint8_t> payload);

  /**
   * Cancel a scheduled task by id.
   *
   * Returns true if the task existed (and cancellation was applied),
   * false if id was unknown.
   */
  bool cancel(TaskId id);

 private:
  // Destination is kept as text IP + port to keep Task self-contained.
  // (We parse into sockaddr on actual send.)
  struct Dest {
    std::string ip;
    std::uint16_t port;
  };

  struct Task {
    TaskId id{};
    std::uint64_t due_tick{};          // absolute tick (seconds since base_)
    std::uint8_t interval_seconds{0};  // 0 = one-shot, >0 = periodic
    Dest dest{};
    std::vector<std::uint8_t> payload{};

    // Timing wheel bookkeeping (protected by mtx_)
    std::size_t bucket_idx{};
    std::list<TaskId>::iterator bucket_it{};
    bool in_wheel{false};

    // Cancellation flag:
    // - set from cancel()
    // - checked by worker_loop()
    std::atomic<bool> canceled{false};
  };

  // Worker main loop: processes ticks and executes due tasks.
  void worker_loop();

  // Low-level send implementation used by both immediate and scheduled sends.
  bool send_now_impl(const Dest& d, const void* data,
                     std::size_t size, int* out_errno);

  static void validate_seconds(std::uint8_t seconds);

 private:
  // Wheel size matches the [1..255] API range, plus one.
  static constexpr std::size_t kWheelSize = 256;

  // Sockets (created as non-blocking in the .cpp).
  int sock4_fd_{-1};
  int sock6_fd_{-1};

  // Lifetime control.
  std::atomic<bool> stop_{false};
  std::thread worker_;

  // Protects wheel_ and tasks_.
  std::mutex mtx_;
  std::condition_variable cv_;

  // Timing wheel: each bucket holds task ids due on that modulo slot.
  std::vector<std::list<TaskId>> wheel_;

  // Task storage and id lookup.
  std::unordered_map<TaskId, std::unique_ptr<Task>> tasks_;

  // Tick base (steady clock) and id generator.
  std::chrono::steady_clock::time_point base_{};
  std::atomic<TaskId> next_id_{1};

  // Error callback for scheduled sends (read by worker thread).
  std::atomic<ErrorCallback> err_cb_{nullptr};
  std::atomic<void*> err_cb_user_{nullptr};
};

}  // namespace udp_sender
