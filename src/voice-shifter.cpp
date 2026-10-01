// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 hakoniwa
// voice-shifter.cpp
//
// 仕組みは voice-shifter.h の冒頭を参照。

#include "voice-shifter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace pf {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

inline double wrap_phase(double x) {
    return x - kTwoPi * std::floor((x + kPi) / kTwoPi);
}

// 包絡の True Envelope 反復回数（多いほど倍音の山をきちんとなぞる）
constexpr int kEnvIterations = 6;
// フォルマント補正の上限・下限（自然対数。約 +40dB / -60dB）
constexpr float kMaxBoost = 4.6f;
constexpr float kMaxCut = -6.9f;

} // namespace

// ---------------------------------------------------------------- RealFFT

RealFFT::RealFFT(int n) : n_(n), h_(n / 2) {
    rev_.resize(h_);
    int bits = 0;
    while ((1 << bits) < h_)
        ++bits;
    for (int i = 0; i < h_; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b))
                r |= 1 << (bits - 1 - b);
        rev_[i] = r;
    }
    tw_.resize(std::max(1, h_ / 2));
    for (int k = 0; k < h_ / 2; ++k)
        tw_[k] = std::polar(1.0f, (float)(-kTwoPi * k / h_));
    post_.resize(h_ + 1);
    for (int k = 0; k <= h_; ++k)
        post_[k] = std::polar(1.0f, (float)(-kTwoPi * k / n_));
    work_.resize(h_);
}

void RealFFT::fft(cpx *z, bool inv) const {
    for (int i = 0; i < h_; ++i)
        if (i < rev_[i])
            std::swap(z[i], z[rev_[i]]);
    for (int len = 2; len <= h_; len <<= 1) {
        const int half = len >> 1;
        const int step = h_ / len;
        for (int i = 0; i < h_; i += len) {
            for (int j = 0; j < half; ++j) {
                cpx w = tw_[j * step];
                if (inv)
                    w = std::conj(w);
                const cpx u = z[i + j];
                const cpx v = z[i + j + half] * w;
                z[i + j] = u + v;
                z[i + j + half] = u - v;
            }
        }
    }
}

void RealFFT::forward(const float *x, cpx *X) const {
    cpx *w = work_.data();
    for (int i = 0; i < h_; ++i)
        w[i] = cpx(x[2 * i], x[2 * i + 1]);
    fft(w, false);
    for (int k = 0; k <= h_; ++k) {
        const cpx zk = w[k % h_];
        const cpx zc = std::conj(w[(h_ - k) % h_]);
        const cpx fe = (zk + zc) * 0.5f;
        const cpx fo = (zk - zc) * cpx(0.0f, -0.5f);
        X[k] = fe + post_[k] * fo;
    }
}

void RealFFT::inverse(const cpx *X, float *x) const {
    cpx *w = work_.data();
    for (int k = 0; k < h_; ++k) {
        const cpx xc = std::conj(X[h_ - k]);
        const cpx fe = (X[k] + xc) * 0.5f;
        const cpx fo = (X[k] - xc) * 0.5f * std::conj(post_[k]);
        w[k] = fe + cpx(0.0f, 1.0f) * fo;
    }
    fft(w, true);
    const float s = 1.0f / (float)h_;
    for (int i = 0; i < h_; ++i) {
        x[2 * i] = w[i].real() * s;
        x[2 * i + 1] = w[i].imag() * s;
    }
}

// ---------------------------------------------------------------- VoiceShifter

