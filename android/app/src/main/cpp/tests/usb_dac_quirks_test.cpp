#include "usb_dac_quirks.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

// 对拍 Kotlin UsbDacQuirksTest 的匹配用例与 UsbPreferredMixerTest 全部用例
//（JSON 解析仍在 Kotlin 侧，对应测试保留在 UsbDacQuirksTest）。

namespace {

void keyFormatsMatchKotlinHex() {
    assert(sylvakru::dacQuirkKey(0x20b1, 0x0002) == "0x20b1:0x0002");
    assert(sylvakru::dacQuirkKey(0x262a, -1) == "0x262a:*");
    // 4 位补零、超 16 位不截断，与 Kotlin "0x%04x".format 一致
    assert(sylvakru::dacQuirkKey(0x1, 0x2) == "0x0001:0x0002");
    assert(sylvakru::dacQuirkKey(0x12345, 0x2) == "0x12345:0x0002");
}

void exactMatchBeatsVendorWildcard() {
    const std::vector<std::string> keys = {"0x20b1:0x0002", "0x262a:*"};
    // 精确命中
    assert(sylvakru::matchDacQuirkIndex(keys, 0x20b1, 0x0002) == 0);
    // 厂商通配：同 vid 不同 pid
    assert(sylvakru::matchDacQuirkIndex(keys, 0x262a, 0x9999) == 1);
    // 未命中
    assert(sylvakru::matchDacQuirkIndex(keys, 0x1234, 0x5678) == -1);
}

void exactKeyWinsEvenWhenListedAfterWildcard() {
    // 厂商目录里 `pid:*` 在前、精确条目在后：精确匹配整表优先于通配
    const std::vector<std::string> keys = {"0x262a:*", "0x262a:0x1001"};
    assert(sylvakru::matchDacQuirkIndex(keys, 0x262a, 0x1001) == 1);
    assert(sylvakru::matchDacQuirkIndex(keys, 0x262a, 0x9999) == 0);
}

void overrideEntryWinsWhenListedFirst() {
    // override 条目在前，同 key 先命中
    const std::vector<std::string> keys = {"0x20b1:0x0002", "0x20b1:0x0002", "0x262a:*"};
    assert(sylvakru::matchDacQuirkIndex(keys, 0x20b1, 0x0002) == 0);
}

void vendorCatalogMatchesExplicitDevicesOnly() {
    // 对拍 enablesIbassoHidOnlyForExplicitDeviceEntries 的匹配语义
    const std::vector<std::string> keys = {"0x262a:0x1001", "0x262a:0x1002", "0x262a:*"};
    assert(sylvakru::matchDacQuirkIndex(keys, 0x262a, 0x1001) == 0);
    assert(sylvakru::matchDacQuirkIndex(keys, 0x262a, 0x1002) == 1);
    assert(sylvakru::matchDacQuirkIndex(keys, 0x262a, 0x9999) == 2);
    assert(sylvakru::matchDacQuirkIndex(keys, 0x1234, 0x9999) == -1);
}

void requestedRateDoesNotFallBackToAnotherBitPerfectRate() {
    assert(sylvakru::chooseBitPerfectMixerSampleRate(96000, {48000}) == -1);
}

void requestedRateUsesExactBitPerfectRateWhenAvailable() {
    assert(sylvakru::chooseBitPerfectMixerSampleRate(96000, {48000, 96000}) == 96000);
}

void missingRequestedRateUsesHighestBitPerfectRate() {
    assert(sylvakru::chooseBitPerfectMixerSampleRate(-1, {44100, 96000, 48000}) == 96000);
}

void invalidRatesAreIgnored() {
    assert(sylvakru::chooseBitPerfectMixerSampleRate(-1, {0, -48000}) == -1);
    assert(sylvakru::chooseBitPerfectMixerSampleRate(-1, {}) == -1);
    assert(sylvakru::chooseBitPerfectMixerSampleRate(0, {48000}) == -1);
}

}  // namespace

int main() {
#define RUN(test)                         \
    do {                                  \
        std::fprintf(stderr, #test "\n"); \
        test();                           \
    } while (false)
    RUN(keyFormatsMatchKotlinHex);
    RUN(exactMatchBeatsVendorWildcard);
    RUN(exactKeyWinsEvenWhenListedAfterWildcard);
    RUN(overrideEntryWinsWhenListedFirst);
    RUN(vendorCatalogMatchesExplicitDevicesOnly);
    RUN(requestedRateDoesNotFallBackToAnotherBitPerfectRate);
    RUN(requestedRateUsesExactBitPerfectRateWhenAvailable);
    RUN(missingRequestedRateUsesHighestBitPerfectRate);
    RUN(invalidRatesAreIgnored);
#undef RUN
    std::fprintf(stderr, "all tests passed\n");
    return 0;
}
