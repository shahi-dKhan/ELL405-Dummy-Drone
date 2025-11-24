#include <iostream>
#include <thread>
#include <mutex>
#include <chrono>
#include <atomic>
#include <vector>
#include <cmath>
#include <condition_variable>
#include <opencv2/opencv.hpp>


// --- SHARED MEMORY SECTION ---
// This struct holds the state of the drone. 
// Multiple threads access this, so we MUST protect it.
struct DroneState {
    // Flight Data
    float target_throttle = 0.0f; // Input from user (0-100)
    float current_altitude = 0.0f; // Calculated physics output
    float velocity = 0.0f;
    
    // Vision Data
    int last_frame_size = 0;      // Meta-data to prove camera is working
    bool object_detected = false; 

    // Synchronization Primitives
    std::mutex data_mutex;             // The "Key" to access this data
    std::condition_variable cv_emergency; // To wake up the emergency thread
    bool emergency_triggered = false;  // Flag for the emergency
};

// Global Instance of our State
DroneState shared_state;
std::atomic<bool> system_running(true); // Global flag to stop all threads

// --- THREAD 1: PERIODIC (Flight Controller) ---
// Simulates the Physics Loop (F=ma)
// Goal frequency: 100Hz (10ms)
void task_flight_control() {
    std::cout << "[Flight] Thread Started." << std::endl;
    
    const float gravity = 9.81f;
    const float mass = 1.0f;
    const float dt = 0.01f; // 10ms

    while (system_running) {
        auto start_time = std::chrono::high_resolution_clock::now();

        // 1. Read & Write Shared Data (CRITICAL SECTION)
        {
            std::lock_guard<std::mutex> lock(shared_state.data_mutex);
            
            // Check Emergency
            if (shared_state.emergency_triggered) {
                shared_state.target_throttle = 0; // Cut motors
            }

            // Simple Physics: Throttle 50% = Hover (approx)
            // Throttle * 0.2 gives us Force in Newtons (0-20N)
            float force = shared_state.target_throttle * 0.2f; 
            float net_force = force - (mass * gravity);
            float acceleration = net_force / mass;

            shared_state.velocity += acceleration * dt;
            shared_state.current_altitude += shared_state.velocity * dt;

            // Ground collision
            if (shared_state.current_altitude < 0) {
                shared_state.current_altitude = 0;
                shared_state.velocity = 0;
            }
        }

        // 2. Timing (Sleep to maintain ~100Hz)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// --- THREAD 2: APERIODIC (Vision System) ---
// Captures camera frames. High Load.
// Goal frequency: ~30Hz (33ms)
void task_vision() {
    std::cout << "[Vision] Thread Started. Opening Camera..." << std::endl;
    
    // Open Pi Camera (Index 0). 
    // IF THIS FAILS: Ensure camera is enabled in raspi-config
    cv::VideoCapture cap(0); 
    
    if (!cap.isOpened()) {
        std::cerr << "[Vision] WARNING: Camera not found! Simulating vision data." << std::endl;
    } else {
        // Lower resolution for performance
        cap.set(cv::CAP_PROP_FRAME_WIDTH, 320);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, 240);
    }

    cv::Mat frame;

    while (system_running) {
        int data_size = 0;

        if (cap.isOpened()) {
            cap >> frame; // Heavy I/O blocking call
            if (!frame.empty()) {
                // Simulate processing (e.g., encode to JPG)
                std::vector<uchar> buf;
                cv::imencode(".jpg", frame, buf);
                data_size = buf.size();
            }
        } else {
            // Simulated dummy load if no camera
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            data_size = 1024; // Fake 1KB frame
        }

        // Update Shared State
        {
            std::lock_guard<std::mutex> lock(shared_state.data_mutex);
            shared_state.last_frame_size = data_size;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

// --- THREAD 3: SPORADIC (Emergency Failsafe) ---
// Sleeps 99% of the time. Wakes up ONLY on event.
void task_emergency() {
    std::cout << "[Emergency] Failsafe ARMED." << std::endl;
    
    std::unique_lock<std::mutex> lock(shared_state.data_mutex);
    
    // Wait here until notified. Does not consume CPU.
    shared_state.cv_emergency.wait(lock, []{ return shared_state.emergency_triggered; });

    // --- EMERGENCY HAPPENED ---
    std::cout << "\n\n!!! [Emergency] FAILSAFE TRIGGERED !!!" << std::endl;
    std::cout << "!!! [Emergency] CUTTING MOTORS !!!\n" << std::endl;
    
    shared_state.target_throttle = 0.0f;
    // In a real drone, we might exit the program or enter a specific landing mode
}

// --- MAIN THREAD (Telemetry & Simulation) ---
int main() {
    std::cout << "--- PHASE 1: DRONE CORE INIT ---" << std::endl;

    // 1. Launch Threads
    std::thread t1(task_flight_control);
    std::thread t2(task_vision);
    std::thread t3(task_emergency);

    // 2. Simulation Loop (Acts like the User/Radio)
    // We will simulate the user throttling up for 5 seconds
    
    std::cout << "System running... Throttling up..." << std::endl;
    
    for (int i = 0; i < 50; i++) { // Run for ~5 seconds
        {
            std::lock_guard<std::mutex> lock(shared_state.data_mutex);
            // Simulate user pushing stick up
            shared_state.target_throttle = 60.0f; 
            
            // Print Telemetry
            std::cout << "Alt: " << shared_state.current_altitude << " m | "
                      << "Vel: " << shared_state.velocity << " m/s | "
                      << "Cam: " << shared_state.last_frame_size << " bytes" 
                      << "\r" << std::flush;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 3. Trigger Emergency Test
    std::cout << "\n\n[Main] SIMULATING CRASH TRIGGER..." << std::endl;
    {
        std::lock_guard<std::mutex> lock(shared_state.data_mutex);
        shared_state.emergency_triggered = true;
    }
    shared_state.cv_emergency.notify_one(); // Wake up Thread 3!

    // 4. Cleanup
    std::this_thread::sleep_for(std::chrono::seconds(1));
    system_running = false; // Stop loops
    
    t1.join();
    t2.join();
    t3.join();

    std::cout << "[Main] System Shutdown Complete." << std::endl;
    return 0;
}