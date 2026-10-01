// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 hakoniwa
// psola.h
//
// 声用のピッチ・フォルマント変換（PSOLA 方式、1 チャンネル分）。
//
//  1. YIN 法で声の周期（ピッチ）を測る
//  2. 声の 1 周期ごとに「目印（ピッチマーク）」を置く。前の周期との相関で位置を合わせるので、
//     目印が波形の同じ場所にそろい、切り貼りしてもつなぎ目が目立たない
//  3. 目印を中心に 2 周期分を窓で切り出し（グレイン）、新しいピッチの間隔で並べ直す
//     → 声の高さだけが変わり、声の響き（フォルマント）はそのまま残る
//  4. フォルマントはグレインを伸び縮みさせて動かす（縮める＝響きが上がる）
//  5. 無声音（さ行の子音など）は周期がないので、並べ替えずにそのまま通す
//
// フェーズボコーダと違って音を周波数に分解しないので、にじみ・二重に聞こえる感じ（位相のずれ）が出にくい。
// 遅延は約 32ms で固定。

#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "voice-shifter.h" // ShifterParams

namespace pf {

class PsolaShifter {
public:
    // latency_sec：遅延（秒）。短いほど遅れは減るが、低い声や声の出だしの扱いが苦しくなる
    void init(double sample_rate, double latency_sec = 0.032);
    void reset();
    void set_params(const ShifterParams &p) { params_ = p; }
    void process(const float *in, float *out, int frames);
    int latency() const { return latency_; }
    bool ready() const { return latency_ > 0; }

    // テスト用：いま有声音と判定しているか・測った周期（サンプル）
    bool voiced() const { return voiced_; }
    float period() const { return period_; }

private:
    struct Mark {
        double pos;  // 入力上の位置（サンプル）
        float period; // この目印の周期（サンプル）
        bool voiced;
    };

    void push_input(float x);
    void run_pitch_detection();
    void make_marks();
    void place_grain();
    float at(int64_t i) const { return in_[(size_t)(i & mask_)]; }
    float interp(double pos) const;
    double align_to_previous(const Mark &prev, double predicted, float period) const;

    ShifterParams params_;
    double sr_ = 48000.0;
    int latency_ = 0;
    int64_t mask_ = 0;
    int64_t n_ = 0; // これまでに入った入力サンプル数

    std::vector<float> in_, lp_, acc_;
    float lp1_ = 0, lp2_ = 0, lp_a_ = 0;

    // ピッチ検出（間引いた信号で YIN）
    int dec_ = 1, dec_phase_ = 0;
    std::vector<float> fir_;
    std::vector<float> dbuf_; // 間引いた信号（リング）
    int64_t nd_ = 0;
    int dmask_ = 0;
    int yin_w_ = 0, tau_min_ = 0, tau_max_ = 0, yin_hop_ = 0, yin_count_ = 0;
    std::vector<float> ydiff_;
    bool voiced_ = false;
    float period_ = 0.0f;     // 検出した周期（入力のサンプル単位）
    float recent_[3] = {0, 0, 0};
    int recent_n_ = 0;
    int hold_left_ = 0; // 有声の判定が外れても有声のまま保つ残り（間引いた信号のサンプル数）

    // ピッチマークとグレイン
    std::deque<Mark> marks_;
    float unvoiced_period_ = 240.0f;
    float min_period_ = 50.0f, max_period_ = 700.0f;
    double cap_half_ = 768.0; // グレインの半分の長さの上限
    double lead_ = 768.0;     // 次のグレインを、出力位置のどれだけ手前で置くか
    double next_grain_ = 0.0;  // 次のグレインを置く位置（入力と同じ時間軸）
    float pitch_s_ = 1.0f, formant_s_ = 1.0f; // なめらかに追いかける現在値
};

} // namespace pf
