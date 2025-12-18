#include "udp_sender/UdpSender.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace udp_sender {

namespace {

/**
 * Minimal RAII wrapper for a POSIX file descriptor.
 *
 * Used only during construction paths to ensure no leaks on exceptions.
 */
class unique_fd {
 public:
  explicit unique_fd(int fd = -1) noexcept : fd_(fd) {}
  ~unique_fd() { reset(); }

  unique_fd(const unique_fd&) = delete;
  unique_fd& operator=(const unique_fd&) = delete;

  unique_fd(unique_fd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  unique_fd& operator=(unique_fd&& other) noexcept {
    if (this != &other) {
      reset();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }

  int release() noexcept {
    int tmp = fd_;
    fd_ = -1;
    return tmp;
  }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = fd;
  }

 private:
  int fd_;
};

/**
 * Parse textual IP (v4 or v6) and port into sockaddr_storage.
 *
 * Returns true on success and writes out_addr/out_len.
 * Returns false if ip is not a valid numeric IPv4/IPv6 address.
 */
bool parse_ip_port(const std::string& ip,
                   std::uint16_t port,
                   sockaddr_storage& out_addr,
                   socklen_t& out_len) {
  std::memset(&out_addr, 0, sizeof(out_addr));

  // Try IPv6 first.
  sockaddr_in6 a6{};
  a6.sin6_family = AF_INET6;
  a6.sin6_port = htons(port);
  if (inet_pton(AF_INET6, ip.c_str(), &a6.sin6_addr) == 1) {
    std::memcpy(&out_addr, &a6, sizeof(a6));
    out_len = sizeof(a6);
    return true;
  }

  // Then IPv4.
  sockaddr_in a4{};
  a4.sin_family = AF_INET;
  a4.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &a4.sin_addr) == 1) {
    std::memcpy(&out_addr, &a4, sizeof(a4));
    out_len = sizeof(a4);
    return true;
  }

  return false;
}

/**
 * Mark fd as non-blocking.
 *
 * Rationale: avoid rare shutdown stalls and keep worker responsive.
 * Note: for UDP, sendto() typically does not block, but non-blocking makes
 * behavior explicit and prevents edge cases (buffer pressure, etc.).
 */
void set_nonblocking(int fd) {
  int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) throw std::runtime_error("fcntl(F_GETFL) failed");
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    throw std::runtime_error("fcntl(F_SETFL, O_NONBLOCK) failed");
  }
}

int create_udp4_socket() {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error("Failed to create IPv4 UDP socket");
  set_nonblocking(fd);
  return fd;
}

int create_udp6_socket() {
  int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) throw std::runtime_error("Failed to create IPv6 UDP socket");
  set_nonblocking(fd);
  return fd;
}

/**
 * Return integer "tick" count in seconds since base.
 *
 * Scheduling resolution is 1 second.
 */
std::uint64_t tick_now(const std::chrono::steady_clock::time_point& base) {
  const auto now = std::chrono::steady_clock::now();
  const auto secs =
      std::chrono::duration_cast<std::chrono::seconds>(now - base).count();
  return secs < 0 ? 0ULL : static_cast<std::uint64_t>(secs);
}

}  // unnamed namespace

void UdpSender::validate_seconds(std::uint8_t seconds) {
  if (seconds < 1 || seconds > 255) {
    throw std::invalid_argument("seconds must be in [1, 255]");
  }
}

UdpSender::UdpSender() : wheel_(kWheelSize) {
  // Use RAII during setup in case something throws.
  unique_fd fd4(create_udp4_socket());
  unique_fd fd6(create_udp6_socket());
  sock4_fd_ = fd4.release();
  sock6_fd_ = fd6.release();

  base_ = std::chrono::steady_clock::now();

  // Starts worker thread. Object is fully constructed at this point.
  worker_ = std::thread([this] { worker_loop(); });
}

