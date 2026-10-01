// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 hakoniwa
// offline.cpp
//
// OBS なしで音声処理部分を確かめるための道具。
//   offline test                                   … 自動テスト（数値で合否を出す）
//   offline [identity|pitch|formant|live|echo|glide|speed] … 一部のテストだけ
//   offline render in.wav out.wav ピッチ フォルマント [pv|pvlow|sweep] … WAV を加工して書き出す（既定は PSOLA。sweep はピッチをゆっくり上下）

#include "../src/psola.h"
#include "../src/three-band-eq.h"
#include "../src/voice-shifter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
int g_fail = 0;

void check(bool ok, const char *what) {
    std::printf("  [%s] %s\n", ok ? " OK " : "FAIL", what);
    if (!ok)
        ++g_fail;
}

// ---------------------------------------------------------------- WAV

bool read_wav(const char *path, std::vector<float> &out, int &sr) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp)
        return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536];
    size_t r;
    while ((r = std::fread(buf, 1, sizeof buf, fp)) > 0)
        b.insert(b.end(), buf, buf + r);
    std::fclose(fp);
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) || std::memcmp(b.data() + 8, "WAVE", 4))
        return false;
    int fmt = 0, ch = 0, bits = 0;
    size_t pos = 12;
    while (pos + 8 <= b.size()) {
        uint32_t len;
        std::memcpy(&len, b.data() + pos + 4, 4);
        const uint8_t *d = b.data() + pos + 8;
        if (!std::memcmp(b.data() + pos, "fmt ", 4)) {
            fmt = d[0] | (d[1] << 8);
            ch = d[2] | (d[3] << 8);
            std::memcpy(&sr, d + 4, 4);
            bits = d[14] | (d[15] << 8);
            if (fmt == 0xFFFE)
                fmt = d[24] | (d[25] << 8);
        } else if (!std::memcmp(b.data() + pos, "data", 4)) {
            len = (uint32_t)std::min<size_t>(len, b.size() - pos - 8);
            const int bps = bits / 8;
            const size_t n = len / (bps * ch);
            out.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const uint8_t *s = d + i * bps * ch;
                if (fmt == 3 && bits == 32) {
                    std::memcpy(&out[i], s, 4);
                } else if (fmt == 1 && bits == 16) {
                    out[i] = (int16_t)(s[0] | (s[1] << 8)) / 32768.0f;
                } else if (fmt == 1 && bits == 24) {
                    int32_t v = (s[0] << 8) | (s[1] << 16) | (s[2] << 24);
                    out[i] = (v >> 8) / 8388608.0f;
                } else {
                    return false;
                }
            }
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

bool write_wav(const char *path, const std::vector<float> &x, int sr) {
    FILE *fp = std::fopen(path, "wb");
    if (!fp)
        return false;
    const uint32_t data = (uint32_t)x.size() * 2, riff = 36 + data, fmtlen = 16, rate = sr,
                   bytes = sr * 2;
    const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
    std::fwrite("RIFF", 1, 4, fp);
    std::fwrite(&riff, 4, 1, fp);
    std::fwrite("WAVEfmt ", 1, 8, fp);
    std::fwrite(&fmtlen, 4, 1, fp);
    std::fwrite(&pcm, 2, 1, fp);
    std::fwrite(&ch, 2, 1, fp);
    std::fwrite(&rate, 4, 1, fp);
    std::fwrite(&bytes, 4, 1, fp);
    std::fwrite(&align, 2, 1, fp);
    std::fwrite(&bits, 2, 1, fp);
    std::fwrite("data", 1, 4, fp);
    std::fwrite(&data, 4, 1, fp);
    for (float v : x) {
        const int16_t s = (int16_t)std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&s, 2, 1, fp);
    }
    std::fclose(fp);
    return true;
}

// ---------------------------------------------------------------- 解析

// 自己相関で基本周波数を求め、区間ごとの値の中央値を返す
double estimate_f0(const std::vector<float> &x, int sr, size_t from, size_t to) {
    const int win = 2048;
    const int min_lag = sr / 1000, max_lag = sr / 50;
    std::vector<double> f0s;
    for (size_t s = from; s + win + max_lag < to; s += win / 2) {
        std::vector<double> r(max_lag + 2, 0.0);
        double e0 = 0;
        for (int i = 0; i < win; ++i)
            e0 += (double)x[s + i] * x[s + i];
        if (e0 < 1e-6)
            continue;
        double best = 0;
        for (int lag = min_lag; lag <= max_lag + 1; ++lag) {
            double acc = 0, el = 0;
            for (int i = 0; i < win; ++i) {
                acc += (double)x[s + i] * x[s + i + lag];
                el += (double)x[s + i + lag] * x[s + i + lag];
            }
            r[lag] = acc / std::sqrt(e0 * el + 1e-12);
            best = std::max(best, r[lag]);
        }
        for (int lag = min_lag + 1; lag <= max_lag; ++lag) {
            if (r[lag] > 0.9 * best && r[lag] >= r[lag - 1] && r[lag] >= r[lag + 1]) {
                const double a = r[lag - 1], b = r[lag], c = r[lag + 1];
                const double den = a - 2 * b + c;
                const double off = den != 0 ? 0.5 * (a - c) / den : 0.0;
                f0s.push_back(sr / (lag + off));
                break;
            }
        }
    }
    if (f0s.empty())
        return 0;
    std::nth_element(f0s.begin(), f0s.begin() + f0s.size() / 2, f0s.end());
    return f0s[f0s.size() / 2];
}

