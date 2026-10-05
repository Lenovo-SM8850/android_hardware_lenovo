/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.lenovo"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <string>

#include "HighTouchPollingRate.h"

using aidl::vendor::lineage::touch::HighTouchPollingRate;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    const auto service = ndk::SharedRefBase::make<HighTouchPollingRate>();
    const std::string instance = std::string(HighTouchPollingRate::descriptor) + "/default";
    const binder_status_t status =
            AServiceManager_addService(service->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << instance;
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