UdpSender::~UdpSender() {
  // Stop worker first, then clear state and close sockets.
  stop_.store(true, std::memory_order_relaxed);
  cv_.notify_all();

  if (worker_.joinable()) worker_.join();

  {
    std::lock_guard<std::mutex> lk(mtx_);
    wheel_.clear();
    tasks_.clear();
  }

  if (sock4_fd_ >= 0) {
    ::close(sock4_fd_);
    sock4_fd_ = -1;
  }
  if (sock6_fd_ >= 0) {
    ::close(sock6_fd_);
    sock6_fd_ = -1;
  }
}

bool UdpSender::send_now_impl(const Dest& d,
                              const void* data,
                              std::size_t size,
                              int* out_errno) {
  if (out_errno) *out_errno = 0;

  sockaddr_storage addr{};
  socklen_t len = 0;
  if (!parse_ip_port(d.ip, d.port, addr, len)) {
    if (out_errno) *out_errno = EINVAL;
    return false;
  }

  const auto fam = reinterpret_cast<const sockaddr*>(&addr)->sa_family;
  int fd = -1;
  if (fam == AF_INET) {
    fd = sock4_fd_;
  } else if (fam == AF_INET6) {
    fd = sock6_fd_;
  } else {
    if (out_errno) *out_errno = EINVAL;
    return false;
  }

  // Note: for UDP, a "successful" sendto() only means the datagram was queued
  // to the local stack. Delivery is not guaranteed.
  if (::sendto(fd, data, size, 0,
               reinterpret_cast<const sockaddr*>(&addr), len) < 0) {
    if (out_errno) *out_errno = errno;
    return false;
  }

  return true;
}

bool UdpSender::send_now(const std::string& ip,
                         std::uint16_t port,
                         const void* data,
                         std::size_t size,
                         int* out_errno) {
  return send_now_impl(Dest{ip, port}, data, size, out_errno);
}

UdpSender::TaskId UdpSender::send_after(std::uint8_t delay_seconds,
                                        const std::string& ip,
                                        std::uint16_t port,
                                        std::vector<std::uint8_t> payload) {
  validate_seconds(delay_seconds);

  const TaskId id = next_id_.fetch_add(1, std::memory_order_relaxed);

  const std::uint64_t now_tick = tick_now(base_);
  const std::uint64_t due = now_tick + delay_seconds;
  const std::size_t idx = static_cast<std::size_t>(due % kWheelSize);

  auto task = std::make_unique<Task>();
  task->id = id;
  task->due_tick = due;
  task->interval_seconds = 0;
  task->dest = Dest{ip, port};
  task->payload = std::move(payload);
  task->bucket_idx = idx;
  task->in_wheel = false;
  task->canceled.store(false, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& bucket = wheel_[idx];
    bucket.push_back(id);
    task->bucket_it = std::prev(bucket.end());
    task->in_wheel = true;
    tasks_.emplace(id, std::move(task));
  }

  cv_.notify_all();
  return id;
}

UdpSender::TaskId UdpSender::send_every(std::uint8_t interval_seconds,
                                        const std::string& ip,
                                        std::uint16_t port,
                                        std::vector<std::uint8_t> payload) {
  validate_seconds(interval_seconds);

  const TaskId id = next_id_.fetch_add(1, std::memory_order_relaxed);

  const std::uint64_t now_tick = tick_now(base_);
  const std::uint64_t due = now_tick + interval_seconds;
  const std::size_t idx = static_cast<std::size_t>(due % kWheelSize);

  auto task = std::make_unique<Task>();
  task->id = id;
  task->due_tick = due;
  task->interval_seconds = interval_seconds;
  task->dest = Dest{ip, port};
  task->payload = std::move(payload);
  task->bucket_idx = idx;
  task->in_wheel = false;
  task->canceled.store(false, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& bucket = wheel_[idx];
    bucket.push_back(id);
    task->bucket_it = std::prev(bucket.end());
    task->in_wheel = true;
    tasks_.emplace(id, std::move(task));
  }

  cv_.notify_all();
  return id;
}

