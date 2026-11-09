/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "vendor.lineage.touch-service.lenovo"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <string>

#include "HighTouchPollingRate.h"
#include "TouchPolicy.h"

using aidl::vendor::lenovo::hardware::touchpolicy::TouchPolicy;
using aidl::vendor::lineage::touch::HighTouchPollingRate;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    const auto polling = ndk::SharedRefBase::make<HighTouchPollingRate>();
    const std::string pollingInstance = std::string(HighTouchPollingRate::descriptor) + "/default";
    binder_status_t status =
            AServiceManager_addService(polling->asBinder().get(), pollingInstance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << pollingInstance;
    const auto policy = ndk::SharedRefBase::make<TouchPolicy>();
    const std::string policyInstance = std::string(TouchPolicy::descriptor) + "/default";
    status = AServiceManager_addService(policy->asBinder().get(), policyInstance.c_str());
    CHECK_EQ(status, STATUS_OK) << "Failed to register " << policyInstance;
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
