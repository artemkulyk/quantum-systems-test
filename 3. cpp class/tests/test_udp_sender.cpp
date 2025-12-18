#include "udp_sender/UdpSender.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * Create an IPv4 UDP socket bound to 127.0.0.1:0 (ephemeral port).
 * Returns fd and writes chosen port into out_port.
 */
int make_server_socket_v4(uint16_t& out_port) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  assert(fd >= 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(0);

  int rc = ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  assert(rc == 0);

  socklen_t len = sizeof(addr);
  rc = ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  assert(rc == 0);

  out_port = ntohs(addr.sin_port);
  return fd;
}

/**
 * Create an IPv6 UDP socket bound to ::1:0 (ephemeral port).
 * Forces IPV6_V6ONLY to exercise IPv6 path.
 * Returns fd or -1 if IPv6 is not available in the environment.
 */
int make_server_socket_v6(uint16_t& out_port) {
  int fd = ::socket(AF_INET6, SOCK_DGRAM, 0);
  if (fd < 0) return -1;

  int v6only = 1;
  (void)::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_loopback;
  addr.sin6_port = htons(0);

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }

  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return -1;
  }

  out_port = ntohs(addr.sin6_port);
  return fd;
}

/**
 * Receive a datagram with a timeout. Returns true if something was received.
 */
bool recv_with_timeout(int fd, std::vector<uint8_t>& out, int timeout_ms) {
  fd_set rfds;
  FD_ZERO(&rfds);
  FD_SET(fd, &rfds);

  timeval tv{};
  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  int rc = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
  if (rc <= 0) return false;

  uint8_t buf[2048];
  ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
  if (n <= 0) return false;

  out.assign(buf, buf + n);
  return true;
}

/**
 * Drain socket for a period, to reduce flakiness in "no further packets" checks.
 */
void drain_for(int fd, int ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  std::vector<uint8_t> tmp;
  while (std::chrono::steady_clock::now() < deadline) {
    (void)recv_with_timeout(fd, tmp, 50);
  }
}

}  // namespace

// ---- basic API tests ----

static void test_send_now_v4() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  const std::string msg = "hello-v4";

  int err = -1;
  bool ok = sender.send_now("127.0.0.1", port, msg.data(), msg.size(), &err);
  assert(ok);
  assert(err == 0);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 500);
  assert(r);
  assert(std::string(got.begin(), got.end()) == msg);

  ::close(srv);
}

static void test_send_now_vector_overload_v4() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'v','e','c','t','o','r'};

  int err = -1;
  bool ok = sender.send_now("127.0.0.1", port, payload, &err);
  assert(ok);
  assert(err == 0);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 500);
  assert(r);
  assert(got == payload);

  ::close(srv);
}

static void test_send_now_v6_if_available() {
  uint16_t port = 0;
  int srv = make_server_socket_v6(port);
  if (srv < 0) return;

  udp_sender::UdpSender sender;
  const std::string msg = "hello-v6";

  int err = -1;
  bool ok = sender.send_now("::1", port, msg.data(), msg.size(), &err);
  assert(ok);
  assert(err == 0);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 500);
  assert(r);
  assert(std::string(got.begin(), got.end()) == msg);

  ::close(srv);
}

static void test_send_after() {
  using clock = std::chrono::steady_clock;

  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'d','e','l','a','y'};

  auto t0 = clock::now();
  sender.send_after(1, "127.0.0.1", port, payload);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 2500);
  assert(r);
  auto t1 = clock::now();

  assert(got == payload);

  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
  assert(elapsed_ms >= 800);
  assert(elapsed_ms <= 2200);

  ::close(srv);
}

static void test_send_every_and_cancel() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'p','i','n','g'};

  auto id = sender.send_every(1, "127.0.0.1", port, payload);

  int count = 0;
  for (int i = 0; i < 3; ++i) {
    std::vector<uint8_t> got;
    if (recv_with_timeout(srv, got, 2500)) {
      if (got == payload) count++;
    }
    if (count >= 2) break;
  }
  assert(count >= 2);

  bool canceled = sender.cancel(id);
  assert(canceled);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 1500);
  assert(!r);

  ::close(srv);
}

static void test_send_now_invalid_ip_sets_errno() {
  udp_sender::UdpSender sender;
  const std::string msg = "x";

  int err = 0;
  bool ok = sender.send_now("not-an-ip", 12345, msg.data(), msg.size(), &err);
  assert(!ok);
  assert(err == EINVAL);
}

