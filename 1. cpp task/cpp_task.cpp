#include <chrono>
#include <atomic>
#include <memory>
#include <thread>
#include <functional>
#include <iostream>

using namespace std::chrono_literals;

void StartThread(
    std::thread& thread,
    std::atomic<bool>& running,
    std::function<bool(void)> Process, // Pass by value to avoid dangling reference to a temporary std::function
    const std::chrono::seconds timeout)
{
    thread = std::thread(
        // Move Process into the thread capture so the callable's lifetime is owned by the thread
        // Use steady_clock for monotonic timeout measurement
        [&running, timeout, Process = std::move(Process)] () mutable
        {
            const auto start = std::chrono::steady_clock::now();
            while (running)
            {
                const bool aborted = Process();

                const auto duration = std::chrono::steady_clock::now() - start;
                if (aborted || duration > timeout)
                {
                    running = false;
                    break;
                }
            }
        });
}

int main(int argc, char **argv)
{
    std::atomic<bool> my_running = true;
    std::thread my_thread1, my_thread2;
    int loop_counter1 = 0, loop_counter2 = 0;

    // start actions in separate threads and wait of them

    StartThread(
        my_thread1,
        my_running,
        [&]()
        {
            // "some actions" simulated with waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            loop_counter1++;
            return false;
        },
        10s); // loop timeout

    StartThread(
        my_thread2,
        my_running,
        [&]()
        {
            // "some actions" simulated with waiting
            if (loop_counter2 < 5)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                loop_counter2++;
                return false;
            }
            return true;
        },
        10s); // loop timeout


    my_thread1.join();
    my_thread2.join();

    // print execution loop counters
    std::cout << "C1: " << loop_counter1 << " C2: " << loop_counter2 << std::endl;
}
