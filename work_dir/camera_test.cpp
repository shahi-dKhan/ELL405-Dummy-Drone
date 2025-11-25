#include <opencv2/opencv.hpp>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <chrono>

int main() {
    std::string pipe_path = "/tmp/drone_pipe";

    // 1. CLEANUP: Remove old pipe and kill old camera processes
    system("pkill -9 rpicam-vid");
    unlink(pipe_path.c_str()); // Delete old pipe file if exists

    // 2. CREATE PIPE (This is the bridge between command and OpenCV)
    // 0666 = Read/Write permissions
    if (mkfifo(pipe_path.c_str(), 0666) != 0) {
        std::cerr << "[Error] Failed to create pipe file!" << std::endl;
        return 1;
    }

    std::cout << "[System] Pipe created at " << pipe_path << std::endl;

    // 3. EXECUTE CAMERA COMMAND (Runs in background via '&')
    // We send output to the pipe_path we just created.
    // We redirect stderr to /dev/null to keep the stream clean.
    std::string cmd = "rpicam-vid -t 0 --inline --width 640 --height 480 --framerate 30 --codec h264 -n -o " + pipe_path + " 2> /dev/null &";
    
    std::cout << "[System] Executing camera command..." << std::endl;
    system(cmd.c_str());

    // 4. OPEN OPENCV
    // Now OpenCV just opens the pipe file. It doesn't need to understand the command.
    std::cout << "[Vision] Connecting to pipe..." << std::endl;
    cv::VideoCapture cap(pipe_path, cv::CAP_FFMPEG);

    if (!cap.isOpened()) {
        std::cerr << "[Error] OpenCV cannot open the pipe." << std::endl;
        return 1;
    }

    std::cout << "[Vision] SUCCESS! Stream locked." << std::endl;

    cv::Mat frame;
    while (true) {
        cap >> frame;
        
        if (frame.empty()) {
            std::cout << "[Vision] Stream ended." << std::endl;
            break;
        }

        std::cout << "Frame: " << frame.cols << "x" << frame.rows << "\r" << std::flush;
        
        // Uncomment to see video if you have a monitor
        // cv::imshow("Drone", frame);
        // if (cv::waitKey(1) == 27) break;
    }

    // Cleanup
    cap.release();
    system("pkill rpicam-vid");
    unlink(pipe_path.c_str());
    return 0;
}