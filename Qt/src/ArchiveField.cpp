#include "ArchiveField.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
double smooth(double value) {
    value = std::clamp(value, 0., 1.);
    return value * value * value * (10 + value * (-15 + 6 * value));
}
double bell(double value, double width) {
    const auto scaled = value / width;
    return std::exp(-0.5 * (scaled * scaled));
}
double archiveWave(double row, double lane, double time) {
    const auto elapsed = time - 22;
    if (elapsed <= 0 || elapsed >= 4.35) {
        return 0;
    }
    const auto phase = row + (lane - 2) * .65;
    const auto packet = [](double distance) {
        return 2.5 * bell(distance, 3.8) - .58 * bell(distance - 6, 3.5);
    };
    return smooth(elapsed / .32) *
           (packet(phase - (3 + elapsed * 19)) * (1 - smooth((elapsed - 2.15) / .65)) +
            packet(phase - (32 - (elapsed - 2.3) * 24)) * smooth((elapsed - 2.17) / .32) *
                (1 - smooth((elapsed - 3.5) / .85)));
}
double settlingWave(double distance) {
    const auto age = 26.56 - 25.05 - std::abs(distance) * .065;
    const auto envelope = std::max(-.42, 2.15 - .17 * (std::sqrt(distance * distance + 1) - 1));
    const auto ring = age > 0 ? std::sin(age * 5.1) * std::exp(-age * 1.3) : 0;
    return envelope * (smooth(age / .62) + .18 * ring * smooth(age / .16));
}
struct Pulse {
    double row;
    double lane;
    double age;
    double envelope;
};
}

/**
 * 一次计算原生资料库本帧所需的基础位移网格
 * 返回 Float64 字节，音乐频谱和接力仍由原状态层叠加
 */
QByteArray sampleArchiveField(const QVariantMap &parameters) {
    const auto number = [&](const char *key) { return parameters.value(key).toDouble(); };
    const int rows = parameters.value("rows").toInt();
    const int lanes = parameters.value("lanes").toInt();
    if (rows <= 0 || lanes <= 0 || qint64(rows) * lanes > 32768) {
        return {};
    }
    const auto row0 = number("row0");
    const auto lane0 = number("lane0");
    const auto originRow = number("originRow");
    const auto originLane = number("originLane");
    const auto scanTime = number("scanTime");
    const auto scanBlend = number("scanBlend");
    const auto shoulder = number("shoulder");
    const auto laneFocus = number("laneFocus");
    const auto idleGain = number("idleGain");
    const auto time = number("time");
    const auto flatMix = number("flatMix");
    const auto pulseGain = number("pulseGain");
    std::vector<Pulse> pulses;
    if (parameters.value("selectionWave").toBool()) {
        for (const auto &value : parameters.value("pulses").toList()) {
            const auto pulse = value.toMap();
            const auto age = time - pulse["time"].toDouble();
            if (age >= 0 && age <= 3.2) {
                pulses.push_back({pulse["row"].toDouble(), pulse["lane"].toDouble(), age,
                                  .8 * smooth(age / .2) * std::exp(-age * 1.15)});
            }
        }
    }
    constexpr double pi = 3.14159265358979323846;
    QByteArray result(rows * lanes * qsizetype(sizeof(double)), Qt::Uninitialized);
    for (int laneIndex = 0; laneIndex < lanes; ++laneIndex) {
        const auto lane = lane0 + laneIndex;
        const auto strength = 1 + ((.25 + .75 * bell(lane - laneFocus, .55)) - 1) * smooth(1);
        for (int rowIndex = 0; rowIndex < rows; ++rowIndex) {
            const auto row = row0 + rowIndex * .5;
            const auto globalRow = row + originRow;
            const auto globalLane = lane + originLane;
            const auto height = archiveWave(globalRow, globalLane, scanTime) * scanBlend;
            const auto breathing =
                (.075 * std::sin((time * pi * 2) / 8 + globalRow * .3 - globalLane * .45) +
                 .027 * std::sin((time * pi * 2) / 13 - globalRow * .17 + globalLane * .3)) *
                idleGain;
            double ripple = 0;
            for (const auto &pulse : pulses) {
                const auto distance = std::hypot(row - pulse.row, (lane - pulse.lane) * 2.2);
                const auto phase = distance - pulse.age * 8;
                ripple += pulse.envelope * std::cos(phase * .58) * bell(phase, 3.4);
            }
            const auto value = (height + settlingWave(row - shoulder) * strength) * (1 - flatMix) +
                               breathing + std::clamp(ripple, -.6, .6) * pulseGain;
            const auto offset = (laneIndex * rows + rowIndex) * qsizetype(sizeof(double));
            memcpy(result.data() + offset, &value, sizeof(value));
        }
    }
    return result;
}
