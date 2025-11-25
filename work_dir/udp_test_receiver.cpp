// drone_core.cpp
// FINAL VERSION: Proper emergency shutdown with cleanup

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
#include <sys/socket.h>   
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <condition_variable>
#include <csignal>
#include <cstdlib>

using namespace std;

// --- CONFIGURATION ---
int LOCAL_PORT = 8080;

// --- SHARED STATE ---
struct DroneState {
    float throttle = 0.0f; 
    float pitch = 0.0f;
    float roll = 0.0f;
    float yaw = 0.0f;
    float altitude = 0.0f;
    float velocity = 0.0f;
    bool emergency_triggered = false;
    std::mutex state_mutex;
    std::condition_variable cv_emergency;
};

// --- PROFILER STATS ---
struct SystemStats {
    long flight_loops = 0;
    long flight_exec_avg_us = 0;
    long flight_preempts = 0;
    long flight_deadline_misses = 0;
    long net_packets = 0;
    long net_preempts = 0;
    long vision_fps = 0;
    long vision_preempts = 0;
    bool vision_active = false;
    string emergency_status = "STANDBY";
    std::mutex stats_mutex;
};

DroneState shared_state;
SystemStats global_stats;
std::atomic<bool> system_running(true);

// --- CLEANUP FUNCTION ---
void cleanup_and_exit() {
    cout << "\n\n=== EMERGENCY SHUTDOWN SEQUENCE ===" << endl;
    
    // Stop all threads
    system_running = false;
    
    // Kill motors
    {
        std::lock_guard<std::mutex> l(shared_state.state_mutex);
        shared_state.throttle = 0;
        shared_state.pitch = 0;
        shared_state.roll = 0;
        shared_state.yaw = 0;
    }
    
    cout << "✓ Motors stopped" << endl;
    
    // Kill camera
    system("pkill -9 rpicam-vid 2>/dev/null");
    cout << "✓ Camera stopped" << endl;
    
    // Print final stats
    {
        std::lock_guard<std::mutex> l(global_stats.stats_mutex);
        cout << "\n--- FINAL STATS ---" << endl;
        cout << "Flight loops: " << global_stats.flight_loops << endl;
        cout << "Deadline misses: " << global_stats.flight_deadline_misses << endl;
        cout << "Total packets: " << global_stats.net_packets << endl;
    }
    
    cout << "\n✓ Shutdown complete" << endl;
    cout << "=================================\n" << endl;
    
    std::exit(0);
}

// --- HELPERS ---
long get_kernel_preemptions() {
    struct rusage usage;
    getrusage(RUSAGE_THREAD, &usage);
    return usage.ru_nivcsw; 
}

void set_priority(std::thread &th, int prio, string name) {
    sched_param param;
    param.sched_priority = prio;
    if (pthread_setschedparam(th.native_handle(), SCHED_FIFO, &param) != 0) {
        cerr << "[Scheduler] FAILED " << name << endl;
    }
}

void pin_thread(std::thread &th, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
}

// --- THREAD 1: FLIGHT CONTROL ---
void task_flight() {
    auto next_wake = std::chrono::steady_clock::now();
    const float dt = 0.01f;

    while (system_running) {
        next_wake += std::chrono::milliseconds(10); // 100Hz

        if (std::chrono::steady_clock::now() > next_wake) {
            std::lock_guard<std::mutex> l(global_stats.stats_mutex);
            global_stats.flight_deadline_misses++;
        }

        auto start = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> l(shared_state.state_mutex);
            
            if (shared_state.emergency_triggered) {
                shared_state.throttle = 0;
                shared_state.pitch = 0;
                shared_state.roll = 0;
            }

            float lift = shared_state.throttle * 0.25f; 
            float gravity = 9.81f;
            float tilt_factor = 1.0f - (abs(shared_state.pitch) + abs(shared_state.roll)) * 0.005f;
            
            float accel = (lift * tilt_factor) - gravity;
            shared_state.velocity += accel * dt;
            shared_state.altitude += shared_state.velocity * dt;

            if (shared_state.altitude < 0) { 
                shared_state.altitude = 0; 
                shared_state.velocity = 0; 
            }
        }

        auto end = std::chrono::steady_clock::now();
        long dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        long preempts = get_kernel_preemptions();

        {
            std::lock_guard<std::mutex> l(global_stats.stats_mutex);
            global_stats.flight_loops++;
            global_stats.flight_exec_avg_us = (global_stats.flight_exec_avg_us + dur) / 2;
            global_stats.flight_preempts = preempts;
        }

        std::this_thread::sleep_until(next_wake);
    }
}

