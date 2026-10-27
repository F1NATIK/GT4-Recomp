// Custom Sounds: the pack reader and the engine mixer (gt4_sound_pack.h). Not in the Classic build. Standard C++ only (the
// host test builds this file on its own).
#if !defined(GT4_STOCK_BUILD)
#include "gt4_sound_pack.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ps2x::iop::gt4::soundpack
{
    namespace
    {
        constexpr double Pi = 3.14159265358979323846;

        // ---- a small JSON reader (pack.json: objects, arrays, strings, numbers, true / false / null)
        struct Json
        {
            enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
            bool b = false;
            double n = 0.0;
            std::string s;
            std::vector<Json> a;
            std::vector<std::pair<std::string, Json>> o;
            const Json *get(const std::string &key) const
            {
                for (const auto &kv : o)
                    if (kv.first == key)
                        return &kv.second;
                return nullptr;
            }
            double num(const std::string &key, double fallback) const
            {
                const Json *j = get(key);
                return j && j->type == Type::Number ? j->n : fallback;
            }
            std::string str(const std::string &key) const
            {
                const Json *j = get(key);
                return j && j->type == Type::String ? j->s : std::string();
            }
        };

        class JsonReader
        {
        public:
            explicit JsonReader(const std::string &text) : t_(text) {}
            bool parse(Json &out, std::string &error)
            {
                if (!value(out, 0))
                {
                    error = "pack.json is not valid JSON (near byte " + std::to_string(at_) + ")";
                    return false;
                }
                space();
                if (at_ != t_.size())
                {
                    error = "pack.json has text after its end (byte " + std::to_string(at_) + ")";
                    return false;
                }
                return true;
            }

        private:
            void space()
            {
                while (at_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[at_])))
                    ++at_;
            }
            bool literal(const char *word)
            {
                const size_t n = std::strlen(word);
                if (t_.compare(at_, n, word) != 0)
                    return false;
                at_ += n;
                return true;
            }
            bool string(std::string &out)
            {
                if (at_ >= t_.size() || t_[at_] != '"')
                    return false;
                ++at_;
                while (at_ < t_.size() && t_[at_] != '"')
                {
                    char c = t_[at_++];
                    if (c == '\\')
                    {
                        if (at_ >= t_.size())
                            return false;
                        const char e = t_[at_++];
                        switch (e)
                        {
                        case 'n': c = '\n'; break;
                        case 't': c = '\t'; break;
                        case 'r': c = '\r'; break;
                        case 'b': c = '\b'; break;
                        case 'f': c = '\f'; break;
                        case 'u':
                        {
                            if (at_ + 4 > t_.size())
                                return false;
                            const unsigned cp = static_cast<unsigned>(std::strtoul(t_.substr(at_, 4).c_str(), nullptr, 16));
                            at_ += 4;
                            if (cp < 0x80)
                                out.push_back(static_cast<char>(cp));
                            else if (cp < 0x800)
                            {
                                out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                            }
                            else
                            {
                                out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
                                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                                out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                            }
                            continue;
                        }
                        default: c = e; break; // \" \\ \/
                        }
                    }
                    out.push_back(c);
                }
                if (at_ >= t_.size())
                    return false;
                ++at_;
                return true;
            }
            bool value(Json &v, int depth)
            {
                if (depth > 32)
                    return false;
                space();
                if (at_ >= t_.size())
                    return false;
                const char c = t_[at_];
                if (c == '{')
                {
                    v.type = Json::Type::Object;
                    ++at_;
                    space();
                    if (at_ < t_.size() && t_[at_] == '}')
                        return ++at_, true;
                    while (true)
                    {
                        space();
                        std::string key;
                        if (!string(key))
                            return false;
                        space();
                        if (at_ >= t_.size() || t_[at_] != ':')
                            return false;
                        ++at_;
                        Json item;
                        if (!value(item, depth + 1))
                            return false;
                        v.o.emplace_back(std::move(key), std::move(item));
                        space();
                        if (at_ < t_.size() && t_[at_] == ',')
                        {
                            ++at_;
                            continue;
                        }
                        if (at_ < t_.size() && t_[at_] == '}')
                            return ++at_, true;
                        return false;
                    }
                }
                if (c == '[')
                {
                    v.type = Json::Type::Array;
                    ++at_;
                    space();
                    if (at_ < t_.size() && t_[at_] == ']')
                        return ++at_, true;
                    while (true)
                    {
                        Json item;
                        if (!value(item, depth + 1))
                            return false;
                        v.a.push_back(std::move(item));
                        space();
                        if (at_ < t_.size() && t_[at_] == ',')
                        {
                            ++at_;
                            continue;
                        }
                        if (at_ < t_.size() && t_[at_] == ']')
                            return ++at_, true;
                        return false;
                    }
                }
                if (c == '"')
                {
                    v.type = Json::Type::String;
                    return string(v.s);
                }
                if (literal("true"))
                    return v.type = Json::Type::Bool, v.b = true, true;
                if (literal("false"))
                    return v.type = Json::Type::Bool, v.b = false, true;
                if (literal("null"))
                    return v.type = Json::Type::Null, true;
                const char *begin = t_.c_str() + at_;
                char *end = nullptr;
                v.n = std::strtod(begin, &end);
                if (end == begin)
                    return false;
                v.type = Json::Type::Number;
                at_ += static_cast<size_t>(end - begin);
                return true;
            }
            const std::string &t_;
            size_t at_ = 0;
        };

        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
        uint16_t le16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }

        std::string pathText(const fs::path &p)
        {
            const auto u = p.u8string();
            return std::string(u.begin(), u.end());
        }
        fs::path fromUtf8(const std::string &s) { return fs::path(std::u8string(s.begin(), s.end())); }
    }

    bool readWav(const fs::path &file, std::vector<float> &samples, double &rate, std::string &error)
    {
        std::ifstream in(file, std::ios::binary);
        if (!in)
        {
            error = "cannot open " + pathText(file.filename());
            return false;
        }
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0)
        {
            error = pathText(file.filename()) + " is not a WAV file";
            return false;
        }
        size_t pos = 12;
        unsigned tag = 0, channels = 0, bits = 0;
        uint32_t fileRate = 0;
        const uint8_t *raw = nullptr;
        size_t rawBytes = 0;
        while (pos + 8 <= d.size())
        {
            const uint32_t size = le32(d.data() + pos + 4);
            const uint8_t *body = d.data() + pos + 8;
            const size_t avail = std::min<size_t>(size, d.size() - pos - 8);
            if (std::memcmp(d.data() + pos, "fmt ", 4) == 0 && avail >= 16)
            {
                tag = le16(body);
                channels = le16(body + 2);
                fileRate = le32(body + 4);
                bits = le16(body + 14);
                if (tag == 0xfffe && avail >= 26)
                    tag = le16(body + 24);
            }
            else if (std::memcmp(d.data() + pos, "data", 4) == 0)
            {
                raw = body;
                rawBytes = avail;
            }
            pos += 8 + size + (size & 1u);
        }
        if (!raw || !channels || !fileRate || !bits)
        {
            error = pathText(file.filename()) + ": no fmt or data in the WAV file";
            return false;
        }
        const size_t width = bits / 8u, frames = rawBytes / (width * channels);
        if (!((tag == 1 && (bits == 8 || bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && (bits == 32 || bits == 64))))
        {
            error = pathText(file.filename()) + ": WAV format " + std::to_string(tag) + " / " + std::to_string(bits) + "-bit is not read (use 16-bit PCM)";
            return false;
        }
        samples.assign(frames, 0.0f);
        for (size_t f = 0; f < frames; ++f)
        {
            double sum = 0.0;
            for (unsigned c = 0; c < channels; ++c)
            {
                const uint8_t *p = raw + (f * channels + c) * width;
                double v = 0.0;
                if (tag == 3 && bits == 32)
                {
                    float x;
                    std::memcpy(&x, p, 4);
                    v = x;
                }
                else if (tag == 3)
                {
                    double x;
                    std::memcpy(&x, p, 8);
                    v = x;
                }
                else if (bits == 8)
                    v = (int(p[0]) - 128) / 128.0;
                else if (bits == 16)
                    v = int16_t(le16(p)) / 32768.0;
                else if (bits == 24)
                    v = double(int32_t((uint32_t(p[0]) << 8) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 24)) >> 8) / 8388608.0;
                else
                    v = int32_t(le32(p)) / 2147483648.0;
                sum += v;
            }
            samples[f] = static_cast<float>(sum / channels);
        }
        rate = fileRate;
        if (samples.empty())
        {
            error = pathText(file.filename()) + " holds no sound";
            return false;
        }
        return true;
    }

    bool loadPack(const fs::path &folder, Pack &out, std::string &error)
    {
        out = Pack{};
        out.folder = folder;
        std::ifstream in(folder / "pack.json", std::ios::binary);
        if (!in)
        {
            error = "no pack.json in " + pathText(folder);
            return false;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string text = ss.str();
        Json root;
        if (!JsonReader(text).parse(root, error))
            return false;
        if (root.type != Json::Type::Object || root.num("format", 0) != 1.0)
        {
            error = "pack.json is not format 1";
            return false;
        }
        out.name = root.str("name");
        out.cylinders = static_cast<int>(root.num("cylinders", 0));
        if (const Json *s = root.get("settings"); s && s->type == Json::Type::Object)
        {
            out.volume = std::clamp(s->num("volume", 1.0), 0.0, 4.0);
            out.bassDb = std::clamp(s->num("bass_db", 0.0), -24.0, 24.0);
            out.popChance = std::clamp(s->num("pop_chance", 0.6), 0.0, 1.0);
            out.popVolume = std::clamp(s->num("pop_volume", 0.8), 0.0, 4.0);
        }
        auto load = [&](const Json &e, Loop &l, bool needRpm) {
            const std::string file = e.str("file");
            if (file.empty() || file.find("..") != std::string::npos)
            {
                error = "pack.json: a layer without a usable file name";
                return false;
            }
            l.rpm = e.num("rpm", 0.0);
            if (needRpm && !(l.rpm >= 100.0 && l.rpm <= 30000.0))
            {
                error = "pack.json: " + file + " has no rpm";
                return false;
            }
            std::string why;
            if (!readWav(folder / fromUtf8(file), l.data, l.rate, why))
            {
                error = why;
                return false;
            }
            return true;
        };
        if (const Json *layers = root.get("layers"); layers && layers->type == Json::Type::Array)
            for (const Json &e : layers->a)
            {
                Loop l;
                if (!load(e, l, true))
                    return false;
                (e.str("load") == "off" ? out.off : out.on).push_back(std::move(l));
            }
        if (const Json *pops = root.get("pops"); pops && pops->type == Json::Type::Array)
            for (const Json &e : pops->a)
            {
                Loop l;
                if (!load(e, l, false))
                    return false;
                out.pops.push_back(std::move(l));
            }
        if (out.on.empty())
        {
            error = "the pack has no on-throttle loop";
            return false;
        }
        auto byRpm = [](const Loop &a, const Loop &b) { return a.rpm < b.rpm; };
        std::sort(out.on.begin(), out.on.end(), byRpm);
        std::sort(out.off.begin(), out.off.end(), byRpm);
        return true;
    }

    bool parsePackMap(const std::string &text, PackMap &out, std::vector<std::string> &warnings)
    {
        out = PackMap{};
        std::istringstream in(text);
        std::string line;
        int number = 0;
        while (std::getline(in, line))
        {
            ++number;
            if (const size_t hash = line.find('#'); hash != std::string::npos)
                line.erase(hash);
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                line.pop_back();
            size_t i = 0;
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                ++i;
            if (i >= line.size())
                continue;
            const size_t space = line.find_first_of(" \t", i);
            if (space == std::string::npos)
            {
                warnings.push_back("line " + std::to_string(number) + ": no pack folder");
                continue;
            }
            const std::string key = line.substr(i, space - i);
            size_t j = space;
            while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j])))
                ++j;
            const std::string folder = line.substr(j);
            if (key == "all")
            {
                out.all = folder;
                continue;
            }
            char *end = nullptr;
            const unsigned long code = std::strtoul(key.c_str(), &end, 10);
            if (!end || *end || key.empty() || code > 0xffffffffu)
            {
                warnings.push_back("line " + std::to_string(number) + ": '" + key + "' is not a car code or 'all'");
                continue;
            }
            out.car[static_cast<uint32_t>(code)] = folder;
        }
        return true;
    }

    std::string pathUtf8(const fs::path &p) { return pathText(p); }

    fs::path resolvePackFolder(const std::string &value, const fs::path &base)
    {
        const fs::path p = fromUtf8(value);
        return p.is_absolute() ? p : base / p;
    }

    // ---- the mixer
    double Mixer::Voice::next(double ratio)
    {
        const std::vector<float> &d = loop->data;
        const size_t n = d.size();
        const size_t j = static_cast<size_t>(pos);
        const double f = pos - double(j);
        const double a = d[j % n], b = d[(j + 1) % n];
        pos = std::fmod(pos + ratio, double(n));
        return a + (b - a) * f;
    }

    void Mixer::Voice::skip(double ratio) { pos = std::fmod(pos + ratio, double(loop->data.size())); }

    Mixer::Mixer(std::shared_ptr<const Pack> pack, uint32_t seed, double outRate) : pack_(std::move(pack)), rng_(seed), rate_(outRate)
    {
        for (const Loop &l : pack_->on)
            on_.push_back({&l, 0.0});
        for (const Loop &l : pack_->off)
            off_.push_back({&l, 0.0});
        // cmd_preview runs at 44.1 kHz: the same smoothing time at 48 kHz.
        rpmCoef_ = 1.0 - std::pow(1.0 - 0.002, 44100.0 / rate_);
        thrCoef_ = 1.0 - std::pow(1.0 - 0.004, 44100.0 / rate_);
        lpCoef_ = 1.0 - std::exp(-2.0 * Pi * 120.0 / rate_);
        bassGain_ = std::pow(10.0, pack_->bassDb / 20.0) - 1.0;
        // cmd_preview's built-in pop (when the pack has none): exp(-i / 900) x (noise x 0.8 + sin(60 Hz) x 0.6), 0.12 s.
        builtinLoop_.rate = 44100.0;
        std::uniform_real_distribution<double> u(-1.0, 1.0);
        for (int i = 0; i < int(0.12 * 44100.0); ++i)
            builtinLoop_.data.push_back(static_cast<float>(std::exp(-i / 900.0) * (u(rng_) * 0.8 + std::sin(i * 2.0 * Pi * 60.0 / 44100.0) * 0.6)));
    }

    void Mixer::reset(double rpm, double throttle)
    {
        rpmS_ = rpmTarget_ = std::max(rpm, 100.0);
        thrS_ = thrTarget_ = std::clamp(throttle, 0.0, 1.0);
        std::fill(std::begin(thrHistory_), std::end(thrHistory_), thrS_);
        started_ = true;
    }

    void Mixer::firePops(bool lift)
    {
        ++popTriggers_;
        std::uniform_real_distribution<double> u01(0.0, 1.0);
        if (u01(rng_) >= pack_->popChance)
            return;
        const std::vector<Loop> &pops = pack_->pops;
        std::uniform_real_distribution<double> delay(0.05, 0.2), gap(0.03, 0.15), level(0.4, 1.0);
        std::uniform_int_distribution<int> count(1, lift ? 5 : 3);
        uint64_t t = sample_ + static_cast<uint64_t>(rate_ * delay(rng_));
        const int n = count(rng_);
        for (int i = 0; i < n; ++i)
        {
            const Loop *l = pops.empty() ? &builtinLoop_ : &pops[std::uniform_int_distribution<size_t>(0, pops.size() - 1)(rng_)];
            active_.push_back({t, l, static_cast<float>(level(rng_) * pack_->popVolume)});
            ++popsFired_;
            t += static_cast<uint64_t>(rate_ * gap(rng_));
        }
    }

    void Mixer::control(double rpm, double throttle, int gear, double pedal)
    {
        rpm = std::clamp(std::isfinite(rpm) ? rpm : 0.0, 0.0, 30000.0);
        throttle = std::clamp(std::isfinite(throttle) ? throttle : 0.0, 0.0, 1.0);
        pedal = std::clamp(std::isfinite(pedal) ? pedal : 0.0, 0.0, 1.0);
        if (!started_)
        {
            reset(rpm, throttle);
            std::fill(std::begin(thrHistory_), std::end(thrHistory_), pedal);
            pedal_ = pedal;
        }
        double recent = 0.0;
        for (double v : thrHistory_)
            recent = std::max(recent, v);
        const double prev = thrHistory_[(thrAt_ + 5) % 6];
        const bool lifted = recent > 0.6 && pedal < 0.2 && rpm > 4000.0 && pedal_ >= 0.2;
        const bool downshift = gear > 0 && gear_ > 0 && gear < gear_ && rpm > 3000.0;
        const bool blipped = downshift || (prev < 0.2 && pedal > 0.5 && pedal < 0.9);
        if (lifted || blipped)
            firePops(lifted);
        thrHistory_[thrAt_] = pedal;
        thrAt_ = (thrAt_ + 1) % 6;
        pedal_ = pedal;
        rpmTarget_ = std::max(rpm, 100.0);
        thrTarget_ = throttle;
        gear_ = gear;
    }

    double Mixer::blend(std::vector<Voice> &voices, double rpm)
    {
        // cmd_preview's blend: below the lowest or above the highest layer only that layer plays (the others wait); in
        // between, the two layers either side with equal-power weights, and the others keep moving.
        if (voices.empty())
            return 0.0;
        auto ratio = [&](const Voice &v) { return rpm / v.loop->rpm * v.loop->rate / rate_; };
        if (voices.size() == 1 || rpm <= voices.front().loop->rpm)
            return voices.front().next(ratio(voices.front()));
        if (rpm >= voices.back().loop->rpm)
            return voices.back().next(ratio(voices.back()));
        for (size_t i = 0; i + 1 < voices.size(); ++i)
        {
            Voice &a = voices[i], &b = voices[i + 1];
            if (a.loop->rpm <= rpm && rpm <= b.loop->rpm)
            {
                const double w = (rpm - a.loop->rpm) / (b.loop->rpm - a.loop->rpm);
                const double out = a.next(ratio(a)) * std::cos(w * Pi / 2.0) + b.next(ratio(b)) * std::sin(w * Pi / 2.0);
                for (size_t k = 0; k < voices.size(); ++k) // keep the others moving so they don't jump in later
                    if (k != i && k != i + 1)
                        voices[k].skip(ratio(voices[k]));
                return out;
            }
        }
        return 0.0;
    }

    void Mixer::render(float *mono, uint32_t frames)
    {
        const double vol = pack_->volume;
        for (uint32_t f = 0; f < frames; ++f)
        {
            rpmS_ += (rpmTarget_ - rpmS_) * rpmCoef_;
            thrS_ += (thrTarget_ - thrS_) * thrCoef_;
            const double eOn = blend(on_, rpmS_);
            const double eOff = off_.empty() ? eOn * 0.35 : blend(off_, rpmS_);
            double v = (eOn * thrS_ + eOff * (1.0 - thrS_)) * vol;
            for (const Pop &p : active_)
            {
                if (sample_ < p.start)
                    continue;
                // the pop at its own rate (nearest sample: pops are short bangs)
                const size_t i = static_cast<size_t>(double(sample_ - p.start) * p.loop->rate / rate_);
                if (i < p.loop->data.size())
                    v += p.loop->data[i] * p.gain;
            }
            lp_ += lpCoef_ * (v - lp_);
            v += lp_ * bassGain_;
            mono[f] = static_cast<float>(std::tanh(v * 1.2) * 0.85);
            ++sample_;
        }
        active_.erase(std::remove_if(active_.begin(), active_.end(),
                                     [&](const Pop &p) {
                                         return sample_ >= p.start &&
                                                static_cast<size_t>(double(sample_ - p.start) * p.loop->rate / rate_) >= p.loop->data.size();
                                     }),
                      active_.end());
        if (std::abs(lp_) < 1e-12)
            lp_ = 0.0;
    }
}
#endif