// 周波数 hz 付近（±width）の平均パワー（Goertzel を細かく並べる簡易版）
double band_power(const std::vector<float> &x, int sr, size_t from, size_t len, double lo, double hi) {
    double sum = 0;
    int cnt = 0;
    for (double hz = lo; hz <= hi; hz += 10.0) {
        double re = 0, im = 0;
        for (size_t i = 0; i < len; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * kPi * i / len);
            const double ph = 2 * kPi * hz * i / sr;
            re += x[from + i] * w * std::cos(ph);
            im += x[from + i] * w * std::sin(ph);
        }
        sum += re * re + im * im;
        ++cnt;
    }
    return sum / cnt;
}

double rms(const std::vector<float> &x, size_t from, size_t to) {
    double s = 0;
    for (size_t i = from; i < to; ++i)
        s += (double)x[i] * x[i];
    return std::sqrt(s / (double)(to - from));
}

// make_vowel と同じ形の包絡（周波数だけで決まる）
double vowel_env(double hz, const double formants[3]) {
    double a = 0;
    for (int i = 0; i < 3; ++i) {
        const double d = (hz - formants[i]) / 110.0;
        a += std::pow(0.6, i) / (1.0 + d * d);
    }
    return a / std::sqrt(hz / 100.0);
}

// f0 の倍音ごとの振幅を最小二乗で求め、倍音以外の成分（濁り）の割合も返す
std::vector<double> harmonics(const std::vector<float> &x, int sr, size_t from, size_t len, double f0,
                              double *snr_db) {
    std::vector<double> amps;
    std::vector<double> fit(len, 0.0);
    for (int h = 1; h * f0 < sr * 0.45; ++h) {
        double c = 0, s = 0;
        for (size_t i = 0; i < len; ++i) {
            const double ph = 2 * kPi * h * f0 * i / sr;
            c += x[from + i] * std::cos(ph);
            s += x[from + i] * std::sin(ph);
        }
        c *= 2.0 / len;
        s *= 2.0 / len;
        amps.push_back(std::sqrt(c * c + s * s));
        for (size_t i = 0; i < len; ++i) {
            const double ph = 2 * kPi * h * f0 * i / sr;
            fit[i] += c * std::cos(ph) + s * std::sin(ph);
        }
    }
    double e = 0, r = 0;
    for (size_t i = 0; i < len; ++i) {
        e += (double)x[from + i] * x[from + i];
        r += std::pow(x[from + i] - fit[i], 2);
    }
    if (snr_db)
        *snr_db = 10 * std::log10(e / (r + 1e-20));
    return amps;
}

// 基本周波数 f0 の倍音に、3 つのフォルマント（共鳴）の重みをかけた母音っぽい音
std::vector<float> make_vowel(int sr, double seconds, double f0, const double formants[3]) {
    const size_t n = (size_t)(sr * seconds);
    std::vector<float> x(n, 0.0f);
    const double bw = 110.0;
    for (int h = 1; h * f0 < sr * 0.45; ++h) {
        const double hz = h * f0;
        double a = 0;
        for (int i = 0; i < 3; ++i) {
            const double d = (hz - formants[i]) / bw;
            a += std::pow(0.6, i) / (1.0 + d * d);
        }
        a *= 1.0 / std::sqrt(hz / 100.0);
        const double ph0 = 0.37 * h * h;
        for (size_t i = 0; i < n; ++i)
            x[i] += (float)(a * std::sin(2 * kPi * hz * i / sr + ph0));
    }
    float mx = 0;
    for (float v : x)
        mx = std::max(mx, std::fabs(v));
    for (float &v : x)
        v *= 0.3f / mx;
    return x;
}

// ---------------------------------------------------------------- 処理方式

enum Engine { kPsola, kVocoder, kVocoderLow };
const Engine kEngines[] = {kPsola, kVocoder, kVocoderLow};

const char *engine_name(Engine e) {
    switch (e) {
    case kPsola:
        return "PSOLA      ";
    case kVocoder:
        return "PV 高音質  ";
    default:
        return "PV 低遅延  ";
    }
}

