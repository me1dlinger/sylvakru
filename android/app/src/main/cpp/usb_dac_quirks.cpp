#include "usb_dac_quirks.h"

#include <cstdio>

namespace sylvakru {

namespace {

// 与 Kotlin "0x%04x".format(value) 一致：4 位补零，更大的值不截断
std::string hex(int value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%04x", static_cast<unsigned int>(value));
    return buffer;
}

int indexOfKey(const std::vector<std::string>& keys, const std::string& key) {
    for (size_t index = 0; index < keys.size(); ++index) {
        if (keys[index] == key) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

}  // namespace

std::string dacQuirkKey(int vendor_id, int product_id) {
    return hex(vendor_id) + ":" + (product_id >= 0 ? hex(product_id) : "*");
}

int matchDacQuirkIndex(
    const std::vector<std::string>& keys,
    int vendor_id,
    int product_id) {
    const int exact = indexOfKey(keys, dacQuirkKey(vendor_id, product_id));
    if (exact >= 0) {
        return exact;
    }
    return indexOfKey(keys, dacQuirkKey(vendor_id, -1));
}

int chooseBitPerfectMixerSampleRate(
    int requested_sample_rate,
    const std::vector<int>& supported_sample_rates) {
    if (requested_sample_rate >= 0) {
        for (const int rate : supported_sample_rates) {
            if (rate > 0 && rate == requested_sample_rate) {
                return rate;
            }
        }
        return -1;
    }
    int best = -1;
    for (const int rate : supported_sample_rates) {
        if (rate > 0 && rate > best) {
            best = rate;
        }
    }
    return best;
}

}  // namespace sylvakru
