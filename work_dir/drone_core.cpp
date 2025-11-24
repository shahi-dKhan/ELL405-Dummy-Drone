// drone_core.cpp
// PHASE 2: RTOS SCHEDULING (SCHED_FIFO)

#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstring>
#include <condition_variable>
#include <pthread.h> // Native Linux Threading
#include <sched.h>   // Scheduler definitions

// OpenCV for Camera handling
#include <opencv2/opencv.hpp>

using namespace std;

// --- SHARED MEMORY ---
struct DroneState {
    float target_throttle = 0.0f; 
    float current_altitude = 0.0f;
    float velocity = 0.0f;
    int last_frame_size = 0;      
    bool emergency_triggered = false; 

    std::mutex data_mutex;             
    std::condition_variable cv_emergency; 
};

DroneState shared_state;
std::atomic<bool> system_running(true);

// --- HELPER: SET RTOS PRIORITY ---
void set_thread_priority(std::thread &th, int priority, string name) {
    sched_param sch_params;
    sch_params.sched_priority = priority;
    
    // Attempt to set SCHED_FIFO (First In First Out) Real-Time Policy
    if(pthread_setschedparam(th.native_handle(), SCHED_FIFO, &sch_params)) {
        cerr << "[Scheduler] FAILED to set priority for " << name << ": " << strerror(errno) << endl;
        cerr << "            (Did you run with SUDO?)" << endl;
    } else {
        cout << "[Scheduler] " << name << " set to Priority " << priority << " (SCHED_FIFO)" << endl;
    }
}

// --- THREAD 1: PERIODIC (Flight Control - High Priority) ---
void task_flight_control() {
    const float gravity = 9.81f;
    const float mass = 1.0f;
    const float dt = 0.01f; // 10ms

    while (system_running) {
        auto start_time = std::chrono::high_resolution_clock::now();

        {
            std::lock_guard<std::mutex> lock(shared_state.data_mutex);
            if (shared_state.emergency_triggered) shared_state.target_throttle = 0;

            float force = shared_state.target_throttle * 0.2f; 
            float net_force = force - (mass * gravity);
            float acceleration = net_force / mass;

            shared_state.velocity += acceleration * dt;
            shared_state.current_altitude += shared_state.velocity * dt;

            if (shared_state.current_altitude < 0) {
                shared_state.current_altitude = 0; 
                shared_state.velocity = 0;
            }
        }

        // Sleep to maintain 100Hz
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- THREAD 2: APERIODIC (Vision - Low Priority) ---
void task_vision() {
    // Attempt to open camera
    cv::VideoCapture cap(0); 
    if(cap.isOpened()) {
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);
    }

    cv::Mat frame;

    while (system_running) {
        int data_size = 0;
        bool capture_success = false;

        // Try to read frame
        if (cap.isOpened()) {
            cap >> frame;
            if (!frame.empty()) {
                capture_success = true;
                // Encode to JPG (Simulate load)
                std::vector<uchar> buf;
                cv::imencode(".jpg", frame, buf);
                data_size = buf.size();
            }
        }

        // FALLBACK: If Camera failed (0 bytes), simulate data
        if (!capture_success) {
            // Simulate 20ms processing time
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            data_size = 1500 + (rand() % 500); // Random fake size
        }

        {
            std::lock_guard<std::mutex> lock(shared_state.data_mutex);
            shared_state.last_frame_size = data_size;
        }
        
        // Run at ~30Hz
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

// --- THREAD 3: SPORADIC (Emergency - Critical Priority) ---
void task_emergency() {
    std::unique_lock<std::mutex> lock(shared_state.data_mutex);
    shared_state.cv_emergency.wait(lock, []{ return shared_state.emergency_triggered; });
    
    // When we wake up, we are holding the lock!
    cout << "\n!!! [Emergency] FAILSAFE TRIGGERED !!!" << endl;
    cout << "!!! [Emergency] CUTTING MOTORS !!!\n" << endl;
    shared_state.target_throttle = 0.0f;
}

// --- MAIN ---
int main() {
    cout << "--- PHASE 2: RTOS SCHEDULER INIT ---" << endl;

    // 1. Create Threads
    std::thread t_flight(task_flight_control);
    std::thread t_vision(task_vision);
    std::thread t_emergency(task_emergency);

    // 2. APPLY RTOS SCHEDULES (The Magic Part)
    // Note: 99 is highest, 1 is lowest in SCHED_FIFO
    set_thread_priority(t_emergency, 90, "Emergency Task");
    set_thread_priority(t_flight,    50, "Flight Loop   ");
    set_thread_priority(t_vision,    10, "Vision Task   ");

    // 3. Simulation
    cout << "\nSystem Running. Switch to 'htop' to see priorities." << endl;
    cout << "Throttling up..." << endl;

    for (int i = 0; i < 50; i++) {
        {
            std::lock_guard<std::mutex> lock(shared_state.data_mutex);
            shared_state.target_throttle = 60.0f; 
            cout << "Alt: " << shared_state.current_altitude << "m | "
                 << "Cam: " << shared_state.last_frame_size << " bytes "
                 << "\r" << flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 4. Trigger Emergency
    cout << "\n\n[Main] TRIGGERING CRASH..." << endl;
    {
        std::lock_guard<std::mutex> lock(shared_state.data_mutex);
        shared_state.emergency_triggered = true;
    }
    shared_state.cv_emergency.notify_one(); 

    // Cleanup
    std::this_thread::sleep_for(std::chrono::seconds(1));
    system_running = false;
    t_flight.join();
    t_vision.join();
    t_emergency.join();

    return 0;
}