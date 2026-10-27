#include "runtime/ps2_vu1_translated.h"

namespace ps2vu1t
{
    extern const TranslatedImage kTranslatedImages[];
    extern const uint32_t kTranslatedImageCount;

    void commitSlow(Ctx &c, uint64_t upTo)
    {
        // The interpreter commits every cycle: the flag entries due that cycle
        // (issue order), then FDIV, then the EFU slots. Replay the due cycles in
        // order.
        if (!c.qPending && !c.efuValid[0] && !c.efuValid[1])
        {
            // Only flag entries (ready cycles in issue order): apply the due ones.
            VU1State &s = *c.s;
            while (c.flagHead != c.flagTail)
            {
                const FlagEntry &e = c.flags[c.flagHead % Ctx::kFlagRing];
                if (e.ready > upTo)
                {
                    c.nextCommit = e.ready;
                    return;
                }
                if (e.writes & kWriteMac)
                    s.mac = e.mac;
                if (e.writes & kWriteStatus)
                {
                    const uint32_t current = e.status & 0xFu;
                    s.status = (s.status & 0xFF0u) | current | ((current | e.extraSticky) << 6);
                }
                if (e.writes & kWriteSticky)
                    s.status = (s.status & 0x03Fu) | (e.status & 0xFC0u);
                if (e.writes & kWriteClip)
                    s.clip = e.clip;
                ++c.flagHead;
            }
            c.nextCommit = std::numeric_limits<uint64_t>::max();
            return;
        }
        for (;;)
        {
            VU1State &s = *c.s;
            // Flag results due before the next Q / P result (and by upTo) go in
            // one pass: in issue order, and before a Q / P due the same cycle.
            uint64_t other = std::numeric_limits<uint64_t>::max();
            if (c.qPending)
                other = c.qReady;
            for (uint32_t slot = 0; slot < 2u; ++slot)
                if (c.efuValid[slot])
                    other = std::min(other, c.efuReady[slot]);
            const uint64_t flagsBy = std::min(other, upTo);
            while (c.flagHead != c.flagTail && c.flags[c.flagHead % Ctx::kFlagRing].ready <= flagsBy)
            {
                const FlagEntry &e = c.flags[c.flagHead++ % Ctx::kFlagRing];
                if (e.writes & kWriteMac)
                    s.mac = e.mac;
                if (e.writes & kWriteStatus)
                {
                    const uint32_t current = e.status & 0xFu;
                    s.status = (s.status & 0xFF0u) | current | ((current | e.extraSticky) << 6);
                }
                if (e.writes & kWriteSticky)
                    s.status = (s.status & 0x03Fu) | (e.status & 0xFC0u);
                if (e.writes & kWriteClip)
                    s.clip = e.clip;
            }
            uint64_t t = other;
            if (c.flagHead != c.flagTail)
                t = std::min(t, c.flags[c.flagHead % Ctx::kFlagRing].ready);
            if (t == std::numeric_limits<uint64_t>::max() || t > upTo)
            {
                c.nextCommit = t;
                return;
            }
            while (c.flagHead != c.flagTail && c.flags[c.flagHead % Ctx::kFlagRing].ready <= t)
            {
                const FlagEntry &e = c.flags[c.flagHead++ % Ctx::kFlagRing];
                if (e.writes & kWriteMac)
                    s.mac = e.mac;
                if (e.writes & kWriteStatus)
                {
                    const uint32_t current = e.status & 0xFu;
                    s.status = (s.status & 0xFF0u) | current | ((current | e.extraSticky) << 6);
                }
                if (e.writes & kWriteSticky)
                    s.status = (s.status & 0x03Fu) | (e.status & 0xFC0u);
                if (e.writes & kWriteClip)
                    s.clip = e.clip;
            }
            if (c.qPending && c.qReady <= t)
            {
                s.q = c.qValue;
                const uint32_t currentDi = c.qDi & 0x30u;
                s.status = (s.status & 0xFCFu) | currentDi | (currentDi << 6);
                c.qPending = false;
            }
            for (uint32_t slot = 0; slot < 2u; ++slot)
            {
                if (c.efuValid[slot] && c.efuReady[slot] <= t)
                {
                    s.p = c.efuValue[slot];
                    c.efuValid[slot] = false;
                }
            }
        }
    }

