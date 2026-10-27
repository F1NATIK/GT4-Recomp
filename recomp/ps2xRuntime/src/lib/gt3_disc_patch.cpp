#include "gt3_disc_patch.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace gt3discpatch
{
    namespace
    {
        struct Patch
        {
            uint64_t offset = 0u;
            std::vector<uint8_t> bytes;
        };

        std::vector<Patch> g_patches;
        std::once_flag g_once;
        bool g_enabled = false;

        void load()
        {
            const char *dir = std::getenv("GT3_DISC_PATCH");
            if (!dir || !*dir)
                return;
            const std::filesystem::path root(dir);
            std::ifstream list(root / "patches.txt");
            if (!list)
            {
                std::cerr << "[disc-patch] no patches.txt in " << root.string() << std::endl;
                return;
            }
            std::string line;
            while (std::getline(list, line))
            {
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream fields(line);
                std::string offset, name;
                fields >> offset >> name;
                if (offset.empty() || name.empty())
                    continue;
                Patch patch;
                try
                {
                    patch.offset = std::stoull(offset, nullptr, 16);
                }
                catch (...)
                {
                    std::cerr << "[disc-patch] bad offset: " << line << std::endl;
                    continue;
                }
                std::ifstream file(root / name, std::ios::binary);
                if (!file)
                {
                    std::cerr << "[disc-patch] missing " << (root / name).string() << std::endl;
                    continue;
                }
                patch.bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
                std::cerr << "[disc-patch] " << name << ": " << patch.bytes.size() << " bytes at image offset 0x"
                          << std::hex << patch.offset << std::dec << std::endl;
                g_patches.push_back(std::move(patch));
            }
            g_enabled = !g_patches.empty();
        }
    }

    bool enabled()
    {
        std::call_once(g_once, load);
        return g_enabled;
    }

    void overlay(uint64_t imageOffset, uint8_t *destination, size_t size)
    {
        if (!destination || size == 0u || !enabled())
            return;
        const uint64_t end = imageOffset + size;
        for (const Patch &patch : g_patches)
        {
            const uint64_t from = std::max(imageOffset, patch.offset);
            const uint64_t to = std::min(end, patch.offset + patch.bytes.size());
            if (from < to)
                std::memcpy(destination + (from - imageOffset), patch.bytes.data() + (from - patch.offset),
                            static_cast<size_t>(to - from));
        }
    }
}
