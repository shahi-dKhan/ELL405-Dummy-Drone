// data_collector.cpp
// Timeline data collection version - adds CSV logging to working drone_core

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
#include <cmath>
#include <fstream>

#include <opencv2/opencv.hpp>

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
    long vision_frames = 0;
    long vision_preempts = 0;
    string emergency_status = "STANDBY";
    std::mutex stats_mutex;
};

DroneState shared_state;
SystemStats global_stats;
std::atomic<bool> system_running(true);
std::mutex console_mutex;
std::ofstream timeline_log;

// --- CLEANUP FUNCTION ---
void cleanup_resources() {
    {
        std::lock_guard<std::mutex> log_lock(console_mutex);
        cout << "\n=== EMERGENCY SHUTDOWN SEQUENCE ===" << endl;
    }
    
    {
        std::lock_guard<std::mutex> l(shared_state.state_mutex);
        shared_state.throttle = 0;
        shared_state.pitch = 0;
        shared_state.roll = 0;
        shared_state.yaw = 0;
    }
    
    system("pkill -9 rpicam-vid 2>/dev/null");
    
    {
        std::lock_guard<std::mutex> log_lock(console_mutex);
        cout << "✓ Motors stopped" << endl;
        cout << "✓ Camera stopped" << endl;
        
        std::lock_guard<std::mutex> l(global_stats.stats_mutex);
        cout << "\n--- FINAL STATS ---" << endl;
        cout << "Flight loops: " << global_stats.flight_loops << endl;
        cout << "Vision Frames: " << global_stats.vision_frames << endl;
    }
    
    cout << "\n✓ Shutdown complete" << endl;
    cout << "=================================\n" << endl;
}

// --- HELPERS ---
long get_kernel_preemptions() {
    struct rusage usage;
    getrusage(RUSAGE_THREAD, &usage);
    return usage.ru_nivcsw; 
}

void log_timeline_event(const string& thread_name, const string& event_type, long preempt_count) {
    auto now = std::chrono::steady_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    
    if (timeline_log.is_open()) {
        timeline_log << timestamp << "," 
                    << thread_name << "," 
                    << event_type << "," 
                    << preempt_count << "\n";
    }
}

void set_priority(std::thread &th, int prio, string name) {
    sched_param param;
    param.sched_priority = prio;
    if (pthread_setschedparam(th.native_handle(), SCHED_FIFO, &param) != 0) {
        std::lock_guard<std::mutex> log_lock(console_mutex);
        cerr << "[Scheduler] FAILED " << name << endl;
    }
}

void pin_thread(std::thread &th, int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(th.native_handle(), sizeof(cpu_set_t), &cpuset);
}

// --- THREAD 1: FLIGHT CONTROL (High Prio 50) ---
void task_flight() {
    auto next_wake = std::chrono::steady_clock::now();
    const float dt = 0.01f;
    long last_preempt_count = 0;

    while (system_running) {
        next_wake += std::chrono::milliseconds(10);
        
        long current_preempts = get_kernel_preemptions();
        log_timeline_event("Flight", "START", current_preempts);
        
        if (current_preempts > last_preempt_count) {
            log_timeline_event("Flight", "PREEMPTED", current_preempts);
            last_preempt_count = current_preempts;
        }

        if (std::chrono::steady_clock::now() > next_wake) {
            std::lock_guard<std::mutex> l(global_stats.stats_mutex);
            global_stats.flight_deadline_misses++;
            log_timeline_event("Flight", "DEADLINE_MISS", current_preempts);
        }

        auto start = std::chrono::high_resolution_clock::now();

        {
            std::lock_guard<std::mutex> l(shared_state.state_mutex);
            
            if (shared_state.emergency_triggered) {
                shared_state.throttle = 0; shared_state.pitch = 0; shared_state.roll = 0;
            }

            float temp_alt = shared_state.altitude;
            for(int i = 0; i < 2000; i++) temp_alt += 0.0001f * sin(i * 0.001f);

            float lift = shared_state.throttle * 0.25f; 
            float gravity = 9.81f;
            float tilt_factor = 1.0f - (abs(shared_state.pitch) + abs(shared_state.roll)) * 0.005f;
            
            float accel = (lift * tilt_factor) - gravity;
            shared_state.velocity += accel * dt;
            shared_state.altitude += shared_state.velocity * dt;

            if (shared_state.altitude < 0) { shared_state.altitude = 0; shared_state.velocity = 0; }
        }

        auto end = std::chrono::high_resolution_clock::now();
        long dur = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        long preempts = get_kernel_preemptions();

        {
            std::lock_guard<std::mutex> l(global_stats.stats_mutex);
            global_stats.flight_loops++;
            global_stats.flight_exec_avg_us = (global_stats.flight_exec_avg_us + dur) / 2;
            global_stats.flight_preempts = preempts;
        }
        
        log_timeline_event("Flight", "END", preempts);

        std::this_thread::sleep_until(next_wake);
    }
}

