// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 hakoniwa
// three-band-eq.h
//
// 3 バンドイコライザ（1 チャンネル分）。OBS 標準の「3バンドイコライザ」フィルタと同じ分け方で、
// 低域（800Hz 未満）・中域・高域（5kHz 以上）に分けて、それぞれの倍率をかける。
// 低域と高域は 4 段の 1 次フィルタで取り出し、中域は「入力 − 低域 − 高域」なので、
// 3 つとも倍率 1 なら元の音に戻る。帯域の境目はゆるやかなので、中域を 0 にしても穴のようには消えない（2kHz で約 -13dB）。

#pragma once

#include <cmath>

namespace pf {

class ThreeBandEq {
public:
    void init(double sample_rate, double low_hz = 800.0, double high_hz = 5000.0) {
        const double pi = 3.14159265358979323846;
        lf_ = 2.0 * std::sin(pi * low_hz / sample_rate);
        hf_ = 2.0 * std::sin(pi * high_hz / sample_rate);
        reset();
    }

    void reset() {
        for (double &v : lp_)
            v = 0.0;
        for (double &v : hp_)
            v = 0.0;
        d1_ = d2_ = d3_ = 0.0;
    }

    float process(float in, float low_gain, float mid_gain, float high_gain) {
        constexpr double kDenormal = 1.0 / 4294967295.0; // ごく小さな値を足して、非正規化数で遅くなるのを防ぐ
        const double x = in;

        lp_[0] += lf_ * (x - lp_[0]) + kDenormal;
        lp_[1] += lf_ * (lp_[0] - lp_[1]);
        lp_[2] += lf_ * (lp_[1] - lp_[2]);
        lp_[3] += lf_ * (lp_[2] - lp_[3]);
        const double low = lp_[3];

        hp_[0] += hf_ * (x - hp_[0]) + kDenormal;
        hp_[1] += hf_ * (hp_[0] - hp_[1]);
        hp_[2] += hf_ * (hp_[1] - hp_[2]);
        hp_[3] += hf_ * (hp_[2] - hp_[3]);
        const double high = d3_ - hp_[3];
        const double mid = d3_ - (high + low);

        d3_ = d2_;
        d2_ = d1_;
        d1_ = x;
        return (float)(low * low_gain + mid * mid_gain + high * high_gain);
    }

private:
    double lf_ = 0.0, hf_ = 0.0;
    double lp_[4] = {}, hp_[4] = {};
    double d1_ = 0.0, d2_ = 0.0, d3_ = 0.0; // 入力を 3 サンプル遅らせたもの
};

} // namespace pf