// --- THREAD 2: VISION SERVER ---
void task_vision() {
    while (system_running) {
        system("pkill -9 rpicam-vid 2>/dev/null");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        {
            std::lock_guard<std::mutex> l(global_stats.stats_mutex);
            global_stats.vision_active = true;
        }

        string cmd = "rpicam-vid -t 0 --nopreview --inline --width 640 --height 480 --codec libav --libav-format mpegts --listen -o tcp://0.0.0.0:8888 > /dev/null 2>&1 &";
        system(cmd.c_str());

        auto last_check = std::chrono::steady_clock::now();
        long frame_count = 0;
        
        for(int i=0; i<100; i++) { 
            if(!system_running) break;
            
            frame_count++;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_check).count();
            
            if (elapsed >= 1) {
                long preempts = get_kernel_preemptions();
                {
                    std::lock_guard<std::mutex> l(global_stats.stats_mutex);
                    global_stats.vision_fps = frame_count / elapsed;
                    global_stats.vision_preempts = preempts;
                }
                frame_count = 0;
                last_check = now;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    system("pkill -9 rpicam-vid 2>/dev/null");
    
    {
        std::lock_guard<std::mutex> l(global_stats.stats_mutex);
        global_stats.vision_active = false;
    }
}

// --- THREAD 3: NETWORKING ---
void task_networking() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        cerr << "[Net] ERROR: Failed to create socket" << endl;
        return;
    }
    
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY; 
    servaddr.sin_port = htons(LOCAL_PORT);
    
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        cerr << "[Net] ERROR: Bind failed - " << strerror(errno) << endl;
        close(sockfd);
        return;
    }
    
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    cout << "[Net] UDP Listening on port " << LOCAL_PORT << endl;
    
    char buffer[1024];
    struct sockaddr_in clientaddr;
    socklen_t clientlen;
    
    while (system_running) {
        clientlen = sizeof(clientaddr);
        int n = recvfrom(sockfd, buffer, sizeof(buffer) - 1, 0,
                        (struct sockaddr *)&clientaddr, &clientlen);
        
        if (n > 0) {
            buffer[n] = '\0';
            string cmd(buffer);
            
            long preempts = get_kernel_preemptions();
            {
                std::lock_guard<std::mutex> l(global_stats.stats_mutex);
                global_stats.net_packets++;
                global_stats.net_preempts = preempts;
            }

            std::lock_guard<std::mutex> l(shared_state.state_mutex);
            
            if (cmd == "PANIC") {
                shared_state.emergency_triggered = true;
                shared_state.cv_emergency.notify_one();
                {
                    std::lock_guard<std::mutex> sl(global_stats.stats_mutex);
                    global_stats.emergency_status = "TRIGGERED";
                }
            }
            else if (cmd == "UP") {
                shared_state.throttle = min(100.0f, shared_state.throttle + 10.0f);
            }
            else if (cmd == "DOWN") {
                shared_state.throttle = max(0.0f, shared_state.throttle - 10.0f);
            }
            else if (cmd == "FRONT") {
                shared_state.pitch = 15.0f;
            }
            else if (cmd == "BACK") {
                shared_state.pitch = -15.0f;
            }
            else if (cmd == "LEFT") {
                shared_state.roll = -15.0f;
            }
            else if (cmd == "RIGHT") {
                shared_state.roll = 15.0f;
            }
            else if (cmd == "STOP") {
                shared_state.pitch = 0.0f;
                shared_state.roll = 0.0f;
            }
        }
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            cerr << "[Net] ERROR: " << strerror(errno) << endl;
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    close(sockfd);
}

