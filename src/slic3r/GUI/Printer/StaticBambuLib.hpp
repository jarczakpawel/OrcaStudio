#pragma once

#include "BambuTunnel.h"

#include <vector>

struct StaticBambuLib : BambuLib
{
    static StaticBambuLib& get(BambuLib* copy = nullptr);
    static int Fake_Bambu_Create(Bambu_Tunnel*, const char*) { return -2; }
    static void reset();
    static void release();
    static void remove(BambuLib* copy);

private:
    static StaticBambuLib& storage();
    void add_copy(BambuLib* copy);

    std::vector<BambuLib*> copies_;
};