static void test_send_now_unreachable_sets_errno() {
  udp_sender::UdpSender sender;
  const std::string msg = "x";

  int err = 0;
  bool ok = sender.send_now("203.0.113.1", 9, msg.data(), msg.size(), &err);
  if (!ok) {
    assert(err != 0);
  }
}

// ---- edge-case tests ----

static void test_invalid_seconds_throw() {
  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'x'};

  bool threw0 = false;
  try { (void)sender.send_after(0, "127.0.0.1", 12345, payload); }
  catch (const std::invalid_argument&) { threw0 = true; }
  assert(threw0);

  bool threw0b = false;
  try { (void)sender.send_every(0, "127.0.0.1", 12345, payload); }
  catch (const std::invalid_argument&) { threw0b = true; }
  assert(threw0b);
}

static void test_cancel_unknown_id_returns_false() {
  udp_sender::UdpSender sender;
  bool ok = sender.cancel(999999999ULL);
  assert(!ok);
}

static void test_cancel_one_shot_before_due() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'n','o'};

  auto id = sender.send_after(1, "127.0.0.1", port, payload);
  bool canceled = sender.cancel(id);
  assert(canceled);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 1600);
  assert(!r);

  ::close(srv);
}

static void test_cancel_one_shot_after_due_race_no_extra_send() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'r','a','c','e'};

  auto id = sender.send_after(1, "127.0.0.1", port, payload);

  std::this_thread::sleep_for(std::chrono::milliseconds(950));
  (void)sender.cancel(id);

  int received = 0;
  for (int i = 0; i < 2; ++i) {
    std::vector<uint8_t> got;
    if (recv_with_timeout(srv, got, 1200) && got == payload) received++;
  }
  assert(received <= 1);

  ::close(srv);
}

static void test_multiple_tasks_same_tick_both_deliver() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> a{'a'};
  std::vector<uint8_t> b{'b'};

  sender.send_after(1, "127.0.0.1", port, a);
  sender.send_after(1, "127.0.0.1", port, b);

  int got_a = 0;
  int got_b = 0;
  for (int i = 0; i < 2; ++i) {
    std::vector<uint8_t> got;
    bool r = recv_with_timeout(srv, got, 2500);
    assert(r);
    if (got == a) got_a++;
    if (got == b) got_b++;
  }
  assert(got_a == 1);
  assert(got_b == 1);

  ::close(srv);
}

static void test_periodic_cancel_immediate_stop() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'p'};

  auto id = sender.send_every(1, "127.0.0.1", port, payload);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 2500);
  assert(r && got == payload);

  assert(sender.cancel(id));
  drain_for(srv, 2100);

  got.clear();
  bool r2 = recv_with_timeout(srv, got, 200);
  assert(!r2);

  ::close(srv);
}

static void test_periodic_cancel_before_first_send() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> payload{'z'};

  auto id = sender.send_every(1, "127.0.0.1", port, payload);
  assert(sender.cancel(id));

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 1600);
  assert(!r);

  ::close(srv);
}

static void test_destructor_with_pending_tasks() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  {
    udp_sender::UdpSender sender;
    std::vector<uint8_t> p1{'a'};
    std::vector<uint8_t> p2{'b'};
    std::vector<uint8_t> p3{'c'};

    (void)sender.send_after(2, "127.0.0.1", port, p1);
    (void)sender.send_after(3, "127.0.0.1", port, p2);
    (void)sender.send_every(1, "127.0.0.1", port, p3);
  }

  ::close(srv);
}

static void test_send_now_errno_is_overwritten_on_success() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  const std::string msg = "ok";

  int err = 12345;
  bool ok = sender.send_now("127.0.0.1", port, msg.data(), msg.size(), &err);
  assert(ok);
  assert(err == 0);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 500);
  assert(r);
  assert(std::string(got.begin(), got.end()) == msg);

  ::close(srv);
}

static void test_long_delay_not_early() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  std::vector<uint8_t> longp{'L'};
  std::vector<uint8_t> shortp{'S'};

  sender.send_after(255, "127.0.0.1", port, longp);
  sender.send_after(1, "127.0.0.1", port, shortp);

  std::vector<uint8_t> got;
  bool r1 = recv_with_timeout(srv, got, 2500);
  assert(r1);
  assert(got == shortp);

  got.clear();
  bool r2 = recv_with_timeout(srv, got, 3000);
  assert(!r2);

  ::close(srv);
}