// --- THREAD 4: EMERGENCY (TRIGGERS SHUTDOWN) ---
void task_emergency() {
    std::unique_lock<std::mutex> l(shared_state.state_mutex);
    shared_state.cv_emergency.wait(l, []{ return shared_state.emergency_triggered; });
    
    cout << "\n\n!!! EMERGENCY STOP ACTIVATED !!!" << endl;
    
    {
        std::lock_guard<std::mutex> sl(global_stats.stats_mutex);
        global_stats.emergency_status = "ACTIVE";
    }
    
    // Wait 2 seconds to show final stats
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    // Trigger cleanup and exit
    cleanup_and_exit();
}

// --- THREAD 5: MONITOR ---
void task_monitor() {
    cout << "\n----------------------------------------------------------------------------------------------------" << endl;
    cout << "| FLIGHT (Prio 50)       | NETWORK (Prio 30)     | VISION (Prio 10)    | SYSTEM STATUS          |" << endl;
    cout << "| Time  | Miss | Preempt | Packets | Preempt   | FPS  | Preempt    | ALT   | THR | EMERGENCY  |" << endl;
    cout << "----------------------------------------------------------------------------------------------------" << endl;

    while(system_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        long f_time, f_miss, f_pre, n_pack, n_pre, v_fps, v_pre;
        float alt, thr;
        string emerg_status;
        
        {
            std::lock_guard<std::mutex> l(global_stats.stats_mutex);
            f_time = global_stats.flight_exec_avg_us;
            f_miss = global_stats.flight_deadline_misses;
            f_pre = global_stats.flight_preempts;
            n_pack = global_stats.net_packets;
            n_pre = global_stats.net_preempts;
            v_fps = global_stats.vision_fps;
            v_pre = global_stats.vision_preempts;
            emerg_status = global_stats.emergency_status;
            global_stats.net_packets = 0;
        }
        {
            std::lock_guard<std::mutex> l(shared_state.state_mutex);
            alt = shared_state.altitude;
            thr = shared_state.throttle;
        }

        cout << "| " << setw(5) << f_time << " | " << setw(4) << f_miss 
             << " | " << setw(7) << f_pre 
             << " | " << setw(7) << n_pack 
             << " | " << setw(9) << n_pre 
             << " | " << setw(4) << v_fps 
             << " | " << setw(10) << v_pre
             << " | " << setw(5) << fixed << setprecision(1) << alt << " | " 
             << setw(3) << (int)thr << " | " << setw(10) << emerg_status << " |" << endl;
    }
}

// --- SIGNAL HANDLER (Ctrl+C) ---
void signal_handler(int signum) {
    cout << "\n\nReceived signal " << signum << endl;
    cleanup_and_exit();
}

int main(int argc, char* argv[]) {
    // Register signal handler for Ctrl+C
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    int target_core = -1;
    if (argc > 1 && string(argv[1]) == "1") target_core = 0;

    cout << "--- DRONE CORE ONLINE ---" << endl;
    cout << "Press Ctrl+C or send PANIC to shutdown cleanly\n" << endl;

    std::thread t1(task_flight);
    std::thread t2(task_vision);
    std::thread t3(task_networking);
    std::thread t4(task_emergency);
    std::thread t5(task_monitor);

    set_priority(t4, 90, "Emergency");
    set_priority(t1, 50, "Flight");
    set_priority(t3, 30, "Networking");
    set_priority(t2, 10, "Vision");

    if (target_core != -1) {
        pin_thread(t1, target_core);
        pin_thread(t2, target_core);
        pin_thread(t3, target_core);
        pin_thread(t4, target_core);
    }

    t1.join(); t2.join(); t3.join(); t4.join(); t5.join();
    return 0;
}