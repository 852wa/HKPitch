// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 hakoniwa
// psola.cpp
//
// 仕組みは psola.h の冒頭を参照。

#include "psola.h"

#include <algorithm>
#include <cmath>

namespace pf {

namespace {

constexpr double kPi = 3.14159265358979323846;

// パラメータを動かしたとき、この時間（秒）くらいでなめらかに新しい値へ移る
constexpr double kGlideSeconds = 0.04;

} // namespace

void PsolaShifter::init(double sample_rate, double latency_sec) {
    sr_ = sample_rate > 0 ? sample_rate : 48000.0;
    latency_ = (int)std::lround(sr_ * latency_sec);
    // グレインの片側の長さの上限。左側の分だけ早めに置き、右側の分だけ先の入力を待つので、合計が遅延に収まるようにする
    cap_half_ = latency_ * 0.42;
    min_period_ = (float)(sr_ / 900.0);
    max_period_ = (float)(sr_ / 70.0);
    unvoiced_period_ = (float)(sr_ * 0.005);
    lp_a_ = (float)std::exp(-2.0 * kPi * 1000.0 / sr_);

    int64_t size = 1;
    while (size < latency_ * 4 + (int64_t)max_period_ * 8)
        size <<= 1;
    mask_ = size - 1;
    in_.assign((size_t)size, 0.0f);
    lp_.assign((size_t)size, 0.0f);
    acc_.assign((size_t)size, 0.0f);

    // ピッチ検出は約 8kHz に間引いた信号で行う（2.4kHz より上は切る）
    dec_ = std::max(1, (int)std::lround(sr_ / 8000.0));
    const double fs_d = sr_ / dec_;
    const int taps = 12 * dec_ + 1, mid = taps / 2;
    const double fc = 0.3 / dec_;
    fir_.assign(taps, 0.0f);
    double sum = 0;
    for (int k = 0; k < taps; ++k) {
        const double x = k - mid;
        const double sinc = x == 0 ? 2 * fc : std::sin(2 * kPi * fc * x) / (kPi * x);
        const double win = 0.54 - 0.46 * std::cos(2 * kPi * k / (taps - 1));
        fir_[k] = (float)(sinc * win);
        sum += fir_[k];
    }
    for (auto &h : fir_)
        h = (float)(h / sum);

    yin_w_ = (int)std::lround(fs_d * 0.020);
    tau_max_ = (int)std::ceil(fs_d / 70.0) + 1;
    tau_min_ = std::max(2, (int)std::floor(fs_d / 900.0));
    yin_hop_ = std::max(1, (int)std::lround(fs_d * 0.004));
    int dsize = 1;
    while (dsize < yin_w_ + tau_max_ + 8)
        dsize <<= 1;
    dmask_ = dsize - 1;
    dbuf_.assign(dsize, 0.0f);
    ydiff_.assign(yin_w_ + tau_max_ + 2, 0.0f);
    reset();
}

void PsolaShifter::reset() {
    std::fill(in_.begin(), in_.end(), 0.0f);
    std::fill(lp_.begin(), lp_.end(), 0.0f);
    std::fill(acc_.begin(), acc_.end(), 0.0f);
    std::fill(dbuf_.begin(), dbuf_.end(), 0.0f);
    lp1_ = lp2_ = 0.0f;
    n_ = 0;
    nd_ = 0;
    dec_phase_ = 0;
    yin_count_ = 0;
    voiced_ = false;
    period_ = 0.0f;
    recent_n_ = 0;
    hold_left_ = 0;
    marks_.clear();
    marks_.push_back({0.0, unvoiced_period_, false});
    next_grain_ = 0.0;
    lead_ = cap_half_;
    pitch_s_ = params_.pitch_ratio;
    formant_s_ = params_.formant_ratio;
}

void PsolaShifter::process(const float *in, float *out, int frames) {
    if (!ready()) {
        for (int i = 0; i < frames; ++i)
            out[i] = in[i];
        return;
    }
    const float mix = std::clamp(params_.mix, 0.0f, 1.0f);
    const float gain = params_.gain;
    for (int i = 0; i < frames; ++i) {
        push_input(in[i]);
        make_marks();
        const int64_t o = n_ - 1 - latency_; // いま出力する位置（入力の時間軸）
        // グレインは左半分が出力に間に合うぎりぎりで置く（そのぶん先の入力を多く見られる）。
        // 左半分の長さは直前のグレインから見積もり、周期が伸びたときのために少し余裕をみる
        while (next_grain_ - lead_ < (double)(o + 1))
            place_grain();
        float &slot = acc_[(size_t)(o & mask_)];
        const float wet = slot;
        slot = 0.0f;
        const float dry = at(o);
        out[i] = (dry + (wet - dry) * mix) * gain;
    }
}

void PsolaShifter::push_input(float x) {
    in_[(size_t)(n_ & mask_)] = x;
    lp1_ = lp_a_ * lp1_ + (1.0f - lp_a_) * x;
    lp2_ = lp_a_ * lp2_ + (1.0f - lp_a_) * lp1_;
    lp_[(size_t)(n_ & mask_)] = lp2_;

    if (++dec_phase_ >= dec_) {
        dec_phase_ = 0;
        float y = 0.0f;
        const int taps = (int)fir_.size();
        for (int k = 0; k < taps; ++k)
            y += fir_[k] * at(n_ - k);
        dbuf_[(size_t)(nd_ & dmask_)] = y;
        ++nd_;
        if (++yin_count_ >= yin_hop_) {
            yin_count_ = 0;
            run_pitch_detection();
        }
    }
    ++n_;
}

// YIN 法（de Cheveigné & Kawahara 2002）で周期を求める
void PsolaShifter::run_pitch_detection() {
    const int w = yin_w_, tmax = tau_max_;
    if (nd_ < w + tmax)
        return;
    float *x = ydiff_.data(); // 作業用（w + tmax 個）
    const int64_t start = nd_ - (w + tmax);
    double energy = 0;
    for (int j = 0; j < w + tmax; ++j) {
        x[j] = dbuf_[(size_t)((start + j) & dmask_)];
        if (j >= tmax)
            energy += (double)x[j] * x[j];
    }
    const double rms = std::sqrt(energy / w);

    // 差分関数 → 累積平均で正規化
    static thread_local std::vector<float> d;
    d.assign(tmax + 2, 1.0f);
    double running = 0;
    for (int tau = 1; tau <= tmax; ++tau) {
        double s = 0;
        // いちばん新しい w サンプルを、tau だけ前と比べる（声の出だしに早く気づける）
        const float *cur = x + tmax;
        for (int j = 0; j < w; ++j) {
            const double v = (double)cur[j] - cur[j - tau];
            s += v * v;
        }
        running += s;
        d[tau] = running > 0 ? (float)(s * tau / running) : 1.0f;
    }

    int tau = -1;
    for (int t = tau_min_; t < tmax; ++t) {
        if (d[t] < 0.12f) {
            while (t + 1 < tmax && d[t + 1] < d[t])
                ++t;
            tau = t;
            break;
        }
    }
    if (tau < 0) {
        tau = tau_min_;
        for (int t = tau_min_; t < tmax; ++t)
            if (d[t] < d[tau])
                tau = t;
        // いちばん深い谷が 2 倍・3 倍の周期（1 オクターブ下）のことがあるので、
        // ほぼ同じ深さの谷がもっと短い周期にあれば、そちらを選ぶ
        const float near = d[tau] * 1.3f + 0.04f;
        for (int t = tau_min_ + 1; t < tau; ++t) {
            if (d[t] < near && d[t] <= d[t - 1] && d[t] <= d[t + 1]) {
                tau = t;
                break;
            }
        }
    }
    const float aperiodic = d[tau];
    // 息まじりの声や声の終わりも拾えるよう、判定はやや甘め（一度有声になったら外れにくく）
    constexpr double kMinRms = 0.0015; // 約 -56dBFS（2.4kHz 以下）
    const bool is_voiced = rms > kMinRms && aperiodic < (voiced_ ? 0.45f : 0.3f);
    if (!is_voiced) {
        // 声の終わりや息まじりの所で一瞬だけ判定が外れても、しばらくは直前の高さのまま有声として扱う
        hold_left_ -= yin_hop_;
        if (voiced_ && hold_left_ > 0 && rms > kMinRms * 0.3)
            return;
        voiced_ = false;
        recent_n_ = 0;
        return;
    }
    hold_left_ = (int)(0.04 * sr_ / dec_); // 40ms

    double off = 0;
    if (tau > 1 && tau < tmax) {
        const double a = d[tau - 1], b = d[tau], c = d[tau + 1];
        const double den = a - 2 * b + c;
        if (den > 1e-9)
            off = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }
    const float t_full = std::clamp((float)((tau + off) * dec_), min_period_, max_period_);

    // 直近 3 回の中央値にして、ときどき出る 1 オクターブの取り違いを消す
    recent_[recent_n_ % 3] = t_full;
    ++recent_n_;
    if (recent_n_ >= 3) {
        float a = recent_[0], b = recent_[1], c = recent_[2];
        period_ = std::max(std::min(a, b), std::min(std::max(a, b), c));
    } else {
        period_ = t_full;
    }
    voiced_ = true;
}

// 直前の目印と波形がいちばんよく重なる位置を、予測位置のまわりで探す
double PsolaShifter::align_to_previous(const Mark &prev, double predicted, float period) const {
    const int h = std::max(2, (int)(period * 0.5f));
    const int r = std::max(1, (int)(period * 0.25f));
    const int64_t pb = (int64_t)std::llround(prev.pos);
    const int64_t c0 = (int64_t)std::llround(predicted);
    double best = -1e30, s_prev = 0, s_next = 0;
    int best_d = 0;
    static thread_local std::vector<double> scores;
    scores.assign(2 * r + 1, 0.0);
    for (int dd = -r; dd <= r; ++dd) {
        const int64_t c = c0 + dd;
        double num = 0, den = 0;
        for (int k = -h; k <= h; ++k) {
            const double v = at(c + k);
            num += (double)at(pb + k) * v;
            den += v * v;
        }
        const double sc = num / std::sqrt(den + 1e-12);
        scores[dd + r] = sc;
        if (sc > best) {
            best = sc;
            best_d = dd;
        }
    }
    double off = 0;
    if (best_d > -r && best_d < r) {
        s_prev = scores[best_d + r - 1];
        s_next = scores[best_d + r + 1];
        const double den = s_prev - 2 * best + s_next;
        if (den < -1e-12)
            off = std::clamp(0.5 * (s_prev - s_next) / den, -0.5, 0.5);
    }
    return (double)(c0 + best_d) + off + (prev.pos - (double)pb);
}

void PsolaShifter::make_marks() {
    const int64_t last_in = n_ - 1;
    for (;;) {
        const Mark last = marks_.back();
        if (voiced_ && period_ > 0.0f) {
            const float t = period_;
            if (last.voiced) {
                const double pred = last.pos + t;
                if ((double)last_in < pred + t * 0.75 + 4)
                    break;
                const double pos = align_to_previous(last, pred, t);
                marks_.push_back({pos, (float)(pos - last.pos), true});
            } else {
                // 有声音の始まり。声だと分かるまで少し時間がかかるので、まだ使っていない無声音の目印は
                // 取り消して、声の出だしから高さを変えられるようにする（取り消すのは次のグレインより先の分だけ）
                {
                    bool rolled = false;
                    while (marks_.size() > 2 && !marks_.back().voiced &&
                           marks_[marks_.size() - 2].pos > next_grain_ + unvoiced_period_) {
                        marks_.pop_back();
                        rolled = true;
                    }
                    if (rolled)
                        continue;
                }
                // 1 周期の中でいちばん振れの大きい所に最初の目印を置く
                const int64_t lo = (int64_t)(last.pos + unvoiced_period_ * 0.5);
                const int64_t hi = lo + (int64_t)t;
                if (last_in < hi + (int64_t)(t * 0.5f) + 4)
                    break;
                int64_t best = lo;
                for (int64_t k = lo; k <= hi; ++k)
                    if (std::fabs(lp_[(size_t)(k & mask_)]) > std::fabs(lp_[(size_t)(best & mask_)]))
                        best = k;
                marks_.push_back({(double)best, t, true});
            }
        } else {
            const double pos = last.pos + unvoiced_period_;
            if ((double)last_in < pos + unvoiced_period_ + 4)
                break;
            marks_.push_back({pos, unvoiced_period_, false});
        }
    }
    const double keep_from = next_grain_ - 2.0 * max_period_ - cap_half_;
    while (marks_.size() > 4 && marks_[1].pos < keep_from)
        marks_.pop_front();
}

void PsolaShifter::place_grain() {
    const double s = next_grain_;
    const double avail = (double)(n_ - 1) - 3.0;

    // 目標の値へなめらかに近づける（対数で。上げ下げどちらも同じ速さに聞こえる）
    const float f_target = std::clamp(params_.formant_ratio, 0.25f, 4.0f);
    const float p_target = std::clamp(params_.pitch_ratio, 0.25f, 4.0f);

    // この位置にいちばん近い、材料がそろっている目印を選ぶ。
    // グレインは左右非対称：左半分は前の目印まで、右半分は次の目印まで。
    // こうすると周期が揺れていても、隣どうしの窓がぴったり 1 に足し合わさる。
    const double f = formant_s_;
    const double lo_p = min_period_ * 0.5, hi_p = max_period_ * 1.5;
    auto halves = [&](size_t i, double &left, double &right) {
        const Mark &m = marks_[i];
        left = i > 0 ? m.pos - marks_[i - 1].pos : m.period;
        right = i + 1 < marks_.size() ? marks_[i + 1].pos - m.pos : m.period;
        left = std::clamp(left, lo_p, hi_p);
        right = std::clamp(right, lo_p, hi_p);
    };
    int best = -1;
    double best_d = 1e30;
    // 次の目印がまだ無い（いちばん新しい）目印は、右半分を今の周期で見積もる
    {
        for (size_t i = 0; i < marks_.size(); ++i) {
            double left, right;
            halves(i, left, right);
            if (marks_[i].pos + std::min(right / f, cap_half_) * f > avail)
                continue;
            const double d = std::fabs(marks_[i].pos - s);
            if (d < best_d) {
                best_d = d;
                best = (int)i;
            }
        }
    }
    if (best < 0) {
        next_grain_ += unvoiced_period_;
        return;
    }

    const Mark &m = marks_[(size_t)best];
    double left, right;
    halves((size_t)best, left, right);
    const double half_l = std::min(left / f, cap_half_), half_r = std::min(right / f, cap_half_);
    lead_ = std::min(cap_half_, std::max(half_l, half_r) * 1.3 + 8.0);
    // 無声音は高さを変えない（響きを上げるときだけ、窓が短くなる分グレインを詰める）
    double spacing = m.voiced ? right / pitch_s_ : right / std::max(1.0, f);
    spacing = std::max(spacing, 4.0);
    // 重なりが多いときは、重なった分だけ小さくして音量をそろえる
    const double g = half_r >= spacing ? spacing / half_r : 1.0;

    const int64_t o = n_ - 1 - latency_;
    const int64_t q0 = std::max((int64_t)std::ceil(s - half_l), o);
    const int64_t q1 = (int64_t)std::floor(s + half_r);
    for (int64_t q = q0; q <= q1; ++q) {
        const double rel = (double)q - s;
        const double w = 0.5 + 0.5 * std::cos(kPi * rel / (rel < 0 ? half_l : half_r));
        acc_[(size_t)(q & mask_)] += (float)(g * w * interp(m.pos + rel * f));
    }
    next_grain_ += spacing;

    const double a = 1.0 - std::exp(-spacing / (kGlideSeconds * sr_));
    pitch_s_ = (float)std::exp(std::log(pitch_s_) + (std::log(p_target) - std::log(pitch_s_)) * a);
    formant_s_ = (float)std::exp(std::log(formant_s_) + (std::log(f_target) - std::log(formant_s_)) * a);
}

float PsolaShifter::interp(double pos) const {
    const double fl = std::floor(pos);
    const int64_t i = (int64_t)fl;
    const float t = (float)(pos - fl);
    const float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
    return y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + t * (3.0f * (y1 - y2) + y3 - y0)));
}

} // namespace pf
