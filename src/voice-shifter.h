// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 hakoniwa
// voice-shifter.h
//
// ピッチとフォルマントを別々に動かすストリーミング音声処理（1チャンネル分）。
// OBS には依存しないので、単体テスト（tests/offline.cpp）からもそのまま使える。
//
//  ・ピッチ : 位相ロック付きフェーズボコーダ（Laroche & Dolson 方式）。
//             スペクトルのピークごとに周囲のビンをまとめて移動し、ピーク間の位相関係を保つ。
//  ・フォルマント : 入力のスペクトル包絡（True Envelope ケプストラム）を推定し、
//             ピッチ移動でずれた包絡を「好きな倍率に伸縮した包絡」へ付け替える。
//
// 遅延は FFT サイズ N サンプルちょうど（乾いた音も同じだけ遅らせて揃える）。

#pragma once

#include <complex>
#include <memory>
#include <vector>

namespace pf {

using cpx = std::complex<float>;

// 実数列用 FFT（N/2 点の複素 FFT で N 点の実数 FFT を行う）。
class RealFFT {
public:
    explicit RealFFT(int n);
    int size() const { return n_; }
    // x[0..n) → X[0..n/2]（正規化なし）
    void forward(const float *x, cpx *X) const;
    // X[0..n/2] → x[0..n)（1/n で正規化）
    void inverse(const cpx *X, float *x) const;

private:
    void fft(cpx *z, bool inv) const; // n/2 点、in-place
    int n_, h_;
    std::vector<int> rev_;
    std::vector<cpx> tw_;   // n/2 点 FFT のひねり係数
    std::vector<cpx> post_; // 実数化用 e^{-2πik/n}
    mutable std::vector<cpx> work_;
};

struct ShifterParams {
    float pitch_ratio = 1.0f;   // 2^(半音/12)
    float formant_ratio = 1.0f; // 2^(半音/12)。1 で元の声の響きを保つ
    float mix = 1.0f;           // 0 = 原音のみ, 1 = 加工音のみ
    float gain = 1.0f;          // 出力の倍率
};

class VoiceShifter {
public:
    // fft_size は 2 のべき乗、overlap は 4 または 8 を想定
    void init(double sample_rate, int fft_size, int overlap);
    void reset();
    void set_params(const ShifterParams &p) { params_ = p; }
    // in と out は同じ配列でもよい
    void process(const float *in, float *out, int frames);
    int latency() const { return n_; }
    bool ready() const { return n_ > 0; }

private:
    void process_frame();
    void compute_envelope();
    float env_at(float bin) const;
    void find_peaks();
    float formant_gain(float out_bin, float in_bin, float formant_ratio) const;
    void pitch_shift_frame(float ratio, bool formant, float formant_ratio);

    struct Peak {
        int bin;       // 入力側のビン
        double phase;  // 出力側の位相
        double omega;  // 出力側の周波数 [rad/サンプル]
    };

    struct Tables {
        Tables(int n, int m) : fft(n), env_fft(m) {}
        RealFFT fft;
        RealFFT env_fft;
        std::vector<float> window;
        std::vector<float> lifter;
    };

    ShifterParams params_;
    double sr_ = 48000.0;
    int n_ = 0, hop_ = 0, half_ = 0;
    int decim_ = 4, m_ = 0, mh_ = 0;
    float ola_norm_ = 1.0f;
    std::shared_ptr<const Tables> tab_;

    // ストリーミング用
    std::vector<float> in_ring_;
    int in_pos_ = 0;
    int hop_count_ = 0;
    std::vector<float> accum_;
    std::vector<float> out_fifo_;
    int out_pos_ = 0;

    // フレーム処理用
    std::vector<float> frame_;
    std::vector<cpx> spec_, out_spec_;
    std::vector<float> mag_, phase_, last_phase_, true_bin_;
    std::vector<float> synth_phase_;
    std::vector<Peak> peaks_, prev_peaks_;
    std::vector<int> prev_peak_at_; // 入力ビン → 直前フレームの近いピーク番号（なければ -1）
    std::vector<int> peak_bins_;
    std::vector<int> region_hi_; // 各ピークが受け持つ範囲の上端

    // 包絡用
    std::vector<float> env_, env_tmp_, env_even_, env_prev_;
    bool env_valid_ = false;
    std::vector<cpx> env_spec_;
    bool pitch_active_ = false;
};

} // namespace pf
