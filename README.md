# Quantum Systems Test
Quantum Systems remote case study

## 0. linux case. Files received from Quantum Systems

## 1. cpp task. Thread runner with cooperative stop and timeout

The original C++ implementation had several issues related to threading and lifetime management.

First, the worker function was passed as `const std::function<bool()>&` and captured by reference inside the thread lambda. When a lambda was passed at the call site, it was implicitly converted into a temporary `std::function`. That temporary was destroyed when `StartThread` returned, while the worker thread continued to use it, resulting in undefined behavior.

Second, the timeout logic relied on `std::chrono::high_resolution_clock`. This clock is not guaranteed to be monotonic and may jump backwards, which makes it unsuitable for measuring elapsed time.

The corrected version makes the following changes:

- The worker callable is passed by value as `std::function<bool()>` and moved into the thread’s capture list. This ensures the thread owns the callable and its lifetime is guaranteed for the entire execution of the thread.

- Timeout measurement is performed using `std::chrono::steady_clock`, which is monotonic and safe for elapsed-time calculations.

- The thread lambda explicitly captures only the required variables, improving clarity and preventing accidental reference captures.

Tests: [![cpp_task](https://github.com/artemkulyk/quantum-systems-test/actions/workflows/cpp_task.yaml/badge.svg)](https://github.com/artemkulyk/quantum-systems-test/actions/workflows/cpp_task.yaml)

## 2. python task. Matrix Rotation Fix

The original implementation attempted to rotate the matrix by directly reassigning values while iterating over all rows and columns. This approach failed because elements were overwritten before their original values were used elsewhere, leading to incorrect results.

The fix uses a layer by layer rotation strategy. A square matrix can be viewed as a set of concentric layers, starting from the outer border and moving inward. Each layer is rotated independently.

For a given layer, four corresponding elements are rotated at a time: top, right, bottom, and left. A temporary variable stores one value so that all four positions can be updated without losing data. After completing one layer, the algorithm proceeds to the next inner layer.

This method rotates the matrix 90 degrees clockwise, works entirely in place, preserves all values during rotation, and runs in $O(n^2)$ time with constant extra space.

Tests: [![python_task](https://github.com/artemkulyk/quantum-systems-test/actions/workflows/python_task.yaml/badge.svg)](https://github.com/artemkulyk/quantum-systems-test/actions/workflows/python_task.yaml)

## 3. cpp class design and implementation. UDP sender with scheduling

This task implements `udp_sender::UdpSender`: a small UDP sending utility with:

- Immediate sends (`send_now`) for raw buffers and `std::vector<std::uint8_t>`.
- One-shot delayed sends (`send_after`) and periodic sends (`send_every`).
- Cancellation of scheduled tasks (`cancel(TaskId)`).
- Optional error callback for scheduled sends (`set_error_callback`), invoked from the worker thread when a scheduled send fails.

Design highlights:

- Scheduling uses a fixed-size timing wheel with 1-second resolution (256 buckets).
- A single worker thread advances time in seconds and processes due buckets.
- Scheduled send failures do not throw; they are reported via the callback with an `errno` value.
- The callback is invoked without holding internal locks to avoid deadlocks and reduce contention.
- `send_now` reports errors synchronously via `bool` return and optional `out_errno`.

Tests: [![cpp_class](https://github.com/artemkulyk/quantum-systems-test/actions/workflows/cpp_class.yaml/badge.svg)](https://github.com/artemkulyk/quantum-systems-test/actions/workflows/cpp_class.yaml)