    uint32_t prodFlagsSlowV(__m128 p, __m128 s, __m128 t, int lanes)
    {
        const __m128i zero = _mm_and_si128(zeroBitsV(p), _mm_or_si128(zeroBitsV(s), zeroBitsV(t)));
        const __m128i ok = _mm_or_si128(normalExpV(p), zero);
        if (((~_mm_movemask_ps(_mm_castsi128_ps(ok))) & lanes) == 0)
            return ((_mm_movemask_ps(p) & lanes) ? 2u : 0u) | ((_mm_movemask_ps(_mm_castsi128_ps(zero)) & lanes) ? 1u : 0u);
        alignas(16) float pa[4], sa[4], ta[4];
        _mm_store_ps(pa, p);
        _mm_store_ps(sa, s);
        _mm_store_ps(ta, t);
        uint32_t flags = 0u;
        for (int k = 0; k < 4; ++k)
            if (lanes & (1 << k))
                flags |= prodFlags(pa[k], sa[k], ta[k]);
        return flags;
    }

    void pushFsset(Ctx &c, uint32_t immediate)
    {
        commit(c);
        for (uint32_t k = c.flagHead; k != c.flagTail; ++k)
        {
            FlagEntry &e = c.flags[k % Ctx::kFlagRing];
            if (e.issue == c.cycle)
                e.writes &= static_cast<uint8_t>(~kWriteStatus);
        }
        FlagEntry &e = c.flags[c.flagTail++ % Ctx::kFlagRing];
        e = {};
        e.issue = c.cycle;
        e.ready = c.cycle + 4u;
        e.status = immediate & 0xFC0u;
        e.writes = kWriteSticky;
        c.nextCommit = std::min(c.nextCommit, e.ready);
    }

    void pushFcset(Ctx &c, uint32_t clip)
    {
        commit(c);
        c.workingClip = clip & 0xFFFFFFu;
        for (uint32_t k = c.flagHead; k != c.flagTail; ++k)
        {
            FlagEntry &e = c.flags[k % Ctx::kFlagRing];
            if (e.issue == c.cycle)
                e.writes &= static_cast<uint8_t>(~kWriteClip);
        }
        FlagEntry &e = c.flags[c.flagTail++ % Ctx::kFlagRing];
        e = {};
        e.issue = c.cycle;
        e.ready = c.cycle + 4u;
        e.clip = c.workingClip;
        e.writes = kWriteClip;
        c.nextCommit = std::min(c.nextCommit, e.ready);
    }

    namespace
    {
        // One PATH1 qword (VU1Interpreter::progressXgkick). Returns true when the
        // packet is complete (and submitted).
        bool kickStep(Ctx &c)
        {
            Kick &k = c.kick;
            if (k.copied > Kick::kBufferSize - 16u)
            {
                ++c.errors;
                k.active = false;
                return true;
            }
            const uint32_t qwordOffset = k.copied;
            const uint32_t start = (k.source + k.copied) % kDataSize;
            std::memcpy(k.packet + k.copied, c.mem + start, 16u); // start is qword aligned
            k.copied += 16u;
            if (k.tagEnd == 0u)
            {
                uint64_t tagLo = 0;
                std::memcpy(&tagLo, k.packet + qwordOffset, sizeof(tagLo));
                const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
                const uint32_t format = static_cast<uint32_t>((tagLo >> 58) & 0x3u);
                uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
                if (nreg == 0u)
                    nreg = 16u;
                uint64_t tagBytes = 16u;
                if (format == 0u)
                    tagBytes += static_cast<uint64_t>(nloop) * nreg * 16u;
                else if (format == 1u)
                    tagBytes += ((static_cast<uint64_t>(nloop) * nreg + 1u) & ~1ull) * 8u;
                else if (format == 2u)
                    tagBytes += static_cast<uint64_t>(nloop) * 16u;
                else
                {
                    ++c.errors;
                    k.active = false;
                    return true;
                }
                if (tagBytes > Kick::kBufferSize - qwordOffset)
                {
                    ++c.errors;
                    k.active = false;
                    return true;
                }
                k.tagEnd = qwordOffset + static_cast<uint32_t>(tagBytes);
                k.tagEop = ((tagLo >> 15) & 1u) != 0u;
                if (k.tagEop)
                    k.total = k.tagEnd;
            }
            if (k.copied >= k.tagEnd)
            {
                if (k.tagEop)
                {
                    k.active = false;
                    if (c.submit)
                        c.submit(c.user, k.packet, k.total);
                    return true;
                }
                k.tagEnd = 0u;
                k.tagEop = false;
            }
            return false;
        }
    }