// pitch_end を指定すると、処理の途中（半分の所）でピッチをそこへ切り替える
std::vector<float> run(const std::vector<float> &in, int sr, float pitch_st, float formant_st, Engine e,
                       int *latency = nullptr, int block = 480, float pitch_end = NAN) {
    pf::VoiceShifter vs;
    pf::PsolaShifter ps;
    if (e == kPsola)
        ps.init(sr, std::getenv("PF_LAT") ? std::atof(std::getenv("PF_LAT")) : 0.032);
    else
        vs.init(sr, e == kVocoderLow ? 1024 : 2048, 8);
    pf::ShifterParams p;
    p.pitch_ratio = std::pow(2.0f, pitch_st / 12.0f);
    p.formant_ratio = std::pow(2.0f, formant_st / 12.0f);
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); i += block) {
        if (!std::isnan(pitch_end) && i >= in.size() / 2)
            p.pitch_ratio = std::pow(2.0f, pitch_end / 12.0f);
        if (e == kPsola)
            ps.set_params(p);
        else
            vs.set_params(p);
        const int n = (int)std::min<size_t>(block, in.size() - i);
        if (e == kPsola)
            ps.process(in.data() + i, out.data() + i, n);
        else
            vs.process(in.data() + i, out.data() + i, n);
    }
    if (latency)
        *latency = e == kPsola ? ps.latency() : vs.latency();
    return out;
}

// 声の高さが揺れる母音（ビブラート＋ゆっくりした上がり下がり）。f0_scale 倍の高さ・fratio 倍の響きで作れる
std::vector<float> make_live_vowel(int sr, double seconds, double f0, const double fm[3], double f0_scale,
                                   double fratio, double onset = 0.0) {
    const size_t n = (size_t)(sr * seconds);
    std::vector<float> x(n, 0.0f);
    std::vector<double> phase(200, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double t = (double)i / sr;
        if (t < onset)
            continue;
        const double cents = 40.0 * std::sin(2 * kPi * 5.5 * t) + 150.0 * std::sin(2 * kPi * 0.4 * t);
        const double hz0 = f0 * std::pow(2.0, cents / 1200.0) * f0_scale;
        double v = 0;
        for (int h = 1; h < 200 && h * hz0 < sr * 0.45; ++h) {
            phase[h] += 2 * kPi * h * hz0 / sr;
            v += vowel_env(h * hz0 / fratio, fm) * std::sin(phase[h] + 0.37 * h * h);
        }
        x[i] = (float)(v * 0.05);
    }
    return x;
}

// 短時間スペクトルの形の違い（dB、50Hz〜5kHz）。小さいほど「理想の声」に近い
double spectral_distance(const std::vector<float> &a, const std::vector<float> &b, int sr, size_t from, size_t to) {
    const int n = 2048;
    pf::RealFFT fft(n);
    std::vector<float> fa(n), fb(n);
    std::vector<pf::cpx> sa(n / 2 + 1), sb(n / 2 + 1);
    const int k0 = 50 * n / sr, k1 = 5000 * n / sr;
    double total = 0;
    int frames = 0;
    for (size_t s = from; s + n <= to; s += n / 4) {
        for (int i = 0; i < n; ++i) {
            const float w = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / n));
            fa[i] = a[s + i] * w;
            fb[i] = b[s + i] * w;
        }
        fft.forward(fa.data(), sa.data());
        fft.forward(fb.data(), sb.data());
        std::vector<double> la, lb;
        double peak = -1e9;
        for (int k = k0; k <= k1; ++k) {
            la.push_back(10 * std::log10(std::norm(sa[k]) + 1e-12));
            lb.push_back(10 * std::log10(std::norm(sb[k]) + 1e-12));
            peak = std::max(peak, lb.back());
        }
        // 全体の音量差は除き、理想側で聞こえる帯域（最大から -50dB まで）だけ比べる
        double mean = 0;
        int cnt = 0;
        for (size_t k = 0; k < la.size(); ++k)
            if (lb[k] > peak - 50) {
                mean += la[k] - lb[k];
                ++cnt;
            }
        mean /= std::max(cnt, 1);
        double e = 0;
        for (size_t k = 0; k < la.size(); ++k)
            if (lb[k] > peak - 50) {
                const double d = std::clamp(la[k] - lb[k] - mean, -30.0, 30.0);
                e += d * d;
            }
        total += std::sqrt(e / std::max(cnt, 1));
        ++frames;
    }
    return total / std::max(frames, 1);
}

// ---------------------------------------------------------------- テスト

void test_identity() {
    std::printf("\n■ 0 半音・0 半音で元の音がそのまま（遅延だけ）出るか（ノイズで確認）\n");
    const int sr = 48000;
    std::mt19937 rng(1);
    std::normal_distribution<float> nd(0.0f, 0.1f);
    std::vector<float> x(sr * 2);
    for (auto &v : x)
        v = nd(rng);
    for (Engine e : kEngines) {
        int lat = 0;
        auto y = run(x, sr, 0, 0, e, &lat, 441);
        double err = 0, ref = 0;
        for (size_t i = lat + 4096; i < x.size(); ++i) {
            err += std::pow(y[i] - x[i - lat], 2);
            ref += std::pow(x[i - lat], 2);
        }
        const double db = 10 * std::log10(err / ref + 1e-30);
        char msg[160];
        std::snprintf(msg, sizeof msg, "%s: 遅延 %d サンプル (%.1f ms), 誤差 %.1f dB", engine_name(e), lat,
                      lat * 1000.0 / sr, db);
        check(db < -60, msg);
    }
}

