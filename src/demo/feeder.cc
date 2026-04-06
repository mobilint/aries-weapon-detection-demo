#include "demo/feeder.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "demo/benchmarker.h"
#include "demo/define.h"
#include "opencv2/opencv.hpp"

namespace {
std::string getYouTube(const std::string& youtube_url) {
#ifdef _MSC_VER
    // (kibum): Need to implement.
    std::cerr << "Youtube input is not implemented for MSVC.\n";
    return "";
#else
    char buf[128];
    std::string URL;
    std::string cmd = "yt-dlp -f \"best[height<=720][width<=1280]\" -g " + youtube_url;

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return URL;
    }

    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        URL += buf;
    }
    pclose(pipe);

    if (!URL.empty()) {
        URL.erase(URL.find('\n'));
    }
    return URL;
#endif
}
}  // namespace

Feeder::Feeder(const FeederSetting& feeder_setting) : mFeederSetting(feeder_setting) {
    switch (mFeederSetting.feeder_type) {
    case FeederType::CAMERA: {
        mCap.open(stoi(mFeederSetting.src_path), cv::CAP_V4L2);
        mCap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
        mCap.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        mCap.set(cv::CAP_PROP_FRAME_HEIGHT, 360);
        mCap.set(cv::CAP_PROP_FPS, 30);
        mDelayOn = false;
        break;
    }
    case FeederType::VIDEO: {
        mCap.open(mFeederSetting.src_path);
        mDelayOn = true;
        break;
    }
    case FeederType::IPCAMERA: {
        mCap.open(mFeederSetting.src_path);
        mDelayOn = false;
        break;
    }
    case FeederType::YOUTUBE: {
        mCap.open(getYouTube(mFeederSetting.src_path));
        mDelayOn = true;
        break;
    }
    }

    if (mFeederSetting.feeder_type == FeederType::VIDEO && mCap.isOpened()) {
        double fps = mCap.get(cv::CAP_PROP_FPS);
        if (fps >= 1.0 && fps <= 240.0) {
            mVideoFps = fps;
        }
    }
}

bool Feeder::consumeFrame(cv::Mat& frame, int64_t& frame_index) {
    int64_t latest_index = frame_index;
    auto sc = mFeederBuffer.getLatest(frame, latest_index);
    if (sc != MatBuffer::OK) {
        return false;
    }
    if (latest_index == frame_index) {
        return false;
    }
    frame_index = latest_index;
    return !frame.empty();
}

void Feeder::produceFrames() {
    mFeederBuffer.open();
    while (mIsFeederRunning.load(std::memory_order_relaxed)) {
        if (mCap.isOpened()) {
            int delay_ms = 0;
            if (mDelayOn) {
                if (mFeederSetting.feeder_type == FeederType::VIDEO && mVideoFps >= 24.0 &&
                    mVideoFps <= 240.0) {
                    delay_ms = std::max(1, (int)std::lround(1000.0 / mVideoFps));
                } else {
                    delay_ms = 33;
                }
            }
            produceFramesInternal(mCap, delay_ms);
            mCap.set(cv::CAP_PROP_POS_FRAMES, 0);
        } else {
            produceFramesInternalDummy();
        }
    }
    mFeederBuffer.close();
}

void Feeder::produceFramesInternal(cv::VideoCapture& cap, int delay_ms) {
    Benchmarker benchmarker;
    int perf_count = 0;
    while (true) {
        benchmarker.start();

        cv::Mat frame;
        cap >> frame;
        if (frame.empty() || !mIsFeederRunning.load(std::memory_order_relaxed)) {
            break;
        }

        mFeederBuffer.put(frame);

        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }

        benchmarker.end();
        if ((perf_count++ % 60) == 0) {
            // printf("[FEED] idx=%d interval=%.3fms fps=%.2f\n", index,
            //        benchmarker.getSec() * 1000, benchmarker.getFPS());
            // fflush(stdout);
        }
    }
}

void Feeder::produceFramesInternalDummy() {
    Benchmarker benchmarker;
    while (true) {
        benchmarker.start();

        cv::Mat frame;
        frame = cv::Mat::zeros(360, 640, CV_8UC3);
        cv::putText(frame, "Dummy Feeder", cv::Point(140, 190), cv::FONT_HERSHEY_DUPLEX,
                    1.5, cv::Scalar(0, 255, 0), 2);
        if (frame.empty() || !mIsFeederRunning.load(std::memory_order_relaxed)) {
            break;
        }

        mFeederBuffer.put(frame);

        std::this_thread::sleep_for(std::chrono::milliseconds(30));

        benchmarker.end();
    }
}