// --- THREAD 2: VISION (Low Prio 10) ---
void task_vision() {
    long last_preempt_count = 0;
    
    system("pkill -9 rpicam-vid 2>/dev/null");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    string cmd = "chrt -f 10 rpicam-vid -t 0 --nopreview --inline --width 640 --height 480 --framerate 30 --codec libav --libav-format mpegts --listen -o tcp://0.0.0.0:8888 > /dev/null 2>&1 &";
    system(cmd.c_str());

    {
        std::lock_guard<std::mutex> l(console_mutex);
        cout << "[Vision] Camera Server Started (No Window). Starting CPU Burner..." << endl;
    }
    
    log_timeline_event("Vision", "START", get_kernel_preemptions());

    cv::Mat fake_frame(480, 640, CV_8UC3, cv::Scalar(0,0,0));

    while (system_running) {
        long current_preempts = get_kernel_preemptions();
        
        if (current_preempts > last_preempt_count) {
            log_timeline_event("Vision", "PREEMPTED", current_preempts);
            last_preempt_count = current_preempts;
        }
        
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
    
    system("pkill -9 rpicam-vid");
    log_timeline_event("Vision", "END", get_kernel_preemptions());
}

// --- THREAD 3: NETWORKING (Mid Prio 30) ---
void task_networking() {
    long last_preempt_count = 0;
    
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY; 
    servaddr.sin_port = htons(LOCAL_PORT);
    
    bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr));
    fcntl(sockfd, F_SETFL, fcntl(sockfd, F_GETFL, 0) | O_NONBLOCK);

    {
        std::lock_guard<std::mutex> l(console_mutex);
        cout << "[Net] Listening on Port " << LOCAL_PORT << endl;
    }
    
    log_timeline_event("Network", "START", get_kernel_preemptions());

    char buffer[1024];
    while (system_running) {
        int n = recvfrom(sockfd, buffer, 1024, 0, NULL, NULL);
        if (n > 0) {
            buffer[n] = '\0';
            string cmd(buffer);

            long current_preempts = get_kernel_preemptions();
            
            if (current_preempts > last_preempt_count) {
                log_timeline_event("Network", "PREEMPTED", current_preempts);
                last_preempt_count = current_preempts;
            }
            
            log_timeline_event("Network", "PACKET_RX", current_preempts);

            {
                std::lock_guard<std::mutex> l(console_mutex);
                cout << "[CMD] " << cmd << endl;
            }

            long preempts = get_kernel_preemptions();
            {
                std::lock_guard<std::mutex> l(global_stats.stats_mutex);
                global_stats.net_packets++;
                global_stats.net_preempts = preempts;
            }

            std::lock_guard<std::mutex> l(shared_state.state_mutex);
            
            if (cmd == "PANIC") {
                {
                    std::lock_guard<std::mutex> cl(console_mutex);
                    cout << "EMERGENCY!" << endl;
                }
                log_timeline_event("Network", "EMERGENCY", current_preempts);
                shared_state.emergency_triggered = true;
                shared_state.cv_emergency.notify_one();
                {
                    std::lock_guard<std::mutex> sl(global_stats.stats_mutex);
                    global_stats.emergency_status = "TRIGGERED";
                }
            }
            else if (cmd == "UP") {
                shared_state.throttle = min(100.0f, shared_state.throttle + 10.0f);
                { std::lock_guard<std::mutex> cl(console_mutex); cout << "Thr " << (int)shared_state.throttle << "%" << endl; }
            }
            else if (cmd == "DOWN") {
                shared_state.throttle = max(0.0f, shared_state.throttle - 10.0f);
                { std::lock_guard<std::mutex> cl(console_mutex); cout << "Thr " << (int)shared_state.throttle << "%" << endl; }
            }
            else if (cmd == "FRONT") {
                shared_state.pitch = 15.0f;
                { std::lock_guard<std::mutex> cl(console_mutex); cout << "Pitch FORWARD" << endl; }
            }
            else if (cmd == "BACK") {
                shared_state.pitch = -15.0f;
                { std::lock_guard<std::mutex> cl(console_mutex); cout << "Pitch BACKWARD" << endl; }
            }
            else if (cmd == "LEFT") {
                shared_state.roll = -15.0f;
                { std::lock_guard<std::mutex> cl(console_mutex); cout << "Roll LEFT" << endl; }
            }
            else if (cmd == "RIGHT") {
                shared_state.roll = 15.0f;
                { std::lock_guard<std::mutex> cl(console_mutex); cout << "Roll RIGHT" << endl; }
            }
            else if (cmd == "STOP") {
                shared_state.pitch = 0.0f; shared_state.roll = 0.0f;
                { std::lock_guard<std::mutex> cl(console_mutex); cout << "CENTERED" << endl; }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    log_timeline_event("Network", "END", get_kernel_preemptions());
    close(sockfd);
}

// --- THREAD 4: EMERGENCY (Critical Prio 90) ---
void task_emergency() {
    log_timeline_event("Emergency", "WAITING", 0);
    
    std::unique_lock<std::mutex> l(shared_state.state_mutex);
    shared_state.cv_emergency.wait(l, []{ return shared_state.emergency_triggered; });
    
    log_timeline_event("Emergency", "TRIGGERED", get_kernel_preemptions());
    
    {
        std::lock_guard<std::mutex> log_lock(console_mutex);
        cout << "\n\n!!! EMERGENCY STOP ACTIVATED !!!" << endl;
    }
    
    {
        std::lock_guard<std::mutex> sl(global_stats.stats_mutex);
        global_stats.emergency_status = "ACTIVE";
    }
    
    shared_state.throttle = 0;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    system_running = false;
}

// --- THREAD 5: MONITOR ---
void task_monitor() {
    std::this_thread::sleep_for(std::chrono::seconds(1)); 
    
    {
        std::lock_guard<std::mutex> log_lock(console_mutex);
        cout << "\n--------------------------------------------------------------------------------" << endl;
        cout << "| FLIGHT (Prio 50)      | NETWORK (Prio 30)   | VISION (Prio 10)    | STATUS   |" << endl;
        cout << "| Time(us) | Miss | Pre | Packets | Preempt   | FPS  | Preempt      | EMERG?   |" << endl;
        cout << "--------------------------------------------------------------------------------" << endl;
    }

    while(system_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        long f_time, f_miss, f_pre, n_pack, n_pre, v_fps, v_pre;
        string emg;
        
        {
            std::lock_guard<std::mutex> l(global_stats.stats_mutex);
            f_time = global_stats.flight_exec_avg_us;
            f_miss = global_stats.flight_deadline_misses;
            f_pre = global_stats.flight_preempts;
            n_pack = global_stats.net_packets;
            n_pre = global_stats.net_preempts;
            v_fps = global_stats.vision_frames;
            v_pre = global_stats.vision_preempts;
            emg = global_stats.emergency_status;
            
            global_stats.vision_frames = 0;
            global_stats.net_packets = 0;
        }

        {
            std::lock_guard<std::mutex> l(console_mutex);
            cout << "| " << setw(8) << f_time << " | " << setw(4) << f_miss 
                 << " | " << setw(3) << f_pre << " | " << setw(7) << n_pack 
                 << " | " << setw(9) << n_pre 
                 << " | " << setw(4) << v_fps 
                 << " | " << setw(10) << v_pre 
                 << " | " << setw(8) << emg << " |" << endl;
        }
    }
}

// --- MAIN ---
void signal_handler(int signum) { system_running = false; }

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Open timeline log file
    timeline_log.open("timeline_data.csv");
    if (timeline_log.is_open()) {
        timeline_log << "timestamp_ms,thread,event,preempt_count\n";
        cout << "[Logger] Timeline data -> timeline_data.csv" << endl;
    }

    int target_core = -1;
    if (argc > 1 && string(argv[1]) == "1") target_core = 0;

    cout << "=== DRONE CORE (DATA COLLECTOR) ===" << endl;
    if(target_core == 0) cout << "[MODE] SINGLE CORE STRESS TEST (Core 0)" << endl;
    else cout << "[MODE] MULTI CORE" << endl;

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
    
    if (timeline_log.is_open()) {
        timeline_log.close();
        cout << "[Logger] Data saved: timeline_data.csv" << endl;
    }
    
    cleanup_resources();
    return 0;
}