void test_pitch() {
    const int sr = 48000;
    const double fm[3] = {700, 1200, 2600};
    for (double f0 : {110.0, 220.0, 300.0}) {
        std::printf("\n■ ピッチの正確さと濁りの少なさ（元 %.0fHz の母音。純度＝倍音以外の成分がどれだけ小さいか）\n", f0);
        auto x = make_vowel(sr, 3.0, f0, fm);
        for (Engine e : kEngines) {
            for (float st : {-12.0f, -5.0f, 4.0f, 7.0f, 12.0f}) {
                auto y = run(x, sr, st, 0, e);
                const double want = f0 * std::pow(2.0, st / 12.0);
                const double f_out = estimate_f0(y, sr, sr, x.size());
                double snr = 0;
                harmonics(y, sr, sr, sr / 2, want, &snr);
                const bool pitch_ok = std::fabs(1200 * std::log2(f_out / want)) < 10 ||
                                      std::fabs(1200 * std::log2(f_out * 2 / want)) < 10 ||
                                      std::fabs(1200 * std::log2(f_out * 3 / want)) < 10;
                char msg[200];
                std::snprintf(msg, sizeof msg, "%s %+5.1f 半音: 狙い %.1f Hz, 純度 %.1f dB", engine_name(e), st, want, snr);
                if (e == kVocoderLow && f0 < 150) {
                    // 低遅延（FFT 1024）では低い声の倍音を分けきれない。README で「高い声向け」と案内している
                    std::printf("  [参考] %s\n", msg);
                    continue;
                }
                check(pitch_ok && snr > 25.0, msg);
            }
        }
    }
}

// 包絡の形がどれだけ狙いどおりか（4kHz までの倍音ごとの dB 差の RMS。全体の音量差は除く）
double envelope_error_db(const std::vector<double> &amps, double f0, double fratio, const double fm[3]) {
    // 聞こえにくい谷（最大より 35dB 以上小さい所）は数えない
    double top = -1e9;
    for (size_t h = 0; h < amps.size() && (h + 1) * f0 <= 4000; ++h)
        top = std::max(top, 20 * std::log10(vowel_env((h + 1) * f0 / fratio, fm)));
    std::vector<double> d;
    for (size_t h = 0; h < amps.size(); ++h) {
        const double hz = (h + 1) * f0;
        if (hz > 4000)
            break;
        if (20 * std::log10(vowel_env(hz / fratio, fm)) < top - 35)
            continue;
        d.push_back(20 * std::log10(amps[h] + 1e-9) - 20 * std::log10(vowel_env(hz / fratio, fm)));
    }
    double mean = 0;
    for (double v : d)
        mean += v;
    mean /= d.size();
    double e = 0;
    for (double v : d)
        e += (v - mean) * (v - mean);
    return std::sqrt(e / d.size());
}

void test_formant() {
    std::printf("\n■ フォルマント（声の響き）が狙いの位置にあるか（包絡のずれ。小さいほど良い）\n");
    const int sr = 48000;
    const double fm[3] = {700, 1200, 2600};
    for (Engine e : {kPsola, kVocoder}) {
        for (double f0 : {110.0, 220.0, 300.0}) {
            auto x = make_vowel(sr, 3.0, f0, fm);
            struct Case {
                float pitch, formant;
            } cases[] = {{12, 0}, {5, 0}, {-7, 0}, {0, 4}, {0, -4}, {7, 3}, {-5, -3}};
            for (auto &c : cases) {
                auto y = run(x, sr, c.pitch, c.formant, e);
                const double p = std::pow(2.0, c.pitch / 12.0), f = std::pow(2.0, c.formant / 12.0);
                auto amps = harmonics(y, sr, sr, sr / 2, f0 * p, nullptr);
                const double err = envelope_error_db(amps, f0 * p, f, fm);
                const double naive = envelope_error_db(amps, f0 * p, p, fm);
                char msg[220];
                std::snprintf(msg, sizeof msg, "%s 元 %.0fHz ピッチ %+3.0f / フォルマント %+3.0f: ずれ %.1f dB（補正なし %.1f dB）",
                              engine_name(e), f0, c.pitch, c.formant, err, naive);
                // PSOLA は 2 周期分の短いグレインなので、響きの山が少しなまる（それでも補正なしの約 1/3）
                check(err < (e == kPsola ? 5.0 : 3.5) && err < naive, msg);
            }
        }
    }
}

