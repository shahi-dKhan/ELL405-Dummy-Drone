// drone_core.cpp
// PHASE: ADVANCED PROFILER (With Deadline Miss Tracking)

#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>
#include <iomanip>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <sys/resource.h>
#include <condition_variable>

#include <opencv2/opencv.hpp>

using namespace std;

// --- SHARED STATS ---
struct SystemStats {
    // Flight
    long flight_loops = 0;
    long flight_exec_avg_us = 0;
    long flight_preempts = 0;
    long flight_deadline_misses = 0;  // NEW

    // Vision
    long vision_frames = 0;
    long vision_preempts = 0;

    // Emergency
    long emerg_wakeups = 0;
    long emerg_preempts = 0;

    std::mutex stats_mutex;
};

SystemStats global_stats;
std::atomic<bool> system_running(true);
std::atomic<bool> emergency_active(false);
std::condition_variable cv_emergency;
std::mutex emergency_mutex;

// --- HELPER: Pin Thread to Core ---
void pin_thread_to_core(std::thread &th, int core_id) {
    if (core_id < 0) return; 

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    if (pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset) != 0) {
        // cerr << "Pin failed" << endl;
    }
}

// --- HELPER: Get Preemptions ---
long get_kernel_preemptions() {
    struct rusage usage;
    getrusage(RUSAGE_THREAD, &usage);
    return usage.ru_nivcsw; 
}

// --- HELPER: Set Priority ---
void set_thread_priority(std::thread &th, int priority) {
    sched_param sch_params;
    sch_params.sched_priority = priority;
    pthread_setschedparam(th.native_handle(), SCHED_FIFO, &sch_params);
}

// --- THREAD 1: FLIGHT (Prio 50) ---
void task_flight_control() {
    float alt = 0;
    auto next_deadline = std::chrono::steady_clock::now();
    
    while (system_running) {
        // Update deadline for this iteration
        next_deadline += std::chrono::milliseconds(10);
        
        auto start = std::chrono::steady_clock::now();

        // Check if we're already late when starting this iteration
        if (start > next_deadline) {
            std::lock_guard<std::mutex> lock(global_stats.stats_mutex);
            global_stats.flight_deadline_misses++;
        }

        // 1. Math Work (Simulation)
        for(int i=0; i<2000; i++) { alt += 0.001f; } 

        // 2. Metrics
        auto end = std::chrono::steady_clock::now();
        long duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        long preempts = get_kernel_preemptions();

        {
            std::lock_guard<std::mutex> lock(global_stats.stats_mutex);
            global_stats.flight_loops++;
            global_stats.flight_exec_avg_us = (global_stats.flight_exec_avg_us + duration) / 2;
            global_stats.flight_preempts = preempts;
        }

        // 3. Trigger Emergency at Loop 500
        static int count = 0;
        count++;
        if (count == 2000) {
            { lock_guard<mutex> l(emergency_mutex); emergency_active = true; }
            cv_emergency.notify_one();
        }

        std::this_thread::sleep_until(next_deadline);
    }
}

// --- THREAD 2: VISION (Prio 10) - PERIOD-LESS ---
void task_vision() {
    cv::Mat fake_frame(480, 640, CV_8UC3, cv::Scalar(0,0,0));

    while (system_running) {
        // Burn CPU
        cv::circle(fake_frame, cv::Point(rand()%640, rand()%480), 50, cv::Scalar(0,255,0), -1);
        vector<uchar> buf;
        vector<int> params = {cv::IMWRITE_JPEG_QUALITY, 50};
        cv::imencode(".jpg", fake_frame, buf, params);

        long preempts = get_kernel_preemptions();

        {
            std::lock_guard<std::mutex> lock(global_stats.stats_mutex);
            global_stats.vision_frames++;
            global_stats.vision_preempts = preempts; 
        }
        
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

// --- THREAD 3: EMERGENCY (Prio 90) ---
void task_emergency() {
    std::unique_lock<std::mutex> lock(emergency_mutex);
    cv_emergency.wait(lock, []{ return emergency_active.load(); });
    
    long preempts = get_kernel_preemptions();
    {
        std::lock_guard<std::mutex> lock(global_stats.stats_mutex);
        global_stats.emerg_wakeups++;
        global_stats.emerg_preempts = preempts;
    }
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    system_running = false;
}

// --- MONITOR THREAD ---
void task_monitor(int core_mode) {
    cout << "\n--------------------------------------------------------------------------------" << endl;
    cout << " RTOS PROFILER | Mode: " << (core_mode == 1 ? "SINGLE CORE (Conflict)" : "MULTI CORE (Parallel)") << endl;
    cout << "--------------------------------------------------------------------------------" << endl;
    cout << "| FLIGHT (High Prio)        | VISION (Low Prio)     | EMERG (Critical)     |" << endl;
    cout << "| Time(us) | Preempts | Miss | FPS | Preempts        | Active | Preempts    |" << endl;
    cout << "--------------------------------------------------------------------------------" << endl;

    while (system_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        long f_time, f_pre, f_miss, v_fps, v_pre, e_act, e_pre;

        {
            std::lock_guard<std::mutex> lock(global_stats.stats_mutex);
            f_time = global_stats.flight_exec_avg_us;
            f_pre  = global_stats.flight_preempts;
            f_miss = global_stats.flight_deadline_misses;
            v_fps  = global_stats.vision_frames;
            v_pre  = global_stats.vision_preempts;
            e_act  = global_stats.emerg_wakeups;
            e_pre  = global_stats.emerg_preempts;
            
            global_stats.vision_frames = 0;  // Reset for per-second FPS count
        }

        cout << "| " << setw(8) << f_time << " | " << setw(8) << f_pre << " | " << setw(4) << f_miss
             << " | " << setw(3) << v_fps << " | " << setw(15) << v_pre 
             << " | " << setw(6) << (e_act ? "YES" : "NO") << " | " << setw(11) << e_pre << " |" << endl;
    }
}

// --- MAIN ---
int main(int argc, char* argv[]) {
    // ARGUMENT PARSING - FIXED LOGIC
    int target_core = -1; 
    if (argc > 1 && string(argv[1]) == "1") {
        target_core = 0; // Force single core (Core 0)
    }
    // If no argument (or any other arg), target_core stays -1 (multi-core)

    cout << "\nUsage: sudo ./drone_core [1]" << endl;
    cout << "  1  -> Single core mode (force Core 0)" << endl;
    cout << "  no args -> Multi-core mode (parallel execution)" << endl;

    std::thread t_flight(task_flight_control);
    std::thread t_vision(task_vision);
    std::thread t_emerg(task_emergency);
    std::thread t_monitor(task_monitor, (target_core == 0 ? 1 : 4));

    set_thread_priority(t_emerg, 90);
    set_thread_priority(t_flight, 50);
    set_thread_priority(t_vision, 10);

    if (target_core != -1) {  // Only pin if "1" was specified
        pin_thread_to_core(t_flight, target_core);
        pin_thread_to_core(t_vision, target_core);
        pin_thread_to_core(t_emerg, target_core);
    }

    t_flight.join();
    t_vision.join();
    t_emerg.join();
    t_monitor.join();
    return 0;
}