void VoiceShifter::init(double sample_rate, int fft_size, int overlap) {
    sr_ = sample_rate > 0 ? sample_rate : 48000.0;
    n_ = fft_size;
    hop_ = fft_size / overlap;
    half_ = n_ / 2;
    decim_ = 2;
    m_ = n_ / decim_;
    mh_ = m_ / 2;

    auto t = std::make_shared<Tables>(n_, m_);
    t->window.resize(n_);
    for (int i = 0; i < n_; ++i)
        t->window[i] = (float)(0.5 - 0.5 * std::cos(kTwoPi * i / n_));

    double ola = 0.0;
    for (int i = 0; i < n_; i += hop_)
        ola += (double)t->window[i] * t->window[i];
    ola_norm_ = (float)(1.0 / ola);

    // 包絡の細かさ（ケプストラムの打ち切り位置）。約 2.7ms より長い周期＝声の高さの成分は落とす
    const int qc = std::clamp((int)std::lround(sr_ / 375.0), 8, mh_ - 1);
    const int taper = std::max(2, qc / 4);
    t->lifter.assign(mh_ + 1, 0.0f);
    for (int q = 0; q <= mh_; ++q) {
        if (q <= qc - taper)
            t->lifter[q] = 1.0f;
        else if (q < qc)
            t->lifter[q] = (float)(0.5 + 0.5 * std::cos(kPi * (q - (qc - taper)) / taper));
    }
    tab_ = t;

    in_ring_.assign(n_, 0.0f);
    accum_.assign(n_, 0.0f);
    out_fifo_.assign(hop_, 0.0f);
    frame_.assign(n_, 0.0f);
    spec_.assign(half_ + 1, cpx());
    out_spec_.assign(half_ + 1, cpx());
    mag_.assign(half_ + 1, 0.0f);
    phase_.assign(half_ + 1, 0.0f);
    last_phase_.assign(half_ + 1, 0.0f);
    true_bin_.assign(half_ + 1, 0.0f);
    synth_phase_.assign(half_ + 1, 0.0f);
    prev_peak_at_.assign(half_ + 1, -1);
    peaks_.reserve(half_);
    prev_peaks_.reserve(half_);
    peak_bins_.reserve(half_);
    region_hi_.reserve(half_);
    env_.assign(mh_ + 1, 0.0f);
    env_tmp_.assign(mh_ + 1, 0.0f);
    env_prev_.assign(mh_ + 1, 0.0f);
    env_even_.assign(m_, 0.0f);
    env_spec_.assign(mh_ + 1, cpx());
    reset();
}

void VoiceShifter::reset() {
    std::fill(in_ring_.begin(), in_ring_.end(), 0.0f);
    std::fill(accum_.begin(), accum_.end(), 0.0f);
    std::fill(out_fifo_.begin(), out_fifo_.end(), 0.0f);
    std::fill(last_phase_.begin(), last_phase_.end(), 0.0f);
    std::fill(synth_phase_.begin(), synth_phase_.end(), 0.0f);
    std::fill(prev_peak_at_.begin(), prev_peak_at_.end(), -1);
    peaks_.clear();
    prev_peaks_.clear();
    in_pos_ = 0;
    hop_count_ = 0;
    out_pos_ = 0;
    pitch_active_ = false;
    env_valid_ = false;
}

void VoiceShifter::process(const float *in, float *out, int frames) {
    if (!ready()) {
        if (out != in)
            std::memmove(out, in, sizeof(float) * frames);
        return;
    }
    const float mix = std::clamp(params_.mix, 0.0f, 1.0f);
    const float gain = params_.gain;
    const int mask = n_ - 1;
    for (int i = 0; i < frames; ++i) {
        const float x = in[i];
        const float dry = in_ring_[in_pos_]; // ちょうど N サンプル前の入力
        in_ring_[in_pos_] = x;
        in_pos_ = (in_pos_ + 1) & mask;
        const float wet = out_fifo_[out_pos_++];
        if (++hop_count_ == hop_) {
            hop_count_ = 0;
            process_frame();
            out_pos_ = 0;
        }
        out[i] = (dry + (wet - dry) * mix) * gain;
    }
}

