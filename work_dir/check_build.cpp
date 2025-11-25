#include <opencv2/opencv.hpp>
#include <iostream>

int main() {
    // Try to open the file we just saved
    cv::VideoCapture cap("test_vid.h264", cv::CAP_FFMPEG);

    if (cap.isOpened()) std::cout << "SUCCESS: File opened!" << std::endl;
    else std::cerr << "FAILURE: Could not read file." << std::endl;
    return 0;
}