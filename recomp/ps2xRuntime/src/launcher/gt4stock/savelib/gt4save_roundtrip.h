#pragma once
// gt4save_roundtrip.h - rebuild a save's encrypted files from what gt4save::load decrypted (port of
// gt4save.py do_encrypt and gt4save_crypto.py env_encrypt / garage_slot_encrypt; from
// analysis/launcher-v3/savelib/src, byte-identical on every test save there). GT4's own random inputs
// (the envelope seed, each slot's seed) are taken from the loaded save, so the rebuilt files equal the
// originals byte for byte.
//
// Linked by the Full edition only (its save-changing Garage actions, gt4full_saveedit.cpp, through
// gt4save_edit.h) and by the tests. The Classic edition never links it: its save view is read-only.

#include "gt4save.h"

#include <map>
#include <string>
#include <vector>

namespace gt4save::roundtrip
{
    // 0x1d4d10 for GAMEDATA. payload = padded plaintext (file size - 8 bytes); seed = header word 0.
    std::vector<uint8_t> envelopeEncrypt(const std::vector<uint8_t> &payload, uint32_t seed, Rounding rounding);
    // 0x1ceaa8. plain = 1,272 bytes (1,216 serialised car bytes + zero padding); seed = slot word 0.
    std::vector<uint8_t> garageSlotEncrypt(const uint8_t *plain1272, uint32_t key, uint32_t slot, uint32_t seed, Rounding rounding);

    // The files of the save (BASCUS-97328GAMEDATA, garage, icon.sys, icon0.ico) rebuilt from save.dec.
    // false (with error) when GT4 would not have written such a file (the GAMEDATA CRC fails).
    bool rebuild(const Save &save, std::map<std::string, std::vector<uint8_t>> &files, std::string &error);
}
