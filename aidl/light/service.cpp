/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Lights.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <thread>

using ::aidl::android::hardware::light::Lights;

int main() {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    CHECK(pthread_sigmask(SIG_BLOCK, &signals, nullptr) == 0);

    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto lights = ndk::SharedRefBase::make<Lights>();
    if (lights->initialize() != 0) return EXIT_FAILURE;

    std::thread([lights, signals]() mutable {
        int signal;
        if (sigwait(&signals, &signal) == 0) {
            const int error = lights->turnOff();
            std::exit(error ? EXIT_FAILURE : EXIT_SUCCESS);
        }
    }).detach();

    const std::string instance = std::string(Lights::descriptor) + "/default";
    const binder_status_t status =
            AServiceManager_addService(lights->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK);
    ABinderProcess_joinThreadPool();
    lights->turnOff();
    return EXIT_FAILURE;
}
