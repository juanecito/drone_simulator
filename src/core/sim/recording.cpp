// SPDX-FileCopyrightText: 2026 Juan Maria Gomez Lopez <juan-maria.gomez-lopez@tutamail.com>
// SPDX-License-Identifier: MIT

#include "ds/sim/recording.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace ds::sim {
namespace {

using common::Json;

constexpr double kAnalogScale = 32767.0;
constexpr std::size_t kAnalogCount = 6;
constexpr std::uint8_t kFlagsChanged = 1U << 6U;

double quantize(double v) noexcept {
    return std::round(std::clamp(v, -1.0, 1.0) * kAnalogScale) / kAnalogScale;
}

std::array<double, kAnalogCount> analog(const input::ControlInputs& c) noexcept {
    return {c.throttle, c.roll, c.pitch, c.yaw, c.drive, c.steering};
}

std::array<double*, kAnalogCount> analogRefs(input::ControlInputs& c) noexcept {
    return {&c.throttle, &c.roll, &c.pitch, &c.yaw, &c.drive, &c.steering};
}

std::uint16_t flags(const input::ControlInputs& c) noexcept {
    std::uint16_t f = 0;
    const std::array<bool, 9> bits{
        c.brake,         c.arm,    c.beeper,    c.returnToHome, c.payloadRelease,
        c.cameraTrigger, c.lights, c.linkValid, false};
    for (std::size_t i = 0; i < bits.size(); ++i) {
        f = static_cast<std::uint16_t>(f | (bits[i] ? 1U << i : 0U));
    }
    return f;
}

void setFlags(input::ControlInputs& c, std::uint16_t f) noexcept {
    const auto bit = [f](unsigned i) { return (f & (1U << i)) != 0U; };
    c.brake = bit(0);
    c.arm = bit(1);
    c.beeper = bit(2);
    c.returnToHome = bit(3);
    c.payloadRelease = bit(4);
    c.cameraTrigger = bit(5);
    c.lights = bit(6);
    c.linkValid = bit(7);
}

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    out.push_back(static_cast<std::uint8_t>(v >> 8U));
}

double num(const Json& j, const char* key, double fallback) {
    return j.contains(key) && j[key].is_number() ? j[key].get<double>() : fallback;
}

}  // namespace

input::ControlInputs quantized(const input::ControlInputs& controls) noexcept {
    input::ControlInputs q = controls;
    for (double* v : analogRefs(q)) {
        *v = quantize(*v);
    }
    q.throttle = std::max(q.throttle, -1.0);
    return q;
}

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'D', 'S', 'R', 'I'};

}  // namespace

InputEncoder::InputEncoder() {
    bytes_.assign(kMagic.begin(), kMagic.end());
    bytes_.push_back(static_cast<std::uint8_t>(ReplayLog::kFormat));
}

void InputEncoder::add(const input::ControlInputs& c) {
    const auto values = analog(c);
    std::array<std::int16_t, kAnalogCount> q{};
    std::uint8_t mask = 0;
    for (std::size_t i = 0; i < kAnalogCount; ++i) {
        q[i] =
            static_cast<std::int16_t>(std::lround(std::clamp(values[i], -1.0, 1.0) * kAnalogScale));
        if (ticks_ == 0 || q[i] != last_[i]) {
            mask = static_cast<std::uint8_t>(mask | (1U << i));
        }
    }
    const std::uint16_t f = flags(c);
    const int mode = std::clamp(c.flightMode, 0, 255);
    if (ticks_ == 0 || f != lastFlags_ || mode != lastMode_) {
        mask |= kFlagsChanged;
    }
    bytes_.push_back(mask);
    for (std::size_t i = 0; i < kAnalogCount; ++i) {
        if ((mask & (1U << i)) != 0U) {
            putU16(bytes_, static_cast<std::uint16_t>(q[i]));
        }
    }
    if ((mask & kFlagsChanged) != 0U) {
        putU16(bytes_, f);
        bytes_.push_back(static_cast<std::uint8_t>(mode));
    }
    last_ = q;
    lastFlags_ = f;
    lastMode_ = mode;
    ++ticks_;
}

std::vector<std::uint8_t> InputEncoder::take() noexcept {
    std::vector<std::uint8_t> out = std::move(bytes_);
    *this = InputEncoder();
    return out;
}

common::Result<InputDecoder> InputDecoder::open(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kMagic.size() + 1 ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return common::makeError("replay inputs: not a replay input stream");
    }
    if (bytes[kMagic.size()] != ReplayLog::kFormat) {
        return common::makeError("replay inputs: unsupported format");
    }
    InputDecoder d(bytes);
    d.at_ = kMagic.size() + 1;
    return d;
}