// ---- error callback tests ----

/**
 * Simple recorder passed to the error callback.
 * Uses atomics because callback runs from the worker thread.
 */
struct ErrRec {
  std::atomic<int> calls{0};
  std::atomic<int> last_err{0};
  std::atomic<udp_sender::UdpSender::TaskId> last_id{0};
};

static void on_err_cb(udp_sender::UdpSender::TaskId id, int err, void* user) {
  auto* r = static_cast<ErrRec*>(user);
  r->last_id.store(id, std::memory_order_relaxed);
  r->last_err.store(err, std::memory_order_relaxed);
  r->calls.fetch_add(1, std::memory_order_relaxed);
}

static void test_error_callback_not_called_on_success_send_after() {
  uint16_t port = 0;
  int srv = make_server_socket_v4(port);

  udp_sender::UdpSender sender;
  ErrRec rec{};
  sender.set_error_callback(&on_err_cb, &rec);

  std::vector<uint8_t> payload{'o','k'};
  sender.send_after(1, "127.0.0.1", port, payload);

  std::vector<uint8_t> got;
  bool r = recv_with_timeout(srv, got, 2500);
  assert(r);
  assert(got == payload);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  assert(rec.calls.load(std::memory_order_relaxed) == 0);

  ::close(srv);
}

static void test_error_callback_called_on_invalid_ip_send_after() {
  udp_sender::UdpSender sender;
  ErrRec rec{};
  sender.set_error_callback(&on_err_cb, &rec);

  std::vector<uint8_t> payload{'x'};
  auto id = sender.send_after(1, "not-an-ip", 12345, payload);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  while (std::chrono::steady_clock::now() < deadline &&
         rec.calls.load(std::memory_order_relaxed) == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  assert(rec.calls.load(std::memory_order_relaxed) >= 1);
  assert(rec.last_id.load(std::memory_order_relaxed) == id);
  assert(rec.last_err.load(std::memory_order_relaxed) == EINVAL);
}

/**
 * Periodic failure should keep invoking the callback until cancel().
 * After cancel(), callback count must stop increasing.
 */
static void test_error_callback_periodic_invalid_ip_and_cancel_stops() {
  udp_sender::UdpSender sender;
  ErrRec rec{};
  sender.set_error_callback(&on_err_cb, &rec);

  std::vector<uint8_t> payload{'x'};
  auto id = sender.send_every(1, "not-an-ip", 12345, payload);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
  while (std::chrono::steady_clock::now() < deadline &&
         rec.calls.load(std::memory_order_relaxed) < 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const int calls_before_cancel = rec.calls.load(std::memory_order_relaxed);
  assert(calls_before_cancel >= 2);
  assert(rec.last_id.load(std::memory_order_relaxed) == id);
  assert(rec.last_err.load(std::memory_order_relaxed) == EINVAL);

  assert(sender.cancel(id));

  std::this_thread::sleep_for(std::chrono::milliseconds(1600));
  const int calls_after = rec.calls.load(std::memory_order_relaxed);
  assert(calls_after == calls_before_cancel);
}

int main() {
  test_send_now_v4();
  test_send_now_vector_overload_v4();
  test_send_now_v6_if_available();
  test_send_after();
  test_send_every_and_cancel();
  test_send_now_invalid_ip_sets_errno();
  test_send_now_unreachable_sets_errno();

  test_invalid_seconds_throw();
  test_cancel_unknown_id_returns_false();
  test_cancel_one_shot_before_due();
  test_cancel_one_shot_after_due_race_no_extra_send();
  test_multiple_tasks_same_tick_both_deliver();
  test_periodic_cancel_immediate_stop();

  test_periodic_cancel_before_first_send();
  test_destructor_with_pending_tasks();
  test_send_now_errno_is_overwritten_on_success();
  test_long_delay_not_early();

  test_error_callback_not_called_on_success_send_after();
  test_error_callback_called_on_invalid_ip_send_after();
  test_error_callback_periodic_invalid_ip_and_cancel_stops();

  std::cout << "All tests passed\n";
  return 0;
}