bool UdpSender::cancel(TaskId id) {
  {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return false;

    Task& t = *it->second;
    t.canceled.store(true, std::memory_order_relaxed);

    // If still queued in the wheel, remove it so worker won't pop it later.
    if (t.in_wheel) {
      wheel_[t.bucket_idx].erase(t.bucket_it);
      t.in_wheel = false;
    }
  }

  cv_.notify_all();
  return true;
}

void UdpSender::worker_loop() {
  std::unique_lock<std::mutex> lk(mtx_);
  std::uint64_t last_processed = tick_now(base_);

  while (!stop_.load(std::memory_order_relaxed)) {
    const std::uint64_t now_tick = tick_now(base_);

    // If the process was stalled for a long time, avoid iterating a huge gap.
    // This drops very old ticks and keeps bounded work per wakeup.
    if (now_tick > last_processed &&
        (now_tick - last_processed) > kWheelSize) {
      last_processed = now_tick - kWheelSize;
    }

    while (last_processed < now_tick &&
           !stop_.load(std::memory_order_relaxed)) {
      ++last_processed;
      const std::size_t idx =
          static_cast<std::size_t>(last_processed % kWheelSize);
      auto& bucket = wheel_[idx];

      // Only process entries that were present at the start of this tick.
      const std::size_t n = bucket.size();
      for (std::size_t i = 0; i < n; ++i) {
        const TaskId id = bucket.front();
        bucket.pop_front();

        auto it = tasks_.find(id);
        if (it == tasks_.end()) continue;

        Task& t = *it->second;
        t.in_wheel = false;

        // If this id landed in the same modulo bucket but its due_tick differs,
        // reinsert it into its actual due bucket.
        if (t.due_tick != last_processed) {
          const std::size_t due_idx =
              static_cast<std::size_t>(t.due_tick % kWheelSize);
          auto& b = wheel_[due_idx];
          b.push_back(id);
          t.bucket_idx = due_idx;
          t.bucket_it = std::prev(b.end());
          t.in_wheel = true;
          continue;
        }

        // Canceled tasks are removed by the worker to avoid races with send.
        if (t.canceled.load(std::memory_order_relaxed)) {
          tasks_.erase(it);
          continue;
        }

        // Copy everything needed for sending so we can release the lock.
        const Dest dest = t.dest;
        const std::vector<std::uint8_t> payload = t.payload;
        const std::uint8_t interval = t.interval_seconds;

        // Snapshot callback pointers (worker uses these for this send attempt).
        const auto cb = err_cb_.load(std::memory_order_acquire);
        void* user = err_cb_user_.load(std::memory_order_acquire);

        // Send without internal lock.
        lk.unlock();
        int send_err = 0;
        const bool ok = send_now_impl(dest, payload.data(), payload.size(), &send_err);
        lk.lock();

        // Report failure (outside lock).
        if (!ok && cb) {
          lk.unlock();
          cb(id, send_err, user);
          lk.lock();
        }

        if (stop_.load(std::memory_order_relaxed)) break;

        // Re-find task after unlock to avoid stale references.
        it = tasks_.find(id);
        if (it == tasks_.end()) continue;

        Task& t2 = *it->second;

        if (t2.canceled.load(std::memory_order_relaxed)) {
          tasks_.erase(it);
          continue;
        }

        if (interval > 0) {
          // Periodic: schedule next run.
          t2.due_tick = last_processed + interval;
          const std::size_t new_idx =
              static_cast<std::size_t>(t2.due_tick % kWheelSize);
          auto& nb = wheel_[new_idx];
          nb.push_back(id);
          t2.bucket_idx = new_idx;
          t2.bucket_it = std::prev(nb.end());
          t2.in_wheel = true;
        } else {
          // One-shot: remove after execution.
          tasks_.erase(it);
        }
      }
    }

    if (stop_.load(std::memory_order_relaxed)) break;

    // Sleep until next second boundary, but allow early wakeups via notify_all().
    const auto next_tp = base_ + std::chrono::seconds(last_processed + 1);
    cv_.wait_until(lk, next_tp);
  }
}

}  // namespace udp_sender