std::optional<input::ControlInputs> InputDecoder::next() {
    if (damaged_ || at_ >= bytes_.size()) {
        return std::nullopt;
    }
    const auto u16 = [this](std::uint16_t& v) {
        if (at_ + 2 > bytes_.size()) {
            return false;
        }
        v = static_cast<std::uint16_t>(bytes_[at_] | (bytes_[at_ + 1] << 8U));
        at_ += 2;
        return true;
    };
    const std::uint8_t mask = bytes_[at_++];
    if ((mask & 0x80U) != 0U) {
        damaged_ = true;
        return std::nullopt;
    }
    input::ControlInputs c = current_;
    const auto refs = analogRefs(c);
    for (std::size_t k = 0; k < kAnalogCount; ++k) {
        if ((mask & (1U << k)) != 0U) {
            std::uint16_t v = 0;
            if (!u16(v)) {
                damaged_ = true;
                return std::nullopt;
            }
            *refs[k] = static_cast<double>(static_cast<std::int16_t>(v)) / kAnalogScale;
        }
    }
    if ((mask & kFlagsChanged) != 0U) {
        std::uint16_t f = 0;
        if (!u16(f) || at_ >= bytes_.size()) {
            damaged_ = true;
            return std::nullopt;
        }
        setFlags(c, f);
        c.flightMode = bytes_[at_++];
    }
    current_ = c;
    return c;
}

std::vector<std::uint8_t> encodeInputs(std::span<const input::ControlInputs> inputs) {
    InputEncoder e;
    for (const input::ControlInputs& c : inputs) {
        e.add(c);
    }
    return e.take();
}

common::Result<std::vector<input::ControlInputs>> decodeInputs(
    std::span<const std::uint8_t> bytes) {
    auto d = InputDecoder::open(bytes);
    if (!d) {
        return common::makeError(d.error().message);
    }
    std::vector<input::ControlInputs> out;
    while (auto c = d->next()) {
        out.push_back(*c);
    }
    if (d->damaged()) {
        return common::makeError("replay inputs: truncated or malformed data");
    }
    return out;
}

Json failureToJson(const physics::Failure& f) {
    Json j = {{"type", std::string(physics::failureTypeName(f.type))},
              {"magnitudeM", f.magnitudeM},
              {"driftMs", f.driftMs},
              {"motor", f.motor},
              {"efficiency", f.efficiency},
              {"fraction", f.fraction},
              {"offsetDeg", f.offsetDeg}};
    if (f.durationS) {
        j["durationS"] = *f.durationS;
    }
    return j;
}

std::optional<physics::Failure> failureFromJson(const Json& j) {
    if (!j.is_object() || !j.contains("type") || !j["type"].is_string()) {
        return std::nullopt;
    }
    const auto type = physics::parseFailureType(j["type"].get<std::string>());
    if (!type) {
        return std::nullopt;
    }
    physics::Failure f = physics::defaultFailure(*type);
    if (j.contains("durationS") && j["durationS"].is_number()) {
        f.durationS = j["durationS"].get<double>();
    }
    f.magnitudeM = num(j, "magnitudeM", f.magnitudeM);
    f.driftMs = num(j, "driftMs", f.driftMs);
    f.motor = static_cast<int>(std::lround(num(j, "motor", f.motor)));
    f.efficiency = num(j, "efficiency", f.efficiency);
    f.fraction = num(j, "fraction", f.fraction);
    f.offsetDeg = num(j, "offsetDeg", f.offsetDeg);
    return f;
}

Json ReplayLog::metaJson() const {
    Json commandList = Json::array();
    for (const ReplayCommand& c : commands) {
        Json e = c.data;
        e["tick"] = c.tick;
        commandList.push_back(std::move(e));
    }
    return {{"format", kFormat},
            {"header", header},
            {"commands", std::move(commandList)},
            {"replayable", replayable},
            {"truncated", truncated}};
}

common::Result<ReplayLog> ReplayLog::fromJson(const Json& meta) {
    if (!meta.is_object() || num(meta, "format", 0.0) != kFormat || !meta.contains("header") ||
        !meta["header"].is_object()) {
        return common::makeError("replay: unsupported or damaged replay");
    }
    ReplayLog log;
    log.header = meta["header"];
    log.replayable = meta.value("replayable", false);
    log.truncated = meta.value("truncated", false);
    for (const Json& e : meta.value("commands", Json::array())) {
        if (!e.is_object() || !e.contains("tick") || !e["tick"].is_number_unsigned()) {
            return common::makeError("replay: damaged command list");
        }
        ReplayCommand c;
        c.tick = e["tick"].get<std::uint64_t>();
        c.data = e;
        c.data.erase("tick");
        log.commands.push_back(std::move(c));
    }
    return log;
}

double ReplayLog::durationS() const {
    double dt = num(header, "dt", 0.002);
    double t = 0.0;
    std::uint64_t at = 0;
    for (const ReplayCommand& c : commands) {
        if (c.data.value("kind", "") == "dt") {
            const std::uint64_t until = std::min(c.tick, ticks);
            if (until > at) {
                t += static_cast<double>(until - at) * dt;
                at = until;
            }
            dt = num(c.data, "value", dt);
        }
    }
    if (ticks > at) {
        t += static_cast<double>(ticks - at) * dt;
    }
    return t;
}

}  // namespace ds::sim