    void kickCatchUp(Ctx &c, uint64_t upTo)
    {
        Kick &k = c.kick;
        while (k.active && k.nextCopy <= upTo)
        {
            // Inside a tag's data, the qwords due by upTo except the tag's last
            // are copied at once: memory cannot change during a catch-up, so
            // this is what one kickStep per qword copies (the step per qword
            // was ~3% of the VIF1/VU1 thread). The last qword of a tag goes
            // through kickStep (end of tag, EOP submit, finishing cycle).
            if (k.tagEnd != 0u && k.copied < k.tagEnd)
            {
                const uint64_t due = (upTo - k.nextCopy) / 2u + 1u;
                const uint32_t left = (k.tagEnd - k.copied) / 16u;
                const uint32_t bulk = static_cast<uint32_t>(std::min<uint64_t>(due, left)) - (due >= left ? 1u : 0u);
                for (uint32_t done = 0; done < bulk;)
                {
                    const uint32_t start = (k.source + k.copied) % kDataSize;
                    const uint32_t run = std::min(bulk - done, (kDataSize - start) / 16u);
                    std::memcpy(k.packet + k.copied, c.mem + start, static_cast<size_t>(run) * 16u);
                    k.copied += run * 16u;
                    k.nextCopy += 2ull * run;
                    done += run;
                }
                if (k.nextCopy > upTo)
                    return;
            }
            const uint64_t at = k.nextCopy;
            k.nextCopy += 2u;
            if (kickStep(c))
            {
                k.nextCopy = at; // finishing cycle
                return;
            }
        }
    }

    uint64_t kickFinish(Ctx &c)
    {
        if (!c.kick.active)
            return c.kick.nextCopy;
        kickCatchUp(c, std::numeric_limits<uint64_t>::max());
        return c.kick.nextCopy;
    }

    void finish(Ctx &c)
    {
        uint64_t end = c.cycle;
        if (c.kick.active)
            end = std::max(end, kickFinish(c));
        end = std::max(end, c.lastReady);
        for (uint32_t k = c.flagHead; k != c.flagTail; ++k)
            end = std::max(end, c.flags[k % Ctx::kFlagRing].ready);
        if (c.qPending)
            end = std::max(end, c.qReady);
        for (uint32_t slot = 0; slot < 2u; ++slot)
            if (c.efuValid[slot])
                end = std::max(end, c.efuReady[slot]);
        commitSlow(c, std::numeric_limits<uint64_t>::max());
        c.s->status |= (c.stickyAcc & 0xFu) << 6;
        c.stickyAcc = 0;
        c.cycle = end;
    }

    uint64_t hashCode(const uint8_t *code, uint32_t size)
    {
        uint64_t hash = 1469598103934665603ull;
        for (uint32_t i = 0; i < size; ++i)
            hash = (hash ^ code[i]) * 1099511628211ull;
        return hash;
    }

    const TranslatedImage *findImage(uint64_t hash)
    {
        for (uint32_t i = 0; i < kTranslatedImageCount; ++i)
            if (kTranslatedImages[i].hash == hash)
                return &kTranslatedImages[i];
        return nullptr;
    }

    // VU0 microprogram images (4 KiB code), generated with vu1_translate.py --vu0.
    extern const TranslatedImage kTranslatedVu0Images[];
    extern const uint32_t kTranslatedVu0ImageCount;
    const TranslatedImage *findVu0Image(uint64_t hash)
    {
        for (uint32_t i = 0; i < kTranslatedVu0ImageCount; ++i)
            if (kTranslatedVu0Images[i].hash == hash)
                return &kTranslatedVu0Images[i];
        return nullptr;
    }
}