void test_live_voice() {
    std::printf("\n■ 揺れのある声（ビブラート＋抑揚）での自然さ（理想の声とのスペクトルの違い。小さいほど良い）\n");
    const int sr = 48000;
    const double fm[3] = {650, 1100, 2500};
    for (double f0 : {120.0, 230.0}) {
        auto x = make_live_vowel(sr, 3.0, f0, fm, 1.0, 1.0);
        struct Case {
            float pitch, formant;
        } cases[] = {{5, 0}, {-5, 0}, {12, 0}, {4, 3}, {-4, -3}};
        for (auto &c : cases) {
            const double p = std::pow(2.0, c.pitch / 12.0), f = std::pow(2.0, c.formant / 12.0);
            auto ideal = make_live_vowel(sr, 3.0, f0, fm, p, f);
            double dist[3] = {};
            for (Engine e : kEngines) {
                int lat = 0;
                auto y = run(x, sr, c.pitch, c.formant, e, &lat);
                y.erase(y.begin(), y.begin() + lat);
                y.resize(x.size(), 0.0f);
                dist[e] = spectral_distance(y, ideal, sr, sr / 2, x.size() - sr / 4);
            }
            char msg[220];
            std::snprintf(msg, sizeof msg, "元 %.0fHz ピッチ %+3.0f / フォルマント %+3.0f: PSOLA %.1f dB / PV 高音質 %.1f dB / PV 低遅延 %.1f dB",
                          f0, c.pitch, c.formant, dist[kPsola], dist[kVocoder], dist[kVocoderLow]);
            check(dist[kPsola] < 6.0 && dist[kPsola] <= dist[kVocoder] + 0.5, msg);
        }
    }
}

void test_pre_echo() {
    std::printf("\n■ 二重に聞こえる原因になる「にじみ」：声が始まる前・終わった後に漏れる音（小さいほど良い）\n");
    const int sr = 48000;
    const double fm[3] = {650, 1100, 2500};
    auto x = make_live_vowel(sr, 1.6, 180.0, fm, 1.0, 1.0, 0.6);
    const size_t on = (size_t)(0.6 * sr), off = (size_t)(1.1 * sr);
    for (size_t i = off; i < x.size(); ++i)
        x[i] = 0.0f; // 0.6〜1.1 秒だけ声がある
    for (float st : {5.0f, -5.0f}) {
        for (Engine e : kEngines) {
            int lat = 0;
            auto y = run(x, sr, st, 0, e, &lat);
            y.erase(y.begin(), y.begin() + lat);
            y.resize(x.size(), 0.0f);
            const size_t ms = sr / 1000;
            const double body = rms(y, on + 50 * ms, off - 50 * ms);
            const double pre = rms(y, on - 40 * ms, on - 6 * ms);
            const double post = rms(y, off + 6 * ms, off + 40 * ms);
            const double pre_db = 20 * std::log10(pre / body + 1e-9), post_db = 20 * std::log10(post / body + 1e-9);
            char msg[200];
            std::snprintf(msg, sizeof msg, "%s ピッチ %+3.0f: 前に漏れる音 %.1f dB / 後に残る音 %.1f dB", engine_name(e), st,
                          pre_db, post_db);
            if (e == kPsola)
                check(pre_db < -40 && post_db < -40, msg);
            else
                std::printf("  [参考] %s\n", msg);
        }
    }
}

// 出力の高さを 2.5 周期ぶんの短い窓で細かく測り、狙いの高さ（入力の揺れ × ピッチ倍率）からのずれ（セント）を返す
void pitch_wobble(const std::vector<float> &y, int sr, double f0, double ratio, double *rms_cents, double *p95_cents) {
    std::vector<double> errs;
    for (size_t s = sr / 2; s + 4 * sr / 100 < y.size() - sr / 4; s += sr / 400) {
        const double t = (double)s / sr;
        const double cents = 40.0 * std::sin(2 * kPi * 5.5 * t) + 150.0 * std::sin(2 * kPi * 0.4 * t);
        const double want = f0 * std::pow(2.0, cents / 1200.0) * ratio;
        const double lag0 = sr / want;
        const int w = (int)(2.5 * lag0);
        const int lo = (int)(lag0 * 0.8), hi = (int)(lag0 * 1.25) + 1;
        std::vector<double> r(hi + 2, -1.0);
        double best = -2;
        int bl = lo;
        for (int lag = lo - 1; lag <= hi + 1; ++lag) {
            double a = 0, e0 = 0, e1 = 0;
            for (int i = 0; i < w; ++i) {
                const double u = y[s + i], v = y[s + i + lag];
                a += u * v;
                e0 += u * u;
                e1 += v * v;
            }
            r[lag] = a / std::sqrt(e0 * e1 + 1e-18);
            if (lag >= lo && lag <= hi && r[lag] > best) {
                best = r[lag];
                bl = lag;
            }
        }
        const double den = r[bl - 1] - 2 * r[bl] + r[bl + 1];
        const double off = den < 0 ? 0.5 * (r[bl - 1] - r[bl + 1]) / den : 0.0;
        errs.push_back(std::fabs(1200 * std::log2((sr / (bl + off)) / want)));
    }
    double sq = 0;
    for (double e : errs)
        sq += e * e;
    *rms_cents = std::sqrt(sq / errs.size());
    std::sort(errs.begin(), errs.end());
    *p95_cents = errs[errs.size() * 95 / 100];
}

