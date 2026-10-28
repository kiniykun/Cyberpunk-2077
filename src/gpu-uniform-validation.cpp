#include "gpu-uniform-web.h"
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cstdint>
using namespace cp2077::web;
int main() {
    // Distinct bit patterns make row/column mistakes visible. Include NaN bits
    // to verify repacking never performs floating-point conversion.
    std::uint32_t source[128];
    for (unsigned i = 0; i < 128; ++i) source[i] = 0x7fc00000u + i;
    std::uint32_t target[128];
    const auto* hud = FindUniformLayout("hud", 0, 6);
    assert(hud && hud->webBytes == 336 && hud->nativeMinimumBytes == 316);
    assert(FindUniformLayout("m_hud", 0, 6) == hud);
    std::memset(target, 0xa5, sizeof(target));
    assert(RepackUniform(*hud, source, sizeof(source), target, sizeof(target)));
    for (unsigned column = 0; column < 3; ++column)
        for (unsigned row = 0; row < 4; ++row)
            assert(target[(272 + column*16 + row*4)/4] == source[(256 + row*16 + column*4)/4]);
    for (unsigned byte = 320; byte < 336; ++byte)
        assert(reinterpret_cast<unsigned char*>(target)[byte] == 0);
    assert(target[336/4] == 0xa5a5a5a5u); // caller's trailing capacity untouched
    const auto* tiles = FindUniformLayout("prepareHudTiles", 0, 6);
    assert(tiles && tiles->webBytes == 192);
    assert(RepackUniform(*tiles, source, sizeof(source), target, sizeof(target)));
    assert(target[152/4] == source[148/4]);
    assert(target[176/4] == source[160/4]);
    assert(target[180/4] == source[164/4]);
    const auto* globals = FindUniformLayout("hud", 0, 0);
    assert(globals && globals->webBytes == 464);
    assert(RepackUniform(*globals, source, sizeof(source), target, sizeof(target)));
    assert(target[32/4] == source[32/4]);
    std::memset(target, 0xa5, sizeof(target));
    assert(!RepackUniform(*hud, source, 315, target, sizeof(target)));
    assert(!RepackUniform(*hud, source, sizeof(source), target, 335));
    for (auto word : target) assert(word == 0xa5a5a5a5u);
    assert(!RepackUniform(*hud, source, sizeof(source), source + 1, sizeof(source)-4));
    assert(source[1] == 0x7fc00001u);
    const UniformScalarCopy invalidField{0, 335, 4};
    auto invalid = *hud;
    invalid.fields = &invalidField;
    invalid.fieldCount = 1;
    assert(!RepackUniform(invalid, source, sizeof(source), target, sizeof(target)));
    for (auto word : target) assert(word == 0xa5a5a5a5u);
    assert(!FindUniformLayout("dilateHudTiles", 0, 6));
    assert(!FindUniformLayout(nullptr, 0, 6));
    // Original UI vertex DXIL has a fully occupied 848-byte camera block and
    // a 112-byte frequent matrix block. Verify exact bits and source bounds
    // using its real cache identity, independently of generated copy entries.
    std::uint32_t graphicsSource[256], graphicsTarget[256];
    for (unsigned i = 0; i < 256; ++i) graphicsSource[i] = 0xff800001u + i;
    std::memset(graphicsTarget, 0xa5, sizeof(graphicsTarget));
    const auto* camera = FindUniformLayout("1022250503081802825", 0, 1);
    assert(camera && camera->nativeMinimumBytes == 848 && camera->webBytes == 848);
    assert(RepackUniform(*camera, graphicsSource, sizeof(graphicsSource), graphicsTarget, sizeof(graphicsTarget)));
    assert(std::memcmp(graphicsSource, graphicsTarget, 848) == 0);
    assert(graphicsTarget[212] == 0xa5a5a5a5u);
    const auto* frequent = FindUniformLayout("1022250503081802825", 0, 5);
    assert(frequent && frequent->nativeMinimumBytes == 112 && frequent->webBytes == 112);
    assert(RepackUniform(*frequent, graphicsSource, 112, graphicsTarget, 112));
    assert(std::memcmp(graphicsSource, graphicsTarget, 112) == 0);
    assert(!RepackUniform(*camera, graphicsSource, 847, graphicsTarget, sizeof(graphicsTarget)));
    assert(!FindUniformLayout("1022250503081802825", 0, 6));
    std::puts("Original UI uniform layouts: matrix orientation, padding, bit preservation and bounds passed");
}
