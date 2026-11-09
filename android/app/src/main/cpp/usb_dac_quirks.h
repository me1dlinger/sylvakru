#pragma once

#include <string>
#include <vector>

namespace sylvakru {

// DAC quirk 键匹配与位完美混音采样率选择：从 Kotlin UsbDacQuirks/UsbPreferredMixer
// 下沉的平台无关纯逻辑（tests/usb_dac_quirks_test.cpp 对拍）。
// quirk 的 JSON/资产装载与 DacQuirk 字段解析仍在 Kotlin 侧；跨 JNI 只传键列表，
// 匹配返回命中下标，DacQuirk 对象本体不过界。

// 匹配键 "0x%04x:0x%04x"；product_id 为 -1 时生成厂商通配键 "0x%04x:*"
//（与 Kotlin matchKey/hex 格式一致，超过 16 位的值不截断）。
std::string dacQuirkKey(int vendor_id, int product_id);

// 按 `vid:pid` 精确匹配 → `vid:*` 厂商匹配两级查找（keys 序即优先序，
// override 条目在前先命中）。返回命中下标，未命中返回 -1。
int matchDacQuirkIndex(
    const std::vector<std::string>& keys,
    int vendor_id,
    int product_id);

// 位完美混音采样率选择：忽略非正值；requested_sample_rate >= 0 时必须恰好命中
//（不允许回退到其它位完美速率），-1（未指定）时取支持列表中的最大值。
// 无可用速率返回 -1（对应 Kotlin null）。
int chooseBitPerfectMixerSampleRate(
    int requested_sample_rate,
    const std::vector<int>& supported_sample_rates);

}  // namespace sylvakru
