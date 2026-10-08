// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "thpd"
#define BIONIC_IOCTL_NO_SIGNEDNESS_OVERLOAD

#include <log/log.h>

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

namespace {
using ReportCallback = int (*)(void*);
using Init = int (*)(void*);
using Start = int (*)();
using Exit = int (*)();
using SetCallback = void (*)(ReportCallback);

constexpr size_t kParamsSize = 0x31c;
constexpr size_t kPathSize = 0x105;
constexpr size_t kLibraryDirOffset = 0x000;
constexpr size_t kLogDirOffset = 0x105;
constexpr size_t kFirmwareDirOffset = 0x20a;
constexpr size_t kLogBufferEnableOffset = 0x30f;
constexpr size_t kLogLevelOffset = 0x310;
constexpr size_t kStockFlagsOffset = 0x314;
constexpr size_t kFileLogEnableOffset = 0x316;
constexpr size_t kDeviceIdOffset = 0x318;
#ifdef THPD_WUJI_RECOVERY
constexpr char kLibraryDir[] = "/system/lib64/";
constexpr char kFirmwareDir[] = "/system/etc/thp-recovery/";
constexpr char kTscLibrary[] = "/system/lib64/libgdix_tsc.so";
#else
constexpr char kLibraryDir[] = "/vendor/lib64/";
constexpr char kFirmwareDir[] = "/vendor/firmware/";
constexpr char kTscLibrary[] = "/vendor/lib64/libgdix_tsc.so";
#endif

static_assert(sizeof(void*) == 8, "The stock THP ABI requires arm64");
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "The stock THP ABI is little-endian");
static_assert(sizeof(kLibraryDir) <= kPathSize && sizeof(kFirmwareDir) <= kPathSize);
static_assert(kFirmwareDirOffset + kPathSize == kLogBufferEnableOffset);
static_assert(kDeviceIdOffset + sizeof(uint32_t) == kParamsSize);

// The kernel accepts an opaque 504-byte input report.
constexpr unsigned long kReportIoctl = 0xc1f8b901UL;
constexpr uint32_t kSendMessageIoctl = 0x4088b811;
constexpr char kScreenState[] = "/sys/bus/platform/devices/gt9976n_thp.0/screen_state";
constexpr char kBrightness[] = "/sys/class/backlight/panel0-backlight/actual_brightness";
int agentFd = -1;
int thpFd = -1;
sem_t hostReady;

bool panelIsOn() {
    const int fd = TEMP_FAILURE_RETRY(open(kBrightness, O_RDONLY | O_CLOEXEC));
    if (fd < 0) {
        ALOGE("Cannot read panel brightness: %s", strerror(errno));
        return false;
    }
    char value[32] = {};
    const ssize_t size = TEMP_FAILURE_RETRY(read(fd, value, sizeof(value) - 1));
    close(fd);
    return size > 0 && strtol(value, nullptr, 10) > 0;
}

void syncScreenState() {
    // Real panel blank/unblank owns controller suspend/resume in the kernel:
    // Stock .text+0x2a04 arms gestures before type-5 off; 0x2acc resumes after on.
    // This synthetic startup handshake only synchronizes TSC's screen bit.
    // B810 alone returns immediately after AFE reinit: the kernel is already
    // in normal mode. screen_state sends the same type-5 request as DRM work
    // (stock gt9976n_thp_core: goodix_thp_screen_store, .text+0x5af4).
    // TSC starts with its screen bit clear, so send off before the first on
    // to run its normal TSA/AFE initialization, without changing the display.
    if (!panelIsOn()) {
        ALOGI("Panel is off; waiting for the normal DRM resume event");
        return;
    }
    const int fd = TEMP_FAILURE_RETRY(open(kScreenState, O_WRONLY | O_CLOEXEC));
    if (fd < 0) {
        ALOGE("Cannot open THP screen state: %s", strerror(errno));
        return;
    }
    uint32_t state = UINT32_MAX;
    if (ioctl(thpFd, static_cast<int>(0x8004b80a), &state) != 0 || state != 0) {
        ALOGI("THP driver is not in normal mode (%u); keeping DRM state", state);
    } else if (TEMP_FAILURE_RETRY(write(fd, "0", 1)) != 1) {
        ALOGE("Cannot initialize THP screen state: %s", strerror(errno));
    } else if (panelIsOn() && ioctl(thpFd, static_cast<int>(0x8004b80a), &state) == 0 &&
               state == 0) {
        if (TEMP_FAILURE_RETRY(pwrite(fd, "1", 1, 0)) != 1) {
            ALOGE("Cannot resume THP screen state: %s", strerror(errno));
        } else {
            ALOGI("Synchronized initial THP screen state after HAL ready");
        }
    }
    close(fd);
}

