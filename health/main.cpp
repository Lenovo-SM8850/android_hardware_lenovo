/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#define LOG_TAG "android.hardware.health-service.qti"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/strings.h>
#include <ctime>
#include <android/binder_interface_utils.h>
#include <cutils/klog.h>
#include <cutils/properties.h>
#include <health/utils.h>
#include <health-impl/ChargerUtils.h>
#include <health-impl/Health.h>

#define ARRAY_SIZE(x)     (sizeof(x) / sizeof((x)[0]))

typedef enum soc_id {
        MSM_NEO_LA = 554,
        MSM_NEO_LE = 525,
        MSM_NEO_LA_V2 = 579,
        MSM_SERAPH = 673,
        MSM_SERAPHP = 672,
}soc_id_t;

static const enum soc_id target_no_psy[] = {
        MSM_NEO_LA,
        MSM_NEO_LE,
        MSM_NEO_LA_V2,
        MSM_SERAPH,
        MSM_SERAPHP,
};

using aidl::android::hardware::health::HalHealthLoop;
using aidl::android::hardware::health::Health;

#if !CHARGER_FORCE_NO_UI
using aidl::android::hardware::health::charger::ChargerCallback;
using aidl::android::hardware::health::charger::ChargerModeMain;
namespace aidl::android::hardware::health {
class ChargerCallbackImpl : public ChargerCallback {
  public:
    ChargerCallbackImpl(const std::shared_ptr<Health>& service) : ChargerCallback(service) {}
    bool ChargerEnableSuspend() override { return true; }
};
} //namespace aidl::android::hardware::health
#endif

static constexpr const char* gInstanceName = "default";
static constexpr std::string_view gChargerArg{"--charger"};

static constexpr std::array<std::string_view, 3> ucsiPSYNames = {
        "ucsi-source-psy-soc:qcom,pmic_glink:qcom,ucsi1",
        "ucsi-source-psy-soc:qcom,pmic_glink:qcom,ucsi2",
        "ucsi-source-psy-soc:pmic-glink:ucsi-glink1"
};

#define RETRY_COUNT    100

void qti_healthd_board_init(struct healthd_config *hc)
{
    FILE *fp = NULL;
    int fd;
    unsigned char retries = RETRY_COUNT;
    int ret = 0;
    unsigned char buf;
    char prop_str[PROPERTY_VALUE_MAX];
    int soc_id_prop = 0;
    bool is_no_batt_psy;
    int soc = property_get_int32("ro.vendor.qti.soc_id", -1);

    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYNames[0]));
    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYNames[1]));
    hc->ignorePowerSupplyNames.push_back(android::String8(ucsiPSYNames[2]));

    is_no_batt_psy = property_get_bool("persist.vendor.hal_health.no_batt_psy", false);

    if (soc <= 0 && (fp = fopen("/sys/devices/soc0/soc_id", "r")) != NULL) {
        fscanf(fp, "%u", &soc);
        fclose(fp);
    }
    soc_id_prop = soc;

    if (!is_no_batt_psy) {
        for (int idx = 0; idx < ARRAY_SIZE(target_no_psy); idx++) {
             if(soc_id_prop == target_no_psy[idx]) {
                KLOG_INFO(LOG_TAG, "no support for batt_psy with socid:%d \n",soc_id_prop);
                return;
           }
        }
    } else {
        KLOG_INFO(LOG_TAG, "no support for batt_psy\n");
        return;
    }

retry:
    if (!retries) {
        KLOG_ERROR(LOG_TAG, "Cannot open battery/capacity, fd=%d\n", fd);
        return;
    }

    fd = open("/sys/class/power_supply/battery/capacity", 0440);
    if (fd >= 0) {
        KLOG_INFO(LOG_TAG, "opened battery/capacity after %d retries\n", RETRY_COUNT - retries);
        while (retries) {
            ret = read(fd, &buf, 1);
            if(ret >= 0) {
                KLOG_INFO(LOG_TAG, "Read Batt Capacity after %d retries ret : %d\n", RETRY_COUNT - retries, ret);
                close(fd);
                return;
            }

            retries--;
            usleep(100000);
        }

        KLOG_ERROR(LOG_TAG, "Failed to read Battery Capacity ret=%d\n", ret);
        close(fd);
        return;
    }

    retries--;
    usleep(100000);
    goto retry;
}

namespace {

int64_t ReadBatteryDate(const char* path) {
    std::string text;
    int value = 0;
    if (!android::base::ReadFileToString(path, &text) ||
        !android::base::ParseInt(android::base::Trim(text), &value) || value <= 0 ||
        value > 991231) {
        return 0;
    }

    // Lenovo reports calendar dates as yyMMdd, not Unix timestamps.
    const int year = 100 + value / 10000;
    const int month = value / 100 % 100 - 1;
    const int day = value % 100;
    if (month < 0 || month > 11 || day < 1 || day > 31) return 0;
    std::tm date = {};
    date.tm_year = year;
    date.tm_mon = month;
    date.tm_mday = day;
    const time_t seconds = timegm(&date);
    if (seconds < 0 || date.tm_year != year || date.tm_mon != month || date.tm_mday != day) {
        return 0;
    }
    return seconds;
}

class LenovoHealth final : public Health {
  public:
    using Health::Health;

    ndk::ScopedAStatus getChargingPolicy(
            aidl::android::hardware::health::BatteryChargingPolicy* /* out */) override {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    ndk::ScopedAStatus getBatteryHealthData(
            aidl::android::hardware::health::BatteryHealthData* out) override {
        auto status = Health::getBatteryHealthData(out);
        if (!status.isOk()) return status;
        out->batteryManufacturingDateSeconds =
                ReadBatteryDate("/sys/class/power_supply/battery/produce_date");
        out->batteryFirstUsageSeconds =
                ReadBatteryDate("/sys/class/power_supply/battery/activate_date");
        if (out->batteryStateOfHealth < 0 || out->batteryStateOfHealth > 100) {
            out->batteryStateOfHealth = 0;
        }
        return status;
    }

    void UpdateHealthInfo(aidl::android::hardware::health::HealthInfo* info) override {
        aidl::android::hardware::health::BatteryHealthData data;
        if (getBatteryHealthData(&data).isOk()) info->batteryHealthData = std::move(data);
    }
};

}  // namespace

int main(int argc, char** argv) {
#ifdef __ANDROID_RECOVERY__
    android::base::InitLogging(argv, android::base::KernelLogger);
#endif
    auto config = std::make_unique<healthd_config>();
    ::android::hardware::health::InitHealthdConfig(config.get());
    qti_healthd_board_init(config.get());
    config->batteryStateOfHealthPath = android::String8("/sys/class/power_supply/battery/soh");
    auto binder = ndk::SharedRefBase::make<LenovoHealth>(gInstanceName, std::move(config));

    if (argc >= 2 && argv[1] == gChargerArg) {
#if !CHARGER_FORCE_NO_UI
        KLOG_INFO(LOG_TAG, "Starting charger mode with UI.");
        auto charger_callback = std::make_shared<aidl::android::hardware::health::ChargerCallbackImpl>(binder);
        return ChargerModeMain(binder, charger_callback);
#endif
        KLOG_INFO(LOG_TAG, "Starting charger mode without UI.");
    } else {
        KLOG_INFO(LOG_TAG, "Starting health HAL.");
    }

    auto hal_health_loop = std::make_shared<HalHealthLoop>(binder, binder);
    return hal_health_loop->StartLoop();
}
