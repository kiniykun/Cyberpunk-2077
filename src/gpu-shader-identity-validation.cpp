#include "gpu-shader-web.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <cstdio>
using namespace cp2077::web;
int main() {
    const char* names[] = {"m_prepareHudTiles", "m_dilateHudTiles_Horizontal",
                           "m_dilateHudTiles_Vertical", "m_hud"};
    for (const auto* name : names) {
        const auto* shader = FindConvertedShader(name);
        assert(shader && std::strcmp(shader->runtimeName, name) == 0);
        assert(FindConvertedShaderBytecode(shader->bytecodeIdentity, shader->bytecodeIdentityBytes) == shader);
        assert(!FindConvertedShaderBytecode(shader->bytecodeIdentity, shader->bytecodeIdentityBytes - 1));
        std::vector<char> changed(shader->bytecodeIdentity, shader->bytecodeIdentity + shader->bytecodeIdentityBytes);
        changed[8] ^= 1; // stale shader hash must never select the current program
        assert(!FindConvertedShaderBytecode(changed.data(), changed.size()));
        changed[8] ^= 1;
        changed.back() ^= 1; // runtime variant is part of the identity
        assert(!FindConvertedShaderBytecode(changed.data(), changed.size()));
        changed.back() ^= 1;
        changed.push_back(0); // trailing data cannot be silently ignored
        assert(!FindConvertedShaderBytecode(changed.data(), changed.size()));
    }
    const auto* horizontal = FindConvertedShader(names[1]);
    const auto* vertical = FindConvertedShader(names[2]);
    assert(std::strcmp(horizontal->wgslSHA256, vertical->wgslSHA256) != 0);
    const char legacy[] = "DXBCnot-a-converted-shader";
    assert(!FindConvertedShaderBytecode(legacy, sizeof(legacy)));
    assert(!FindConvertedShaderBytecode(nullptr, 100));
    assert(!FindConvertedShader("unknown"));
    std::puts("Converted shader identities: four exact variants accepted; stale, truncated, altered and legacy records rejected");
}
