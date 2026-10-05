/*
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LenovoCover"

#include <aidl/android/frameworks/sensorservice/BnEventQueueCallback.h>
#include <aidl/android/frameworks/sensorservice/ISensorManager.h>
#include <android-base/unique_fd.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mutex>
#include <vector>

using aidl::android::frameworks::sensorservice::BnEventQueueCallback;
using aidl::android::frameworks::sensorservice::IEventQueue;
using aidl::android::frameworks::sensorservice::ISensorManager;
using aidl::android::hardware::sensors::Event;
using aidl::android::hardware::sensors::SensorInfo;

namespace {
constexpr int kHallType = 33171002;
constexpr char kSensorService[] = "android.frameworks.sensorservice.ISensorManager/default";

// A dead connection cannot deliver an on-change initial state. Let init reconnect.
void sensorServiceDied(void*) {
    ALOGE("Sensor service died; restarting cover listener");
    _exit(EXIT_FAILURE);
}

android::base::unique_fd createLid() {
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(open("/dev/uinput", O_WRONLY | O_NONBLOCK)));
    if (fd < 0 || ioctl(fd, UI_SET_EVBIT, EV_SW) < 0 ||
        ioctl(fd, UI_SET_SWBIT, SW_LID) < 0) {
        ALOGE("Cannot configure uinput: %s", strerror(errno));
        return {};
    }
    uinput_setup setup{};
    strcpy(setup.name, "lenovo-cover");
    setup.id.bustype = BUS_VIRTUAL;
    if (ioctl(fd, UI_DEV_SETUP, &setup) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        ALOGE("Cannot create lid switch: %s", strerror(errno));
        return {};
    }
    return fd;
}

class CoverCallback : public BnEventQueueCallback {
  public:
    CoverCallback(int fd, int handle) : mFd(fd), mHandle(handle) {}

    ndk::ScopedAStatus onEvent(const Event& event) override {
        if (event.sensorHandle != mHandle || static_cast<int>(event.sensorType) != kHallType ||
            event.payload.getTag() != Event::EventPayload::Tag::data) {
            return ndk::ScopedAStatus::ok();
        }
        const float value = event.payload.get<Event::EventPayload::Tag::data>().values[0];
        // 0 = closed, 1 = open; reject malformed events.
        if (value != 0.0f && value != 1.0f) {
            ALOGW("Ignoring invalid hall value %f", value);
            return ndk::ScopedAStatus::ok();
        }
        std::lock_guard lock(mMutex);
        mClosed = value == 0.0f;
        reportLocked();
        return ndk::ScopedAStatus::ok();
    }

  private:
    void reportLocked() {
        if (mClosed < 0) return;  // Wait for the sensor's initial state.
        const int closed = mClosed;
        if (closed == mReported) return;

        input_event events[2]{};
        events[0].type = EV_SW;
        events[0].code = SW_LID;
        events[0].value = closed;
        events[1].type = EV_SYN;
        events[1].code = SYN_REPORT;
        if (TEMP_FAILURE_RETRY(write(mFd, events, sizeof(events))) !=
            static_cast<ssize_t>(sizeof(events))) {
            ALOGE("Cannot report lid state: %s", strerror(errno));
            _exit(EXIT_FAILURE);
        }
        mReported = closed;
        ALOGI("Lid %s", closed ? "closed" : "open");
    }

    const int mFd;
    const int mHandle;
    std::mutex mMutex;
    int mClosed = -1;
    int mReported = -1;
};
}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(1);
    ABinderProcess_startThreadPool();
    const ndk::SpAIBinder binder(AServiceManager_waitForService(kSensorService));
    const auto manager = ISensorManager::fromBinder(binder);
    if (!manager) return EXIT_FAILURE;

    ndk::ScopedAIBinder_DeathRecipient death(AIBinder_DeathRecipient_new(sensorServiceDied));
    if (AIBinder_linkToDeath(binder.get(), death.get(), nullptr) != STATUS_OK) {
        ALOGE("Cannot monitor sensor service death");
        return EXIT_FAILURE;
    }

    std::vector<SensorInfo> sensors;
    auto status = manager->getSensorList(&sensors);
    if (!status.isOk()) {
        ALOGE("Cannot list sensors: %s", status.getDescription().c_str());
        return EXIT_FAILURE;
    }
    int handle = -1;
    for (const auto& sensor : sensors) {
        if (static_cast<int>(sensor.type) == kHallType &&
            sensor.typeAsString == "qti.sensor.hall_effect" &&
            (sensor.flags & SensorInfo::SENSOR_FLAG_BITS_WAKE_UP)) {
            handle = sensor.sensorHandle;
            break;
        }
    }
    if (handle < 0) {
        ALOGE("No wakeup hall sensor; retrying in 30 seconds");
        return EXIT_FAILURE;
    }

    const auto fd = createLid();
    if (fd < 0) return EXIT_FAILURE;
    const auto callback = ndk::SharedRefBase::make<CoverCallback>(fd.get(), handle);
    std::shared_ptr<IEventQueue> queue;
    status = manager->createEventQueue(callback, &queue);
    if (!status.isOk() || !queue) {
        ALOGE("Cannot create sensor queue: %s", status.getDescription().c_str());
        return EXIT_FAILURE;
    }
    status = queue->enableSensor(handle, 200000, 0);
    if (!status.isOk()) {
        ALOGE("Cannot enable hall sensor: %s", status.getDescription().c_str());
        return EXIT_FAILURE;
    }

    ALOGI("Listening to wakeup hall handle %#x", handle);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