void test_wobble() {
    std::printf("\n■ 高い声でも音程がヨレないか（狙いの高さからのずれ。セント、小さいほど良い）\n");
    const int sr = 48000;
    const double fm[3] = {650, 1100, 2500};
    for (double f0 : {150.0, 300.0, 450.0, 650.0}) {
        auto x = make_live_vowel(sr, 3.0, f0, fm, 1.0, 1.0);
        double in_rms, in_p95;
        pitch_wobble(x, sr, f0, 1.0, &in_rms, &in_p95);
        for (float st : {4.0f, -4.0f}) {
            int lat = 0;
            auto y = run(x, sr, st, st / 2, kPsola, &lat);
            y.erase(y.begin(), y.begin() + lat);
            y.resize(x.size(), 0.0f);
            double r, p;
            pitch_wobble(y, sr, f0, std::pow(2.0, st / 12.0), &r, &p);
            char msg[200];
            std::snprintf(msg, sizeof msg, "元 %3.0fHz ピッチ %+2.0f / フォルマント %+2.0f: ずれ 平均 %.1f / 95%% %.1f セント（入力そのものは %.1f / %.1f）",
                          f0, st, st / 2, r, p, in_rms, in_p95);
            // 測り方そのものの誤差（入力を同じ方法で測った値）と比べる
            check(r < in_rms * 1.3 + 3.0 && p < in_p95 * 1.3 + 5.0, msg);
        }
    }
}

void test_onset() {
    std::printf("\n■ 子音のあとの声の出だしから、ちゃんと高さが変わるか（出だし 10ms ごとの狙いとのずれ、セント）\n");
    const int sr = 48000;
    const double fm[3] = {650, 1100, 2500};
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.0f, 0.02f);
    double worst_all = 0;
    for (double f0 : {140.0, 220.0}) {
        // 0.5 秒の無音 → 80ms の「さ」っぽい息の音 → 声（0.4 秒）→ 無音、を 3 回
        std::vector<float> x(sr * 3, 0.0f);
        std::vector<size_t> onsets;
        for (int k = 0; k < 3; ++k) {
            const size_t s = (size_t)((0.5 + k * 0.8) * sr), v = s + (size_t)(0.08 * sr);
            for (size_t i = s; i < v; ++i)
                x[i] = nd(rng);
            auto vowel = make_vowel(sr, 0.4, f0 * std::pow(2.0, k / 12.0), fm);
            for (size_t i = 0; i < vowel.size(); ++i)
                x[v + i] = vowel[i] * (float)std::min(1.0, i / (0.01 * sr)); // 10ms で立ち上がる
            onsets.push_back(v);
        }
        int lat = 0;
        auto y = run(x, sr, 4, 2, kPsola, &lat);
        y.erase(y.begin(), y.begin() + lat);
        y.resize(x.size(), 0.0f);
        std::printf("  元 %.0fHz:", f0);
        double worst = 0;
        for (int k = 0; k < 3; ++k) {
            const double want = f0 * std::pow(2.0, k / 12.0) * std::pow(2.0, 4 / 12.0);
            for (int ms = 15; ms <= 55; ms += 10) {
                const size_t s = onsets[k] + (size_t)(ms * sr / 1000);
                const int lag0 = (int)(sr / want);
                double best = -2, bl = lag0;
                for (int lag = (int)(lag0 * 0.8); lag <= (int)(lag0 * 1.25); ++lag) {
                    double a = 0, e0 = 0, e1 = 0;
                    for (int i = 0; i < 2 * lag0; ++i) {
                        a += y[s + i] * y[s + i + lag];
                        e0 += y[s + i] * y[s + i];
                        e1 += y[s + i + lag] * y[s + i + lag];
                    }
                    const double r = a / std::sqrt(e0 * e1 + 1e-18);
                    if (r > best) {
                        best = r;
                        bl = lag;
                    }
                }
                const double c = 1200 * std::log2((sr / bl) / want);
                if (k == 0)
                    std::printf(" %dms:%+.0f", ms, c);
                worst = std::max(worst, std::fabs(c));
            }
        }
        std::printf("  （3 回の出だしで最大 %.0f セント）\n", worst);
        worst_all = std::max(worst_all, worst);
    }
    char msg[120];
    std::snprintf(msg, sizeof msg, "声の出だし 15ms 以降のずれ 最大 %.0f セント", worst_all);
    check(worst_all < 40, msg);
}