void VoiceShifter::process_frame() {
    const Tables &t = *tab_;
    const float *w = t.window.data();
    const int mask = n_ - 1;
    for (int k = 0; k < n_; ++k)
        frame_[k] = in_ring_[(in_pos_ + k) & mask] * w[k];

    t.fft.forward(frame_.data(), spec_.data());
    for (int k = 0; k <= half_; ++k) {
        mag_[k] = std::abs(spec_[k]);
        phase_[k] = std::arg(spec_[k]);
    }

    const float p = std::clamp(params_.pitch_ratio, 0.25f, 4.0f);
    const float f = std::clamp(params_.formant_ratio, 0.25f, 4.0f);
    const bool do_pitch = std::fabs(p - 1.0f) > 1e-4f;
    const bool do_formant = std::fabs(std::log(f / p)) > 1e-4f;

    if (do_formant)
        compute_envelope();
    if (do_pitch || do_formant)
        find_peaks();

    if (do_pitch) {
        pitch_shift_frame(p, do_formant, f);
        pitch_active_ = true;
    } else {
        if (do_formant) {
            // 響きだけを変える：倍音ごとに 1 つの倍率をかける（位相はそのまま）
            int lo = 0;
            for (size_t i = 0; i < peak_bins_.size(); ++i) {
                const int kp = peak_bins_[i], hi = region_hi_[i];
                const float g = std::exp(formant_gain((float)kp, (float)kp, f));
                for (int k = lo; k <= hi; ++k)
                    out_spec_[k] = spec_[k] * g;
                lo = hi + 1;
            }
            for (int k = lo; k <= half_; ++k)
                out_spec_[k] = cpx();
        } else {
            std::copy(spec_.begin(), spec_.end(), out_spec_.begin());
        }
        std::copy(phase_.begin(), phase_.end(), last_phase_.begin());
        std::copy(phase_.begin(), phase_.end(), synth_phase_.begin());
        if (pitch_active_) {
            prev_peaks_.clear();
            std::fill(prev_peak_at_.begin(), prev_peak_at_.end(), -1);
            pitch_active_ = false;
        }
    }

    out_spec_[0] = cpx(out_spec_[0].real(), 0.0f);
    out_spec_[half_] = cpx(out_spec_[half_].real(), 0.0f);
    t.fft.inverse(out_spec_.data(), frame_.data());

    const float s = ola_norm_;
    for (int k = 0; k < n_; ++k)
        accum_[k] += frame_[k] * w[k] * s;
    std::copy(accum_.begin(), accum_.begin() + hop_, out_fifo_.begin());
    std::memmove(accum_.data(), accum_.data() + hop_, sizeof(float) * (n_ - hop_));
    std::fill(accum_.begin() + (n_ - hop_), accum_.end(), 0.0f);
}

void VoiceShifter::find_peaks() {
    float mx = 0.0f;
    for (int k = 0; k <= half_; ++k)
        mx = std::max(mx, mag_[k]);
    const float thr = std::max(mx * 1e-5f, 1e-9f);
    peak_bins_.clear();
    region_hi_.clear();
    for (int k = 1; k < half_; ++k) {
        const float m = mag_[k];
        if (m <= thr || m <= mag_[k - 1] || m < mag_[k + 1])
            continue;
        if (k >= 2 && m <= mag_[k - 2])
            continue;
        if (k + 2 <= half_ && m < mag_[k + 2])
            continue;
        peak_bins_.push_back(k);
    }
    // それぞれの山の受け持ち範囲：次の山との間で一番低いビンまで
    const int np = (int)peak_bins_.size();
    for (int i = 0; i < np; ++i) {
        int hi = half_;
        if (i + 1 < np) {
            const int kp = peak_bins_[i], kn = peak_bins_[i + 1];
            hi = kp;
            for (int k = kp + 1; k < kn; ++k)
                if (mag_[k] < mag_[hi])
                    hi = k;
        }
        region_hi_.push_back(hi);
    }
}

float VoiceShifter::formant_gain(float out_bin, float in_bin, float formant_ratio) const {
    // 出力の周波数に「伸縮した元の包絡」が来るよう、元の位置の包絡との差だけ持ち上げる/下げる
    return std::clamp(env_at(out_bin / formant_ratio) - env_at(in_bin), kMaxCut, kMaxBoost);
}

void VoiceShifter::pitch_shift_frame(float ratio, bool formant, float f) {
    const double expct = kTwoPi * hop_ / n_;

    // 各ビンの本当の周波数（前フレームとの位相差から）
    for (int k = 0; k <= half_; ++k) {
        const double dp = wrap_phase((double)phase_[k] - last_phase_[k] - k * expct);
        true_bin_[k] = (float)(k + dp / expct);
        last_phase_[k] = phase_[k];
    }

    std::fill(out_spec_.begin(), out_spec_.end(), cpx());
    peaks_.clear();

    const int np = (int)peak_bins_.size();
    int lo = 0;
    for (int i = 0; i < np; ++i) {
        const int kp = peak_bins_[i];
        const int hi = region_hi_[i];

        const double target = (double)true_bin_[kp] * ratio;
        const int shift = (int)std::lround(target - kp);
        const int out_bin = kp + shift;
        if (out_bin >= 0 && out_bin <= half_) {
            const double w_out = target * kTwoPi / n_;
            const int j = prev_peak_at_[kp];
            double ph_out;
            if (j >= 0)
                ph_out = prev_peaks_[j].phase + hop_ * 0.5 * (prev_peaks_[j].omega + w_out);
            else
                ph_out = synth_phase_[out_bin] + hop_ * w_out;
            ph_out = wrap_phase(ph_out);

            float amp = 1.0f;
            if (formant)
                amp = std::exp(formant_gain((float)target, true_bin_[kp], f));
            const cpx rot = std::polar(amp, (float)(ph_out - phase_[kp]));
            const int k0 = std::max(lo, -shift);
            const int k1 = std::min(hi, half_ - shift);
            for (int k = k0; k <= k1; ++k)
                out_spec_[k + shift] += spec_[k] * rot;
            peaks_.push_back({kp, ph_out, w_out});
        }
        lo = hi + 1;
    }

    for (int k = 0; k <= half_; ++k) {
        const cpx v = out_spec_[k];
        if (v.real() != 0.0f || v.imag() != 0.0f)
            synth_phase_[k] = std::arg(v);
    }

    // 次のフレームで「同じ倍音」を見つけるための索引
    std::swap(prev_peaks_, peaks_);
    std::fill(prev_peak_at_.begin(), prev_peak_at_.end(), -1);
    constexpr int kReach = 3;
    for (int j = 0; j < (int)prev_peaks_.size(); ++j) {
        const int b = prev_peaks_[j].bin;
        for (int d = -kReach; d <= kReach; ++d) {
            const int idx = b + d;
            if (idx < 0 || idx > half_)
                continue;
            const int cur = prev_peak_at_[idx];
            if (cur < 0 || std::abs(prev_peaks_[cur].bin - idx) > std::abs(d))
                prev_peak_at_[idx] = j;
        }
    }
}

