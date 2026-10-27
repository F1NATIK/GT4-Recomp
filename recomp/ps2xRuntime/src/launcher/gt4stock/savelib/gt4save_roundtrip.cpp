// gt4save_roundtrip.cpp - see gt4save_roundtrip.h (Full edition and tests only).

#include "gt4save_roundtrip.h"
#include "gt4save_crypto.h"

namespace gt4save::roundtrip
{
    using namespace crypto;

    std::vector<uint8_t> envelopeEncrypt(const std::vector<uint8_t> &payload, uint32_t seed, Rounding rounding)
    {
        const EnvelopeKind &kind = kGamedataEnvelope;
        const size_t size = payload.size() + 8;
        if (size & 63)
            return {}; // the payload must be padded so that len + 8 is a multiple of 64
        std::vector<uint8_t> buf(8, 0);
        buf.insert(buf.end(), payload.begin(), payload.end());
        const uint32_t w1 = crc32(buf.data() + 8, size - 8) ^ kind.key;
        wr32(buf.data(), seed);
        wr32(buf.data() + 4, w1);
        MT mt(w1 + seed);
        LCG17 lcg(w1 ^ seed);
        wordCipherEnc(buf.data() + 8, size - 8, mt, lcg);
        uint64_t t = 0;
        wordSwap(buf.data(), size, 4, size - 4, kind.fracA, rounding, t);
        if (kind.byteShuffle)
            byteShuffle(buf.data() + 4, uint32_t(size - 4), seed ^ kind.key, rounding, false);
        wordSwap(buf.data(), size, 0, size, kind.fracB, rounding, t);
        return buf;
    }

    std::vector<uint8_t> garageSlotEncrypt(const uint8_t *plain, uint32_t key, uint32_t slot, uint32_t seed, Rounding rounding)
    {
        std::vector<uint8_t> b(8, 0);
        b.insert(b.end(), plain, plain + (kSlotBytes - 8));
        const uint32_t crc = crc32(b.data() + 8, kSerialCarBytes);
        wr32(b.data(), seed);
        wr32(b.data() + 4, crc);
        MT mt(key ^ seed);
        LCG17 lcg(crc);
        wordCipherEnc(b.data() + 8, b.size() - 8, mt, lcg);
        bitShuffle(b.data(), b.size(), key + slot, rounding, false);
        return b;
    }

    bool rebuild(const Save &s, std::map<std::string, std::vector<uint8_t>> &files, std::string &error)
    {
        files.clear();
        if (!s.loaded)
        {
            error = "the save was not loaded: " + s.error;
            return false;
        }
        if (!s.gamedata.outerCrcOk)
        {
            error = "GAMEDATA did not decrypt with a valid checksum; not rebuilding it";
            return false;
        }
        const Rounding r = s.rounding;
        std::vector<uint8_t> gd = envelopeEncrypt(s.dec.gamedataPlain, s.gamedata.seed, r);
        if (gd.empty())
        {
            error = "GAMEDATA plaintext has an unexpected size";
            return false;
        }
        if (s.gamedata.copies > 1)
            gd.insert(gd.begin(), s.dec.gamedataPrefix.begin(), s.dec.gamedataPrefix.end());
        files[kGamedataFile] = std::move(gd);
        if (s.garage && s.dec.hasGarage)
        {
            const GarageInfo &g = *s.garage;
            const uint32_t key = rd32(s.dec.gamedataPlain.data() + 0x135E4);
            std::vector<uint8_t> buf = garageNoise(key, kGarageBytes);
            const std::array<uint8_t, 64> hdr = garageHeaderEncrypt(s.dec.garageHeader.data(), r);
            std::copy(hdr.begin(), hdr.end(), buf.begin());
            for (const SlotState &st : g.slots)
            {
                const size_t a = 64 + size_t(st.slot) * kSlotBytes;
                if (st.kind == SlotKind::Car)
                {
                    const auto &[seed, plain] = s.dec.slotPlain.at(st.slot);
                    const std::vector<uint8_t> enc = garageSlotEncrypt(plain.data(), key, uint32_t(st.slot), seed, r);
                    std::copy(enc.begin(), enc.end(), buf.begin() + std::ptrdiff_t(a));
                }
                else if (st.kind == SlotKind::Raw)
                {
                    const std::vector<uint8_t> &raw = s.dec.slotRaw.at(st.slot);
                    std::copy(raw.begin(), raw.end(), buf.begin() + std::ptrdiff_t(a));
                }
            }
            size_t ih = 0, ic = 0, ir = 0;
            for (const TrailingWrite &t : g.trailing)
            {
                if (t.kind == TrailingKind::Header)
                {
                    const std::array<uint8_t, 64> th = garageHeaderEncrypt(s.dec.trailingHeaders.at(ih++).data(), r);
                    buf.insert(buf.end(), th.begin(), th.end());
                }
                else if (t.kind == TrailingKind::Car)
                {
                    const std::vector<uint8_t> enc = garageSlotEncrypt(s.dec.trailingCars.at(ic++).data(), key, uint32_t(t.slot), t.seed, r);
                    buf.insert(buf.end(), enc.begin(), enc.end());
                }
                else
                {
                    const std::vector<uint8_t> &raw = s.dec.trailingRaw.at(ir++);
                    buf.insert(buf.end(), raw.begin(), raw.end());
                }
            }
            if (g.size < kGarageBytes)
                buf.resize(size_t(g.size));
            files[kGarageFile] = std::move(buf);
        }
        for (const auto &[name, data] : s.dec.otherFiles)
            files[name] = data;
        return true;
    }
}