void test_quiet() {
    std::printf("\n■ 小さい声・マイクから遠い声でも地声が漏れないか（狙いの高さからのずれ。地声が漏れると数百セントずれる）\n");
    const int sr = 48000;
    const double fm[3] = {600, 1100, 2400};
    std::mt19937 rng(11);
    for (double f0 : {110.0, 140.0}) {
        auto base = make_live_vowel(sr, 3.0, f0, fm, 1.0, 1.0);
        double in_rms, in_p95;
        pitch_wobble(base, sr, f0, 1.0, &in_rms, &in_p95);
        // 声の大きさ（RMS）を指定の dBFS にそろえ、そこから 25dB 小さい雑音を足す
        const double base_rms = rms(base, 0, base.size());
        for (double level_db : {-30.0, -50.0, -65.0}) {
            const double gain = std::pow(10.0, level_db / 20.0) / base_rms;
            std::normal_distribution<float> nd(0.0f, (float)std::pow(10.0, (level_db - 25.0) / 20.0));
            std::vector<float> x(base.size());
            for (size_t i = 0; i < x.size(); ++i)
                x[i] = (float)(base[i] * gain) + nd(rng);
            int lat = 0;
            auto y = run(x, sr, 5, 2, kPsola, &lat);
            y.erase(y.begin(), y.begin() + lat);
            y.resize(x.size(), 0.0f);
            double r, p;
            pitch_wobble(y, sr, f0, std::pow(2.0, 5 / 12.0), &r, &p);
            char msg[200];
            std::snprintf(msg, sizeof msg, "元 %.0fHz・声の大きさ %.0fdBFS を +5 半音: ずれ 平均 %.1f / 95%% %.1f セント", f0, level_db, r, p);
            check(p < in_p95 * 1.3 + 15.0, msg);
        }
    }
}

void test_pakitto() {
    std::printf("\n■ ぱきっと：3 バンドイコライザで中域を消してから処理\n");
    const int sr = 48000;
    for (double hz : {150.0, 400.0, 1000.0, 2000.0, 3500.0, 8000.0, 12000.0}) {
        pf::ThreeBandEq eq;
        eq.init(sr);
        std::vector<float> x(sr / 2), y(sr / 2);
        for (size_t i = 0; i < x.size(); ++i) {
            x[i] = (float)(0.5 * std::sin(2 * kPi * hz * i / sr));
            y[i] = eq.process(x[i], 1.0f, 0.0f, 1.0f);
        }
        const double db = 20 * std::log10(rms(y, sr / 4, y.size()) / rms(x, sr / 4, x.size()) + 1e-12);
        char msg[120];
        const bool mid = hz > 1500 && hz < 4000;
        std::snprintf(msg, sizeof msg, "%6.0f Hz の音: %+6.1f dB（%s）", hz, db, mid ? "中域なので下がるはず" : "低域・高域なので残るはず");
        // 3 バンドイコライザは帯域の境目がゆるやかなので、中域でも穴のように 0 にはならない（OBS 標準と同じ性質）
        if (mid)
            check(db < -5, msg);
        else if (hz < 300 || hz > 10000)
            check(db > -3, msg);
        else
            std::printf("  [参考] %s 境目に近いので一部だけ残る\n", msg);
    }
    // はっきり（中域 -10dB）は、ぱきっとより浅く、元の音より深く下がるか
    {
        auto through = [&](float mid_gain, double hz) {
            pf::ThreeBandEq eq;
            eq.init(sr);
            std::vector<float> x(sr / 2), y(sr / 2);
            for (size_t i = 0; i < x.size(); ++i) {
                x[i] = (float)(0.5 * std::sin(2 * kPi * hz * i / sr));
                y[i] = eq.process(x[i], 1.0f, mid_gain, 1.0f);
            }
            return 20 * std::log10(rms(y, sr / 4, y.size()) / rms(x, sr / 4, x.size()) + 1e-12);
        };
        const double hk = through((float)std::pow(10.0, -10.0 / 20.0), 2000.0), pk = through(0.0f, 2000.0);
        char msg[160];
        std::snprintf(msg, sizeof msg, "はっきり：2000Hz で %+.1f dB（ぱきっとは %+.1f dB）", hk, pk);
        check(hk < -4.0 && hk > pk + 2.0, msg);
    }
    // 中域を消した声でも、ピッチは狙いどおりに変わるか
    const double fm[3] = {700, 1200, 2600};
    auto x = make_vowel(sr, 3.0, 180.0, fm);
    pf::ThreeBandEq eq;
    eq.init(sr);
    for (auto &v : x)
        v = eq.process(v, 1.0f, 0.0f, 1.0f);
    auto y = run(x, sr, 4, 2, kPsola);
    const double want = 180.0 * std::pow(2.0, 4 / 12.0);
    double snr = 0;
    harmonics(y, sr, sr, sr / 2, want, &snr);
    const double f_out = estimate_f0(y, sr, sr, x.size());
    char msg[160];
    std::snprintf(msg, sizeof msg, "中域を消した声を +4 半音: %.1f Hz（狙い %.1f Hz）、純度 %.1f dB", f_out, want, snr);
    check(std::fabs(1200 * std::log2(f_out / want)) < 10 && snr > 25, msg);
}