void VoiceShifter::compute_envelope() {
    const Tables &t = *tab_;
    // 2 ビンごとの最大値を取った対数スペクトル（倍音の山の高さを拾う）
    float top = -1e30f;
    for (int m = 0; m <= mh_; ++m) {
        const int c = m * decim_;
        const int a = std::max(0, c - decim_ / 2);
        const int b = std::min(half_, c + decim_ / 2);
        float v = 0.0f;
        for (int k = a; k <= b; ++k)
            v = std::max(v, mag_[k]);
        env_tmp_[m] = std::log(v + 1e-12f);
        top = std::max(top, env_tmp_[m]);
    }
    const float floor_v = top - 6.9f; // 約 -60dB で底上げ（深い谷の揺れを包絡に入れない）
    for (int m = 0; m <= mh_; ++m)
        env_tmp_[m] = std::max(env_tmp_[m], floor_v);

    // True Envelope：ケプストラムで滑らかにし、山より下がったところを持ち上げて繰り返す
    for (int it = 0; it < kEnvIterations; ++it) {
        for (int m = 0; m < m_; ++m)
            env_even_[m] = env_tmp_[m <= mh_ ? m : m_ - m];
        t.env_fft.forward(env_even_.data(), env_spec_.data());
        for (int q = 0; q <= mh_; ++q)
            env_spec_[q] = cpx(env_spec_[q].real() * t.lifter[q], 0.0f);
        t.env_fft.inverse(env_spec_.data(), env_even_.data());
        for (int m = 0; m <= mh_; ++m)
            env_[m] = env_even_[m];
        if (it + 1 < kEnvIterations)
            for (int m = 0; m <= mh_; ++m)
                env_tmp_[m] = std::max(env_tmp_[m], env_[m]);
    }

    // 一番低い倍音（基音）より下は声の成分がなく、包絡が落ち込んで見える。
    // そのままだとピッチを下げたときに新しい基音が削られるので、基音の高さで平らにする。
    const float first_thr = top - 4.0f; // 最大から約 -35dB 以内の最初の山
    for (int m = 1; m < mh_; ++m) {
        const float v = env_tmp_[m];
        if (v > first_thr && v >= env_tmp_[m - 1] && v >= env_tmp_[m + 1]) {
            const float hold = env_[m];
            for (int j = 0; j < m; ++j)
                env_[j] = std::max(env_[j], hold);
            break;
        }
    }

    // フレームごとの細かな揺れをならす（揺れたままだと倍音の音量が震えて濁る）
    constexpr float a = 0.3f;
    if (env_valid_) {
        for (int m = 0; m <= mh_; ++m)
            env_[m] = a * env_prev_[m] + (1.0f - a) * env_[m];
    }
    std::copy(env_.begin(), env_.end(), env_prev_.begin());
    env_valid_ = true;
}

float VoiceShifter::env_at(float bin) const {
    const float x = bin / (float)decim_;
    if (x <= 0.0f)
        return env_[0];
    if (x >= (float)mh_)
        return env_[mh_];
    const int i = (int)x;
    const float fr = x - (float)i;
    return env_[i] + (env_[i + 1] - env_[i]) * fr;
}

} // namespace pf