int reportTouch(void* report) {
    return ioctl(agentFd, static_cast<int>(kReportIoctl), report);
}

template <typename Function>
Function loadSymbol(void* library, const char* name) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    if (error || !symbol) {
        ALOGE("Cannot resolve %s: %s", name, error ? error : "null symbol");
        return nullptr;
    }
    return reinterpret_cast<Function>(symbol);
}

void* runTsc(void* opaque) {
    const auto start = *static_cast<Start*>(opaque);
    ALOGI("Starting Goodix THP worker");
    const int result = start();
    ALOGE("thp_start returned %d; init will retry in 30 seconds", result);
    return nullptr;
}
}  // namespace

// Observe the existing stock handshake instead of guessing the startup delay.
// Goodix supplies a third argument for every ioctl. Forward it unchanged and
// signal only successful cmd101: TSC .text+0xbcc0 -> AFE .text+0xdaf8 -> B811.
extern "C" __attribute__((visibility("default"))) int ioctl(int fd, int request, ...) {
    va_list args;
    va_start(args, request);
    void* argument = va_arg(args, void*);
    va_end(args);
    const int result = static_cast<int>(syscall(SYS_ioctl, fd, request, argument));
    if (result == 0 && static_cast<uint32_t>(request) == kSendMessageIoctl && argument) {
        uint32_t message[2];
        memcpy(message, argument, sizeof(message));
        if (message[0] == 101 && message[1] == 0) {
            thpFd = fd;
            sem_post(&hostReady);
        }
    }
    return result;
}

int main() {
    if (sem_init(&hostReady, 0, 0) != 0) {
        ALOGE("Cannot create HAL-ready semaphore: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    agentFd = TEMP_FAILURE_RETRY(open("/dev/input_agent", O_RDWR | O_CLOEXEC));
    if (agentFd < 0) {
        ALOGE("Cannot open /dev/input_agent: %s", strerror(errno));
        return EXIT_FAILURE;
    }

    void* library = dlopen(kTscLibrary, RTLD_LAZY | RTLD_LOCAL);
    if (!library) {
        ALOGE("Cannot load %s: %s", kTscLibrary, dlerror());
        close(agentFd);
        return EXIT_FAILURE;
    }
    const auto init = loadSymbol<Init>(library, "thp_init");
    auto start = loadSymbol<Start>(library, "thp_start");
    const auto exit = loadSymbol<Exit>(library, "thp_exit");
    const auto setCallback = loadSymbol<SetCallback>(library, "thp_set_touch_report_callback");
    if (!init || !start || !exit || !setCallback) {
        dlclose(library);
        close(agentFd);
        return EXIT_FAILURE;
    }

    alignas(uint32_t) uint8_t params[kParamsSize] = {};
    memcpy(params + kLibraryDirOffset, kLibraryDir, sizeof(kLibraryDir));
    memcpy(params + kLogDirOffset, kFirmwareDir, sizeof(kFirmwareDir));
    memcpy(params + kFirmwareDirOffset, kFirmwareDir, sizeof(kFirmwareDir));
    params[kLogBufferEnableOffset] = 0;
    params[kFileLogEnableOffset] = 0;
    const uint32_t logLevel = 3;
    const uint16_t stockFlags = 0x100;
    const uint32_t deviceId = 0;
    memcpy(params + kLogLevelOffset, &logLevel, sizeof(logLevel));
    memcpy(params + kStockFlagsOffset, &stockFlags, sizeof(stockFlags));
    memcpy(params + kDeviceIdOffset, &deviceId, sizeof(deviceId));

    const int result = init(params);
    if (result != 0) {
        ALOGE("thp_init failed: %d", result);
        dlclose(library);
        close(agentFd);
        return EXIT_FAILURE;
    }
    setCallback(reportTouch);

    pthread_t worker;
    int error = pthread_create(&worker, nullptr, runTsc, &start);
    if (error != 0) {
        ALOGE("Cannot create THP worker: %s", strerror(error));
        return EXIT_FAILURE;
    }
    timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 30;
    if (TEMP_FAILURE_RETRY(sem_timedwait(&hostReady, &deadline)) == 0) {
        syncScreenState();
    } else {
        ALOGE("THP HAL-ready handshake timed out: %s", strerror(errno));
        return EXIT_FAILURE;
    }
    error = pthread_join(worker, nullptr);
    if (error != 0) {
        ALOGE("Cannot join THP worker: %s", strerror(error));
    }
    // thp_exit is a stub in the analyzed TSC. Other blob threads may still run;
    // keep their library and agent fd alive until process exit, then let init retry.
    return EXIT_FAILURE;
}