void test_glide() {
    std::printf("\n■ 途中でピッチを変えたとき、なめらかに移るか（切り替え前後の 100ms で音が途切れたり跳ねたりしないか）\n");
    const int sr = 48000;
    const double fm[3] = {650, 1100, 2500};
    auto x = make_vowel(sr, 2.0, 200.0, fm);
    auto y = run(x, sr, 0, 0, kPsola, nullptr, 480, 7.0f);
    const size_t mid = x.size() / 2 + (size_t)(0.032 * sr);
    double worst = 0;
    const double base = rms(y, mid - sr / 4, mid - sr / 8);
    for (size_t s = mid - sr / 10; s < mid + sr / 10; s += sr / 200) {
        const double r = rms(y, s, s + sr / 200);
        worst = std::max(worst, std::fabs(20 * std::log10(r / base + 1e-9)));
    }
    const double f_after = estimate_f0(y, sr, mid + sr / 4, y.size());
    char msg[200];
    std::snprintf(msg, sizeof msg, "PSOLA 0→+7 半音: 5ms ごとの音量の揺れ 最大 %.1f dB、切り替え後 %.1f Hz（狙い %.1f Hz）", worst,
                  f_after, 200 * std::pow(2.0, 7 / 12.0));
    check(worst < 4.0 && std::fabs(1200 * std::log2(f_after / (200 * std::pow(2.0, 7 / 12.0)))) < 10, msg);
}

void test_speed() {
    std::printf("\n■ 処理の重さ（48kHz・1 チャンネル・60 秒分）\n");
    const int sr = 48000;
    const double fm[3] = {600, 1500, 2500};
    auto x = make_vowel(sr, 60.0, 130.0, fm);
    for (Engine e : kEngines) {
        const auto t0 = std::chrono::steady_clock::now();
        auto y = run(x, sr, 5, 2, e);
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char msg[160];
        std::snprintf(msg, sizeof msg, "%s: %.2f 秒で処理（CPU 1 コアの約 %.1f%%）", engine_name(e), sec, sec / 60.0 * 100);
        check(sec < 6.0, msg);
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc >= 2 && !std::strcmp(argv[1], "render") && argc >= 6) {
        std::vector<float> x;
        int sr = 0;
        if (!read_wav(argv[2], x, sr)) {
            std::fprintf(stderr, "WAV を読めません: %s\n", argv[2]);
            return 1;
        }
        // 最後の引数のどこかに pakitto があれば、OBS の「ぱきっと」と同じく中域を下げてから処理する
        for (int a = 6; a < argc; ++a) {
            if (!std::strcmp(argv[a], "pakitto")) {
                pf::ThreeBandEq eq;
                eq.init(sr);
                for (auto &v : x)
                    v = eq.process(v, 1.0f, 0.0f, 1.0f);
            }
        }
        Engine e = kPsola;
        if (argc >= 7 && !std::strcmp(argv[6], "pv"))
            e = kVocoder;
        if (argc >= 7 && !std::strcmp(argv[6], "pvlow"))
            e = kVocoderLow;
        int lat = 0;
        x.resize(x.size() + 4096, 0.0f);
        std::vector<float> y;
        if (argc >= 7 && !std::strcmp(argv[6], "sweep")) {
            // ピッチを「指定値 ± 6 半音」で 4 秒周期でゆっくり上げ下げする（なめらかさの確認用）
            pf::PsolaShifter ps;
            ps.init(sr);
            pf::ShifterParams p;
            p.formant_ratio = std::pow(2.0f, (float)std::atof(argv[5]) / 12.0f);
            y.resize(x.size());
            for (size_t i = 0; i < x.size(); i += 240) {
                const double st = std::atof(argv[4]) + 6.0 * std::sin(2 * kPi * (double)i / sr / 4.0);
                p.pitch_ratio = (float)std::pow(2.0, st / 12.0);
                ps.set_params(p);
                const int n = (int)std::min<size_t>(240, x.size() - i);
                ps.process(x.data() + i, y.data() + i, n);
            }
            lat = ps.latency();
        } else {
            y = run(x, sr, (float)std::atof(argv[4]), (float)std::atof(argv[5]), e, &lat);
        }
        y.erase(y.begin(), y.begin() + lat);
        write_wav(argv[3], y, sr);
        std::printf("%s を書き出しました（%d Hz）\n", argv[3], sr);
        return 0;
    }
    const char *only = argc >= 2 && std::strcmp(argv[1], "test") ? argv[1] : "";
    auto want = [&](const char *name) { return !*only || !std::strcmp(only, name); };
    if (want("identity"))
        test_identity();
    if (want("pitch"))
        test_pitch();
    if (want("formant"))
        test_formant();
    if (want("live"))
        test_live_voice();
    if (want("echo"))
        test_pre_echo();
    if (want("wobble"))
        test_wobble();
    if (want("onset"))
        test_onset();
    if (want("quiet"))
        test_quiet();
    if (want("pakitto"))
        test_pakitto();
    if (want("glide"))
        test_glide();
    if (want("speed"))
        test_speed();
    std::printf("\n%s（失敗 %d 件）\n", g_fail ? "失敗あり" : "すべて合格", g_fail);
    return g_fail ? 1 : 0;
}
