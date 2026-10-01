// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 hakoniwa
// plugin-main.cpp
//
// OBS の音声フィルタ「HKPitch」（声のピッチ・フォルマント変換）。音の加工は voice-shifter.cpp が行い、
// ここは OBS とのやりとり（設定画面・チャンネルの振り分け・スレッドの守り）だけを受け持つ。

#include <obs-module.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "psola.h"
#include "three-band-eq.h"

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("hakoniwa")
OBS_MODULE_USE_DEFAULT_LOCALE("HKPitch", "en-US")

#define T_(s) obs_module_text(s)

#define S_PAKITTO "pakitto"
#define S_PRESET "preset"
#define S_PITCH "pitch"
#define S_FORMANT "formant"
#define S_QUALITY "quality"
#define S_MIX "mix"
#define S_GAIN "gain"

namespace {

constexpr size_t kMaxChannels = MAX_AUDIO_CHANNELS;

// どれも声向けの PSOLA。違いは遅延だけ（短いほど遅れは減るが、低い声や声の出だしが少し苦しくなる）
enum Mode { kQuality = 0, kBalanced = 1, kLowLatency = 2 };
double mode_latency(int mode) {
    return mode == kLowLatency ? 0.020 : mode == kBalanced ? 0.024 : 0.032;
}

struct Preset {
    const char *label;
    double pitch, formant;
};

// 0 番は「手動」。選ぶと値を入れて手動に戻る（あとから自由に微調整できる）
// 0 番の「手動」のあとは、声の高い順
const Preset kPresets[] = {
    {"HKPitch.Preset.Custom", 0, 0},     {"HKPitch.Preset.Child", 4, 2},
    {"HKPitch.Preset.Girl", 2, 1},       {"HKPitch.Preset.Polite", 1, 0.5},
    {"HKPitch.Preset.Reset", 0, 0},      {"HKPitch.Preset.Boy", -2, -1},
    {"HKPitch.Preset.Man", -4, -2},      {"HKPitch.Preset.DeepMan", -8, -4},
};

struct Filter {
    obs_source_t *context = nullptr;
    std::mutex mtx;
    uint32_t sample_rate = 0;
    int quality = -1;
    size_t channels = 0;
    pf::ShifterParams params;
    pf::PsolaShifter ch[kMaxChannels];
    pf::ThreeBandEq eq[kMaxChannels]; // 「ぱきっと」用の前処理
    bool pakitto = false;
    bool follows_ch0[kMaxChannels] = {}; // 直前まで 0 番と同じ音だったので処理を省いたチャンネル
    uint64_t last_ts = 0;
};

void init_shifters(Filter *f) {
    for (size_t c = 0; c < kMaxChannels; ++c) {
        if (c < f->channels)
            f->ch[c].init(f->sample_rate, mode_latency(f->quality));
        f->eq[c].init(f->sample_rate);
        f->follows_ch0[c] = false;
    }
}

const char *pf_name(void *) {
    return T_("HKPitch.Name");
}

void pf_update(void *data, obs_data_t *s) {
    auto *f = static_cast<Filter *>(data);
    const uint32_t sr = audio_output_get_sample_rate(obs_get_audio());
    const size_t chs = std::clamp<size_t>(audio_output_get_channels(obs_get_audio()), 1, kMaxChannels);
    const int quality = std::clamp((int)obs_data_get_int(s, S_QUALITY), (int)kQuality, (int)kLowLatency);

    pf::ShifterParams p;
    p.pitch_ratio = (float)std::pow(2.0, obs_data_get_double(s, S_PITCH) / 12.0);
    p.formant_ratio = (float)std::pow(2.0, obs_data_get_double(s, S_FORMANT) / 12.0);
    p.mix = (float)(obs_data_get_double(s, S_MIX) / 100.0);
    p.gain = (float)std::pow(10.0, obs_data_get_double(s, S_GAIN) / 20.0);
    const bool pakitto = obs_data_get_bool(s, S_PAKITTO);

    std::lock_guard<std::mutex> lock(f->mtx);
    if (sr != f->sample_rate || chs != f->channels || quality != f->quality) {
        f->sample_rate = sr;
        f->channels = chs;
        f->quality = quality;
        init_shifters(f);
    }
    f->params = p;
    f->pakitto = pakitto;
    for (size_t c = 0; c < f->channels; ++c)
        f->ch[c].set_params(p);
}

void *pf_create(obs_data_t *settings, obs_source_t *source) {
    auto *f = new Filter();
    f->context = source;
    pf_update(f, settings);
    return f;
}

void pf_destroy(void *data) {
    delete static_cast<Filter *>(data);
}

obs_audio_data *pf_filter_audio(void *data, obs_audio_data *audio) {
    auto *f = static_cast<Filter *>(data);
    const uint32_t frames = audio->frames;
    if (frames == 0)
        return audio;

    std::lock_guard<std::mutex> lock(f->mtx);

    // 長く途切れたあとは、前の音の残りが混ざらないように空にする
    if (f->last_ts && (audio->timestamp < f->last_ts || audio->timestamp - f->last_ts > 500000000ULL)) {
        for (size_t c = 0; c < f->channels; ++c)
            f->ch[c].reset();
    }
    f->last_ts = audio->timestamp;

    // 「ぱきっと」：3 バンドイコライザで中域（800Hz〜5kHz）を完全に消してから処理する
    if (f->pakitto) {
        for (size_t c = 0; c < f->channels; ++c) {
            auto *d = reinterpret_cast<float *>(audio->data[c]);
            if (!d)
                continue;
            for (uint32_t i = 0; i < frames; ++i)
                d[i] = f->eq[c].process(d[i], 1.0f, 0.0f, 1.0f);
        }
    }

    auto *ch0 = reinterpret_cast<float *>(audio->data[0]);
    if (!ch0)
        return audio;

    // マイクはたいてい全チャンネル同じ音。そのときは 0 番だけ処理して写す（負荷が半分以下になる）
    bool all_same = true;
    for (size_t c = 1; c < f->channels && all_same; ++c) {
        const auto *d = reinterpret_cast<const float *>(audio->data[c]);
        if (d && std::memcmp(d, ch0, sizeof(float) * frames) != 0)
            all_same = false;
    }

    if (all_same) {
        f->ch[0].process(ch0, ch0, (int)frames);
        for (size_t c = 1; c < f->channels; ++c) {
            auto *d = reinterpret_cast<float *>(audio->data[c]);
            if (!d)
                continue;
            std::memcpy(d, ch0, sizeof(float) * frames);
            f->follows_ch0[c] = true;
        }
        return audio;
    }

    // 別々の音になった：省いていたチャンネルは 0 番の状態（ここまで同じ入力だった）を引き継ぐ
    for (size_t c = 1; c < f->channels; ++c) {
        if (f->follows_ch0[c]) {
            f->ch[c] = f->ch[0];
            f->follows_ch0[c] = false;
        }
    }
    for (size_t c = 0; c < f->channels; ++c) {
        auto *d = reinterpret_cast<float *>(audio->data[c]);
        if (d)
            f->ch[c].process(d, d, (int)frames);
    }
    return audio;
}

void pf_defaults(obs_data_t *s) {
    obs_data_set_default_bool(s, S_PAKITTO, true);
    obs_data_set_default_int(s, S_PRESET, 0);
    obs_data_set_default_double(s, S_PITCH, 0.0);
    obs_data_set_default_double(s, S_FORMANT, 0.0);
    obs_data_set_default_int(s, S_QUALITY, kQuality);
    obs_data_set_default_double(s, S_MIX, 100.0);
    obs_data_set_default_double(s, S_GAIN, 0.0);
}

bool preset_changed(obs_properties_t *, obs_property_t *, obs_data_t *s) {
    const long long i = obs_data_get_int(s, S_PRESET);
    if (i <= 0 || i >= (long long)(sizeof kPresets / sizeof kPresets[0]))
        return false;
    obs_data_set_double(s, S_PITCH, kPresets[i].pitch);
    obs_data_set_double(s, S_FORMANT, kPresets[i].formant);
    obs_data_set_int(s, S_PRESET, 0);
    return true;
}

obs_properties_t *pf_properties(void *) {
    obs_properties_t *props = obs_properties_create();

    obs_property_t *pk = obs_properties_add_bool(props, S_PAKITTO, T_("HKPitch.Pakitto"));
    obs_property_set_long_description(pk, T_("HKPitch.Pakitto.Help"));

    obs_property_t *preset = obs_properties_add_list(props, S_PRESET, T_("HKPitch.Preset"), OBS_COMBO_TYPE_LIST,
                                                     OBS_COMBO_FORMAT_INT);
    for (size_t i = 0; i < sizeof kPresets / sizeof kPresets[0]; ++i)
        obs_property_list_add_int(preset, T_(kPresets[i].label), (long long)i);
    obs_property_set_modified_callback(preset, preset_changed);

    obs_property_t *p = obs_properties_add_float_slider(props, S_PITCH, T_("HKPitch.Pitch"), -24.0, 24.0, 0.1);
    obs_property_float_set_suffix(p, T_("HKPitch.Semitones"));
    obs_property_set_long_description(p, T_("HKPitch.Pitch.Help"));

    p = obs_properties_add_float_slider(props, S_FORMANT, T_("HKPitch.Formant"), -12.0, 12.0, 0.1);
    obs_property_float_set_suffix(p, T_("HKPitch.Semitones"));
    obs_property_set_long_description(p, T_("HKPitch.Formant.Help"));

    p = obs_properties_add_list(props, S_QUALITY, T_("HKPitch.Quality"), OBS_COMBO_TYPE_LIST,
                                OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(p, T_("HKPitch.Quality.High"), kQuality);
    obs_property_list_add_int(p, T_("HKPitch.Quality.Balanced"), kBalanced);
    obs_property_list_add_int(p, T_("HKPitch.Quality.Low"), kLowLatency);
    obs_property_set_long_description(p, T_("HKPitch.Quality.Help"));

    p = obs_properties_add_float_slider(props, S_MIX, T_("HKPitch.Mix"), 0.0, 100.0, 1.0);
    obs_property_float_set_suffix(p, "%");

    p = obs_properties_add_float_slider(props, S_GAIN, T_("HKPitch.Gain"), -24.0, 24.0, 0.1);
    obs_property_float_set_suffix(p, " dB");

    obs_properties_add_text(props, "info", T_("HKPitch.Info"), OBS_TEXT_INFO);
    return props;
}

} // namespace

bool obs_module_load(void) {
    static obs_source_info info = {};
    info.id = "hkpitch_filter";
    info.type = OBS_SOURCE_TYPE_FILTER;
    info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = pf_name;
    info.create = pf_create;
    info.destroy = pf_destroy;
    info.update = pf_update;
    info.get_defaults = pf_defaults;
    info.get_properties = pf_properties;
    info.filter_audio = pf_filter_audio;
    obs_register_source(&info);
    blog(LOG_INFO, "[HKPitch] loaded (version %s)", HKPITCH_VERSION);
    return true;
}

MODULE_EXPORT const char *obs_module_description(void) {
    return "HKPitch - natural pitch & formant shifter for voice (https://github.com/852wa/HKPitch)";
}
