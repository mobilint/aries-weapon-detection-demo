#include "demo/demo.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "demo/benchmarker.h"
#include "demo/feeder.h"
#include "demo/model.h"
#include "opencv2/opencv.hpp"
#include "qbruntime/qbruntime.h"

using mobilint::Accelerator;
using mobilint::StatusCode;
using namespace std;

namespace {
void sleepForMS(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

class InFlightGuard {
   public:
    explicit InFlightGuard(std::atomic<uint8_t>* slot) : mSlot(slot) {}

    bool tryAcquire() {
        if (mSlot == nullptr) return false;
        uint8_t expected = 0;
        mAcquired =
            mSlot->compare_exchange_strong(expected, 1, std::memory_order_relaxed);
        return mAcquired;
    }

    ~InFlightGuard() {
        if (mAcquired && mSlot != nullptr) {
            mSlot->store(0, std::memory_order_relaxed);
        }
    }

   private:
    std::atomic<uint8_t>* mSlot = nullptr;
    bool mAcquired = false;
};

cv::Rect getTimeBoxRect(const cv::Size& size) {
    const int w = 260;
    const int h = 44;
    const int top_band_h = 137;
    const int x = std::max(0, size.width - w - 280);
    const int y = std::max(0, (top_band_h - h) / 2);
    return cv::Rect(x, y, std::min(w, size.width - x), std::min(h, size.height - y));
}

std::string secToString(int sec) {
    int h = sec / 3600;
    int m = (sec % 3600) / 60;
    int s = sec % 60;

    char buf[20];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    return std::string(buf);
}

void displayBenchmark(Item& item) {
    const int pad = 5;
    const int box_w = 110;
    const int box_h = 40;

    cv::Rect box(pad, pad, box_w, box_h);
    cv::Mat roi = item.img(box);
    cv::Mat overlay = cv::Mat::zeros(roi.size(), roi.type());
    cv::addWeighted(overlay, 0.60, roi, 0.40, 0, roi);

    char fps_val[24];
    char npu_val[24];
    std::snprintf(fps_val, sizeof(fps_val), "%.2f", item.fps);
    std::snprintf(npu_val, sizeof(npu_val), "%.1fms", item.time);

    const double font_scale = 0.5;
    const int thickness = 1;
    const int lx = box.x + 4;
    int baseline = 0;
    int label_w = std::max(
        cv::getTextSize("FPS", cv::FONT_HERSHEY_DUPLEX, font_scale, thickness, &baseline)
            .width,
        cv::getTextSize("NPU", cv::FONT_HERSHEY_DUPLEX, font_scale, thickness, &baseline)
            .width);
    const int rx = lx + label_w + 8;
    const int y1 = box.y + 16;
    const int y2 = box.y + box_h - 8;

    cv::putText(item.img, "FPS", cv::Point(lx, y1), cv::FONT_HERSHEY_DUPLEX, font_scale,
                cv::Scalar(230, 230, 230), thickness, cv::LINE_AA);
    cv::putText(item.img, fps_val, cv::Point(rx, y1), cv::FONT_HERSHEY_DUPLEX, font_scale,
                cv::Scalar(0, 255, 0), thickness, cv::LINE_AA);
    cv::putText(item.img, "NPU", cv::Point(lx, y2), cv::FONT_HERSHEY_DUPLEX, font_scale,
                cv::Scalar(230, 230, 230), thickness, cv::LINE_AA);
    cv::putText(item.img, npu_val, cv::Point(rx, y2), cv::FONT_HERSHEY_DUPLEX, font_scale,
                cv::Scalar(0, 255, 0), thickness, cv::LINE_AA);
}

void displayTime(cv::Mat& display, bool validate, float time = 0.0f) {
    if (!validate) return;
    cv::Rect box = getTimeBoxRect(display.size());
    if (box.width <= 0 || box.height <= 0) return;

    cv::Mat roi = display(box);
    cv::Mat overlay = cv::Mat::zeros(roi.size(), roi.type());
    cv::addWeighted(overlay, 0.65, roi, 0.35, 0, roi);

    const double font_scale = 0.9;
    const int thickness = 1;
    cv::putText(display, "Time", cv::Point(box.x + 12, box.y + 30),
                cv::FONT_HERSHEY_DUPLEX, font_scale, cv::Scalar(255, 255, 255), thickness,
                cv::LINE_AA);
    cv::putText(display, secToString((int)time), cv::Point(box.x + 94, box.y + 30),
                cv::FONT_HERSHEY_DUPLEX, font_scale, cv::Scalar(0, 255, 0), thickness,
                cv::LINE_AA);
}
}  // namespace

Demo::Demo() : mDisplayFPSMode(false), mDisplayTimeMode(false), mModeIndex(-1) {}

float Demo::smoothDisplayFPS(int worker_index, float instant_fps) {
    if (worker_index < 0 || (size_t)worker_index >= mWorkerDisplayFPSHistory.size() ||
        (size_t)worker_index >= mWorkerDisplayFPSSum.size()) {
        return instant_fps;
    }

    auto& history = mWorkerDisplayFPSHistory[worker_index];
    float& sum = mWorkerDisplayFPSSum[worker_index];

    if (instant_fps > 0.0f) {
        history.push_back(instant_fps);
        sum += instant_fps;
        if (history.size() > DISPLAY_FPS_AVG_WINDOW) {
            sum -= history.front();
            history.pop_front();
        }
    }

    if (history.empty()) {
        return 0.0f;
    }
    return sum / static_cast<float>(history.size());
}

void Demo::startWorker(int index) {
    if (index < 0 || index >= (int)mLayoutSetting.worker_layout.size()) {
        return;
    }
    if ((size_t)index >= mWorkerEnabledSize || !mWorkerEnabled) return;
    mWorkerEnabled[index].store(1, std::memory_order_relaxed);
}

void Demo::stopWorker(int index) {
    if (index < 0 || index >= (int)mLayoutSetting.worker_layout.size()) {
        return;
    }
    if ((size_t)index >= mWorkerEnabledSize || !mWorkerEnabled) return;
    mWorkerEnabled[index].store(0, std::memory_order_relaxed);
}

void Demo::startWorkerAll() {
    const size_t n = mLayoutSetting.worker_layout.size();
    ensureWorkerEnabledStorage(n);
    for (size_t i = 0; i < n; i++) {
        mWorkerEnabled[i].store(1, std::memory_order_relaxed);
    }
}

void Demo::stopWorkerAll() {
    const size_t n = mLayoutSetting.worker_layout.size();
    ensureWorkerEnabledStorage(n);
    for (size_t i = 0; i < n; i++) {
        mWorkerEnabled[i].store(0, std::memory_order_relaxed);
    }
}

void Demo::ensureWorkerEnabledStorage(size_t n) {
    if (mWorkerEnabledSize == n && mWorkerEnabled) return;
    mWorkerEnabled = std::make_unique<std::atomic<uint8_t>[]>(n);
    mWorkerEnabledSize = n;
}

void Demo::startFeeder(int index) {
    if (index < 0 || index >= (int)mFeeders.size()) {
        return;
    }

    if ((size_t)index < mFeederThreads.size() && mFeederThreads[index].joinable()) {
        return;
    }

    mFeeders[index]->start();
    mFeederThreads[index] = std::thread(&Feeder::produceFrames, mFeeders[index].get());
}

void Demo::stopFeeder(int index) {
    if (index < 0 || index >= (int)mFeeders.size()) {
        return;
    }

    mFeeders[index]->stop();
    if ((size_t)index < mFeederThreads.size() && mFeederThreads[index].joinable()) {
        mFeederThreads[index].join();
    }
}

void Demo::startFeederAll() {
    if (mFeeders.empty()) return;
    if (mFeederThreads.size() != mFeeders.size()) {
        mFeederThreads.clear();
        mFeederThreads.resize(mFeeders.size());
    }

    for (int i = 0; i < (int)mFeeders.size(); i++) {
        startFeeder(i);
    }
}

void Demo::stopFeederAll() {
    for (int i = 0; i < (int)mFeeders.size(); i++) {
        stopFeeder(i);
    }
}

void Demo::startProcessing() {
    if (mProcessingOn.exchange(true)) {
        return;
    }
    mInferThreads.clear();
    for (size_t mi = 0; mi < mModelSetting.size(); mi++) {
        int core_count = mModelSetting[mi].num_core;
        if (core_count <= 0) continue;
        for (int ci = 0; ci < core_count; ci++) {
            mInferThreads.emplace_back(&Demo::modelInferLoop, this, mi);
        }
    }
}

void Demo::stopProcessing() {
    if (!mProcessingOn.exchange(false)) {
        return;
    }
    for (auto& t : mInferThreads) {
        if (t.joinable()) t.join();
    }
    mInferThreads.clear();
}

int Demo::getWorkerIndex(int x, int y) {
    for (int i = 0; i < mLayoutSetting.worker_layout.size(); i++) {
        if (mLayoutSetting.worker_layout[i].roi.contains(cv::Point(x, y))) {
            return i;
        }
    }
    return -1;
}

void Demo::onMouseEvent(int event, int x, int y, int flags, void* ctx) {
    if (event != cv::EVENT_RBUTTONDOWN && event != cv::EVENT_LBUTTONDOWN) {
        return;
    }

    Demo* demo = (Demo*)ctx;
    int worker_index = demo->getWorkerIndex(x, y);
    if (worker_index == -1) {
        return;
    }

    switch (event) {
        case cv::EVENT_RBUTTONDOWN:
            demo->stopWorker(worker_index);
            break;
        case cv::EVENT_LBUTTONDOWN:
            demo->startWorker(worker_index);
            break;
    }
}

bool Demo::tryProcessWorker(int worker_index) {
    if (worker_index < 0 || (size_t)worker_index >= mLayoutSetting.worker_layout.size()) {
        return false;
    }
    if ((size_t)worker_index < mWorkerEnabledSize && mWorkerEnabled &&
        mWorkerEnabled[worker_index].load(std::memory_order_relaxed) == 0) {
        return false;
    }
    if ((size_t)worker_index >= mWorkerInFlightSize || !mWorkerInFlight) return false;
    if ((size_t)worker_index >= mWorkerLastFrameIndex.size()) return false;
    if ((size_t)worker_index >= mWorkerInferBench.size()) return false;
    if ((size_t)worker_index >= mWorkerDisplayFPSBench.size()) return false;

    const auto& wl = mLayoutSetting.worker_layout[worker_index];
    if (wl.feeder_index < 0 || wl.feeder_index >= (int)mFeeders.size()) return false;
    if ((size_t)wl.model_index >= mModels.size() || !mModels[wl.model_index])
        return false;

    InFlightGuard guard(&mWorkerInFlight[worker_index]);
    if (!guard.tryAcquire()) return false;

    cv::Mat frame;
    if (!mFeeders[wl.feeder_index]->consumeFrame(frame,
                                                 mWorkerLastFrameIndex[worker_index])) {
        return false;
    }
    if (frame.empty()) return false;

    const cv::Size out_size = wl.roi.size();
    Benchmarker& infer_bench = mWorkerInferBench[worker_index];
    infer_bench.start();
    cv::Mat result = mModels[wl.model_index]->inference(frame, out_size, worker_index);
    infer_bench.end();
    if (result.empty() || result.size() != out_size) return false;

    Benchmarker& display_fps_bench = mWorkerDisplayFPSBench[worker_index];
    float display_fps = 0.0f;
    if (display_fps_bench.isStarted()) {
        display_fps_bench.end();
        display_fps = smoothDisplayFPS(worker_index, display_fps_bench.getFPS());
    } else {
        display_fps = smoothDisplayFPS(worker_index, 0.0f);
    }
    display_fps_bench.start();
    const float infer_ms = infer_bench.getSec() * 1000.0f;
    const size_t infer_count = infer_bench.getCount();

    if (mDisplayFPSMode) {
        Item item{worker_index, result, display_fps, infer_ms, infer_count};
        displayBenchmark(item);
        result = item.img;
    }

    mRenderQueue.push({worker_index, result, display_fps, infer_ms, infer_count});
    return true;
}

void Demo::modelInferLoop(size_t model_index) {
    if (model_index >= mWorkersByModel.size()) {
        return;
    }

    while (mProcessingOn.load(std::memory_order_relaxed)) {
        const auto& workers = mWorkersByModel[model_index];
        const size_t worker_count = workers.size();
        if (worker_count == 0) {
            sleepForMS(1);
            continue;
        }

        size_t start = 0;
        if (model_index < mModelWorkerCursorSize && mModelWorkerCursor) {
            start =
                mModelWorkerCursor[model_index].fetch_add(1, std::memory_order_relaxed) %
                worker_count;
        }
        bool did_work = false;

        for (size_t wi = 0; wi < worker_count; wi++) {
            const int worker_index = workers[(start + wi) % worker_count];
            if (tryProcessWorker(worker_index)) {
                did_work = true;
                break;
            }
        }

        if (!did_work) {
            sleepForMS(1);
        }
    }
}

void Demo::initWindow() {
    cv::Size window_size(1920, 1080);

    // Init Window
    cv::namedWindow(WINDOW_NAME, cv::WINDOW_GUI_NORMAL);
    cv::resizeWindow(WINDOW_NAME, window_size / 2);
    cv::moveWindow(WINDOW_NAME, 0, 0);
    cv::setMouseCallback(WINDOW_NAME, onMouseEvent, this);

    mDisplay = cv::Mat(window_size, CV_8UC3, {255, 255, 255});
    mDisplayBase = mDisplay.clone();

    mSplashes.clear();
    for (string path : {"../rc/layout/splash_01.png", "../rc/layout/splash_02.png"}) {
        cv::Mat splash = cv::imread(path);
        cv::resize(splash, splash, cv::Size(1920, 1080));
        mSplashes.push_back(splash);
    }
}

void Demo::initLayout(std::string path) {
    mLayoutSetting = loadLayoutSettingYAML(path);

    mDisplayBase.setTo(cv::Scalar(255, 255, 255));

    // Draw Banner
    for (const auto& il : mLayoutSetting.image_layout) {
        il.img.copyTo(mDisplayBase(il.roi));
    }

    {
        unique_lock<mutex> lock(mDisplayMutex);
        mDisplayBase.copyTo(mDisplay);
    }

    {
        const size_t n = mLayoutSetting.worker_layout.size();
        mWorkerEnabled = std::make_unique<std::atomic<uint8_t>[]>(n);
        mWorkerEnabledSize = n;
        mWorkerInFlight = std::make_unique<std::atomic<uint8_t>[]>(n);
        mWorkerInFlightSize = n;
        for (size_t i = 0; i < n; i++) {
            mWorkerEnabled[i].store(1, std::memory_order_relaxed);
            mWorkerInFlight[i].store(0, std::memory_order_relaxed);
        }
    }
    mWorkerInferBench.assign(mLayoutSetting.worker_layout.size(), Benchmarker());
    mWorkerDisplayFPSBench.assign(mLayoutSetting.worker_layout.size(), Benchmarker());
    for (auto& display_fps_bench : mWorkerDisplayFPSBench) {
        display_fps_bench.start();
    }
    mWorkerDisplayFPSHistory.assign(mLayoutSetting.worker_layout.size(), {});
    mWorkerDisplayFPSSum.assign(mLayoutSetting.worker_layout.size(), 0.0f);
    mWorkerLastFrameIndex.assign(mLayoutSetting.worker_layout.size(), 0);
    mRenderQueue.clear();

    checkLayoutValidation();
}

void Demo::initFeeders(std::string path) {
    mFeederSetting = loadFeederSettingYAML(path);

    stopFeederAll();
    mFeeders.resize(mFeederSetting.size());
    mFeederThreads.clear();
    mFeederThreads.resize(mFeederSetting.size());
    for (int i = 0; i < mFeederSetting.size(); i++) {
        mFeeders[i] = make_unique<Feeder>(mFeederSetting[i]);
    }

    checkLayoutValidation();
}

void Demo::initModels(std::string path) {
    mModelSetting = loadModelSettingYAML(path);

    mModels.clear();
    mAccs.clear();
    mModels.resize(mModelSetting.size());
    for (int i = 0; i < mModelSetting.size(); i++) {
        int dev_no = mModelSetting[i].dev_no;
        auto it = mAccs.find(dev_no);
        if (it == mAccs.end()) {
            StatusCode sc;
            mAccs.emplace(dev_no, Accelerator::create(dev_no, sc));
        }
        mModels[i] = std::make_unique<Model>(mModelSetting[i], *mAccs[dev_no]);
    }
    checkLayoutValidation();
}

void Demo::checkLayoutValidation() {
    mWorkerLayoutValid.assign(mLayoutSetting.worker_layout.size(), 0);
    mWorkersByModel.assign(mModels.size(), {});
    mModelWorkerCursor = std::make_unique<std::atomic<uint32_t>[]>(mModels.size());
    mModelWorkerCursorSize = mModels.size();
    for (size_t i = 0; i < mModelWorkerCursorSize; i++) {
        mModelWorkerCursor[i].store(0, std::memory_order_relaxed);
    }
    if (mModels.empty() || mFeeders.empty()) {
        return;
    }

    for (size_t wi = 0; wi < mLayoutSetting.worker_layout.size(); wi++) {
        const auto& wl = mLayoutSetting.worker_layout[wi];

        bool valid = true;
        if (wl.feeder_index < 0 || wl.feeder_index >= (int)mFeeders.size()) valid = false;
        if (wl.model_index < 0 || wl.model_index >= (int)mModels.size()) valid = false;

        if (valid) {
            mWorkerLayoutValid[wi] = 1;
            if (wl.model_index >= 0 && (size_t)wl.model_index < mWorkersByModel.size()) {
                mWorkersByModel[wl.model_index].push_back((int)wi);
            }
        } else {
            printf(
                "[WARNING] Worker[%zu]: Invalid index detected (Feeder:%d, Model:%d)\n",
                wi, wl.feeder_index, wl.model_index);
        }
    }
}

void Demo::display() {
    Item item;
    const size_t n = mLayoutSetting.worker_layout.size();
    std::vector<Item> latest(n);
    std::vector<uint8_t> valid(n, 0);
    while (mRenderQueue.tryPop(item) == ItemQueue::OK) {
        if (item.index < 0 || (size_t)item.index >= n) continue;
        latest[item.index] = std::move(item);
        valid[item.index] = 1;
    }

    unique_lock<mutex> lock(mDisplayMutex);
    for (size_t wi = 0; wi < n; wi++) {
        if (!valid[wi]) continue;
        const auto& roi = mLayoutSetting.worker_layout[wi].roi;
        if (latest[wi].img.empty() || latest[wi].img.size() != roi.size()) continue;
        latest[wi].img.copyTo(mDisplay(roi));
    }
    if (mDisplayTimeMode) {
        displayTime(mDisplay, true, mBenchmarker.getTimeSinceCreated());
    }
    cv::imshow(WINDOW_NAME, mDisplay);
}

void Demo::toggleDisplayFPSMode() { mDisplayFPSMode = !mDisplayFPSMode; }

void Demo::toggleDisplayTimeMode() {
    mDisplayTimeMode = !mDisplayTimeMode;
    if (!mDisplayTimeMode) {
        std::lock_guard<std::mutex> lk(mDisplayMutex);
        cv::Rect box = getTimeBoxRect(mDisplay.size());
        if (box.width > 0 && box.height > 0 && box.x >= 0 && box.y >= 0 &&
            box.x + box.width <= mDisplay.cols && box.y + box.height <= mDisplay.rows) {
            mDisplayBase(box).copyTo(mDisplay(box));
        }
    }
}

void Demo::toggleScreenSize() {
    int cur = cv::getWindowProperty(WINDOW_NAME, cv::WND_PROP_FULLSCREEN);
    cv::setWindowProperty(WINDOW_NAME, cv::WND_PROP_FULLSCREEN, !cur);

    cv::Size window_size(1920, 1080);
    cv::resizeWindow(WINDOW_NAME, window_size / 2);
}

bool Demo::keyHandler(int key) {
    if (cv::getWindowProperty(WINDOW_NAME, cv::WND_PROP_AUTOSIZE) != 0) {
        return false;
    }

    if (key == -1) {
        return true;
    }

    if (key >= 128) {  // Numpad 반환값은 128을 빼서 사용
        key -= 128;
    }

    key = tolower(key);

    if (key == 'd') {
        toggleDisplayFPSMode();
    } else if (key == 't') {
        toggleDisplayTimeMode();
    } else if (key == 'm') {  // 'M'aximize Screen
        toggleScreenSize();
    } else if (key == 'c') {  // 'C'lear
        stopWorkerAll();
    } else if (key == 'f') {  // 'F'ill Grid
        startWorkerAll();
    } else if (key == 'q' || key == 27) {  // 'Q'uit, esc
        return false;
    } else if (key == '1' || key == '2' || key == '3') {
        setMode(key - '0');
    }

    return true;
}

void Demo::setMode(int mode_index) {
    // clang-format off
    switch (mode_index) {
        case 1: setMode1(); break;
        case 2: setMode2(); break;
        case 3: setMode3(); break;
    }
    // clang-format on
}

void Demo::showSplash(int splash_index) {
    if (splash_index < 0 || splash_index >= (int)mSplashes.size()) {
        return;
    }
    unique_lock<mutex> lock(mDisplayMutex);
    mSplashes[splash_index].copyTo(mDisplay);
    cv::imshow(WINDOW_NAME, mDisplay);
    cv::waitKey(100);
}

void Demo::applyMode(int mode_index, const std::string& layout_path,
                     const std::string& model_path, bool reload_model, int splash_index) {
    if (mModeIndex == mode_index) {
        return;
    }

    stopProcessing();
    stopWorkerAll();
    showSplash(splash_index);
    initLayout(layout_path);
    if (reload_model) {
        initModels(model_path);
    }
    startWorkerAll();
    startProcessing();
    mModeIndex = mode_index;
    sleepForMS(500);
}

void Demo::setMode1() {
    const bool reload_model = (mModeIndex == -1 || mModeIndex == 3);
    applyMode(1, "../rc/LayoutSetting.yaml", "../rc/ModelSetting.yaml", reload_model, 0);
}

void Demo::setMode2() {
    const bool reload_model = (mModeIndex == -1 || mModeIndex == 3);
    applyMode(2, "../rc/LayoutSetting2.yaml", "../rc/ModelSetting.yaml", reload_model, 0);
}

void Demo::setMode3() {
    applyMode(3, "../rc/LayoutSetting3.yaml", "../rc/ModelSetting2.yaml", true, 1);
}

void Demo::run() {
    initWindow();
    initLayout("../rc/LayoutSetting_MLA100.yaml");
    initModels("../rc/ModelSetting_MLA100.yaml");
    initFeeders("../rc/FeederSetting_MLA100.yaml");

    startFeederAll();
    startWorkerAll();
    startProcessing();

    toggleScreenSize();

    while (true) {
        display();
        if (!keyHandler(cv::waitKey(10))) {
            break;
        }
    }

    stopProcessing();
    stopFeederAll();
    cv::destroyAllWindows();
}

int main(int argc, char* argv[]) {
    Demo demo;
    demo.run();

    return 0;
}
