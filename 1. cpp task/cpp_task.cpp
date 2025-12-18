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
    const std::function<bool(void)>& Process,
    const std::chrono::seconds timeout)
{
    thread = std::thread(
        [&] ()
        {
            auto start = std::chrono::high_resolution_clock::now();
            while(running)
            {
                bool aborted = Process();

                auto end = std::chrono::high_resolution_clock::now();
                auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
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

    // start actions in seprate threads and wait of them

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

    // print execlution loop counters
    std::cout << "C1: " << loop_counter1 << " C2: " << loop_counter2 << std::endl;
}
