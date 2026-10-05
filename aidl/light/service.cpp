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
using ::aidl::android::hardware::light::LightRing;

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
    auto ring = ndk::SharedRefBase::make<LightRing>(lights);
    // Keep the configured Binder alive: asBinder() only caches a weak reference.
    const ndk::SpAIBinder ringBinder = ring->asBinder();
    AIBinder_setRequestingSid(ringBinder.get(), true);
    const std::string ringInstance = std::string(LightRing::descriptor) + "/default";
    CHECK(AServiceManager_addService(ringBinder.get(), ringInstance.c_str()) == STATUS_OK);
    ABinderProcess_joinThreadPool();
    lights->turnOff();
    return EXIT_FAILURE;
}
