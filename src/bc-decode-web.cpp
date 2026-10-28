// bc-decode-web.cpp - CPU BC1-BC7 block decoder (see bc-decode-web.h).
// BC1-BC5 are written from the D3D format spec; BC6H/BC7 decode paths and tables are ported from
// Microsoft DirectXTex (BC6HBC7.cpp: D3DX_BC6H::Decode, D3DX_BC7::Decode) with DirectXMath types
// replaced by plain ints. Original notice:
//
//   Copyright (c) Microsoft Corporation. All rights reserved.
//   Licensed under the MIT License.
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
//   associated documentation files (the "Software"), to deal in the Software without restriction,
//   including without limitation the rights to use, copy, modify, merge, publish, distribute,
//   sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions: The above copyright notice and this
//   permission notice shall be included in all copies or substantial portions of the Software.
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
//   NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
//   NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
//   DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
//   OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//   http://go.microsoft.com/fwlink/?LinkId=248926
#include "bc-decode-web.h"
#include <cstring>

namespace cp2077 { namespace bc {
namespace {

// ---- DirectXTex tables (all const; 1-region rows of the partition/fixup tables are all zero) ----
// Partition, Shape, Pixel (index into 4x4 block)
const uint8_t g_aPartitionTable[3][64][16] =
{
    {}, // 1 region: all 0
    {   // BC6H/BC7 Partition Set for 2 Subsets
        { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 }, // Shape 0
        { 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1 }, // Shape 1
        { 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1 }, // Shape 2
        { 0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 1 }, // Shape 3
        { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 1 }, // Shape 4
        { 0, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 }, // Shape 5
        { 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1 }, // Shape 6
        { 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1 }, // Shape 7
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1 }, // Shape 8
        { 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, // Shape 9
        { 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1 }, // Shape 10
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1 }, // Shape 11
        { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, // Shape 12
        { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1 }, // Shape 13
        { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 }, // Shape 14
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 }, // Shape 15
        { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1 }, // Shape 16
        { 0, 1, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 }, // Shape 17
        { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0 }, // Shape 18
        { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0 }, // Shape 19
        { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 }, // Shape 20
        { 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0 }, // Shape 21
        { 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 }, // Shape 22
        { 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 1 }, // Shape 23
        { 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0 }, // Shape 24
        { 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0 }, // Shape 25
        { 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0 }, // Shape 26
        { 0, 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0 }, // Shape 27
        { 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0 }, // Shape 28
        { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 }, // Shape 29
        { 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0 }, // Shape 30
        { 0, 0, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0 }, // Shape 31

                                                            // BC7 Partition Set for 2 Subsets (second-half)
        { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 }, // Shape 32
        { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1 }, // Shape 33
        { 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0 }, // Shape 34
        { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0 }, // Shape 35
        { 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0 }, // Shape 36
        { 0, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0 }, // Shape 37
        { 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1 }, // Shape 38
        { 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1 }, // Shape 39
        { 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0 }, // Shape 40
        { 0, 0, 0, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 0, 0, 0 }, // Shape 41
        { 0, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0 }, // Shape 42
        { 0, 0, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0 }, // Shape 43
        { 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0 }, // Shape 44
        { 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1 }, // Shape 45
        { 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1 }, // Shape 46
        { 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0 }, // Shape 47
        { 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0 }, // Shape 48
        { 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0 }, // Shape 49
        { 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0 }, // Shape 50
        { 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0 }, // Shape 51
        { 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1 }, // Shape 52
        { 0, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1 }, // Shape 53
        { 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0 }, // Shape 54
        { 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0 }, // Shape 55
        { 0, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1 }, // Shape 56
        { 0, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 1 }, // Shape 57
        { 0, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 1 }, // Shape 58
        { 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1 }, // Shape 59
        { 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1 }, // Shape 60
        { 0, 0, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0 }, // Shape 61
        { 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0 }, // Shape 62
        { 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1 }  // Shape 63
    },

    {   // BC7 Partition Set for 3 Subsets
        { 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 2 }, // Shape 0
        { 0, 0, 0, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 2, 1 }, // Shape 1
        { 0, 0, 0, 0, 2, 0, 0, 1, 2, 2, 1, 1, 2, 2, 1, 1 }, // Shape 2
        { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 1 }, // Shape 3
        { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2 }, // Shape 4
        { 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 2 }, // Shape 5
        { 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1 }, // Shape 6
        { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1 }, // Shape 7
        { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2 }, // Shape 8
        { 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2 }, // Shape 9
        { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2 }, // Shape 10
        { 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2 }, // Shape 11
        { 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2, 0, 1, 1, 2 }, // Shape 12
        { 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2, 0, 1, 2, 2 }, // Shape 13
        { 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 2 }, // Shape 14
        { 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0, 2, 2, 2, 0 }, // Shape 15
        { 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 2 }, // Shape 16
        { 0, 1, 1, 1, 0, 0, 1, 1, 2, 0, 0, 1, 2, 2, 0, 0 }, // Shape 17
        { 0, 0, 0, 0, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2 }, // Shape 18
        { 0, 0, 2, 2, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 1 }, // Shape 19
        { 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 2 }, // Shape 20
        { 0, 0, 0, 1, 0, 0, 0, 1, 2, 2, 2, 1, 2, 2, 2, 1 }, // Shape 21
        { 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2 }, // Shape 22
        { 0, 0, 0, 0, 1, 1, 0, 0, 2, 2, 1, 0, 2, 2, 1, 0 }, // Shape 23
        { 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0 }, // Shape 24
        { 0, 0, 1, 2, 0, 0, 1, 2, 1, 1, 2, 2, 2, 2, 2, 2 }, // Shape 25
        { 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1, 0, 1, 1, 0 }, // Shape 26
        { 0, 0, 0, 0, 0, 1, 1, 0, 1, 2, 2, 1, 1, 2, 2, 1 }, // Shape 27
        { 0, 0, 2, 2, 1, 1, 0, 2, 1, 1, 0, 2, 0, 0, 2, 2 }, // Shape 28
        { 0, 1, 1, 0, 0, 1, 1, 0, 2, 0, 0, 2, 2, 2, 2, 2 }, // Shape 29
        { 0, 0, 1, 1, 0, 1, 2, 2, 0, 1, 2, 2, 0, 0, 1, 1 }, // Shape 30
        { 0, 0, 0, 0, 2, 0, 0, 0, 2, 2, 1, 1, 2, 2, 2, 1 }, // Shape 31
        { 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 2, 2, 2 }, // Shape 32
        { 0, 2, 2, 2, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 1 }, // Shape 33
        { 0, 0, 1, 1, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 2 }, // Shape 34
        { 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0, 0, 1, 2, 0 }, // Shape 35
        { 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0 }, // Shape 36
        { 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 }, // Shape 37
        { 0, 1, 2, 0, 2, 0, 1, 2, 1, 2, 0, 1, 0, 1, 2, 0 }, // Shape 38
        { 0, 0, 1, 1, 2, 2, 0, 0, 1, 1, 2, 2, 0, 0, 1, 1 }, // Shape 39
        { 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 0, 0, 0, 0, 1, 1 }, // Shape 40
        { 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2 }, // Shape 41
        { 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1 }, // Shape 42
        { 0, 0, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2, 1, 1, 2, 2 }, // Shape 43
        { 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 1 }, // Shape 44
        { 0, 2, 2, 0, 1, 2, 2, 1, 0, 2, 2, 0, 1, 2, 2, 1 }, // Shape 45
        { 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 0, 1, 0, 1 }, // Shape 46
        { 0, 0, 0, 0, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1, 2, 1 }, // Shape 47
        { 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 2 }, // Shape 48
        { 0, 2, 2, 2, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 1 }, // Shape 49
        { 0, 0, 0, 2, 1, 1, 1, 2, 0, 0, 0, 2, 1, 1, 1, 2 }, // Shape 50
        { 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 2 }, // Shape 51
        { 0, 2, 2, 2, 0, 1, 1, 1, 0, 1, 1, 1, 0, 2, 2, 2 }, // Shape 52
        { 0, 0, 0, 2, 1, 1, 1, 2, 1, 1, 1, 2, 0, 0, 0, 2 }, // Shape 53
        { 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2 }, // Shape 54
        { 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2, 2, 1, 1, 2 }, // Shape 55
        { 0, 1, 1, 0, 0, 1, 1, 0, 2, 2, 2, 2, 2, 2, 2, 2 }, // Shape 56
        { 0, 0, 2, 2, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 2, 2 }, // Shape 57
        { 0, 0, 2, 2, 1, 1, 2, 2, 1, 1, 2, 2, 0, 0, 2, 2 }, // Shape 58
        { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 1, 1, 2 }, // Shape 59
        { 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 1 }, // Shape 60
        { 0, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 1, 2, 2, 2 }, // Shape 61
        { 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 }, // Shape 62
        { 0, 1, 1, 1, 2, 0, 1, 1, 2, 2, 0, 1, 2, 2, 2, 0 }  // Shape 63
    }
};

// Partition, Shape, Fixup
const uint8_t g_aFixUp[3][64][3] =
{
    {}, // 1 region: only index 0
    {   // BC6H/BC7 Partition Set Fixups for 2 Subsets
        { 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },
        { 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },
        { 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },
        { 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },
        { 0,15, 0 },{ 0, 2, 0 },{ 0, 8, 0 },{ 0, 2, 0 },
        { 0, 2, 0 },{ 0, 8, 0 },{ 0, 8, 0 },{ 0,15, 0 },
        { 0, 2, 0 },{ 0, 8, 0 },{ 0, 2, 0 },{ 0, 2, 0 },
        { 0, 8, 0 },{ 0, 8, 0 },{ 0, 2, 0 },{ 0, 2, 0 },

        // BC7 Partition Set Fixups for 2 Subsets (second-half)
        { 0,15, 0 },{ 0,15, 0 },{ 0, 6, 0 },{ 0, 8, 0 },
        { 0, 2, 0 },{ 0, 8, 0 },{ 0,15, 0 },{ 0,15, 0 },
        { 0, 2, 0 },{ 0, 8, 0 },{ 0, 2, 0 },{ 0, 2, 0 },
        { 0, 2, 0 },{ 0,15, 0 },{ 0,15, 0 },{ 0, 6, 0 },
        { 0, 6, 0 },{ 0, 2, 0 },{ 0, 6, 0 },{ 0, 8, 0 },
        { 0,15, 0 },{ 0,15, 0 },{ 0, 2, 0 },{ 0, 2, 0 },
        { 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },{ 0,15, 0 },
        { 0,15, 0 },{ 0, 2, 0 },{ 0, 2, 0 },{ 0,15, 0 }
    },

    {   // BC7 Partition Set Fixups for 3 Subsets
        { 0, 3,15 },{ 0, 3, 8 },{ 0,15, 8 },{ 0,15, 3 },
        { 0, 8,15 },{ 0, 3,15 },{ 0,15, 3 },{ 0,15, 8 },
        { 0, 8,15 },{ 0, 8,15 },{ 0, 6,15 },{ 0, 6,15 },
        { 0, 6,15 },{ 0, 5,15 },{ 0, 3,15 },{ 0, 3, 8 },
        { 0, 3,15 },{ 0, 3, 8 },{ 0, 8,15 },{ 0,15, 3 },
        { 0, 3,15 },{ 0, 3, 8 },{ 0, 6,15 },{ 0,10, 8 },
        { 0, 5, 3 },{ 0, 8,15 },{ 0, 8, 6 },{ 0, 6,10 },
        { 0, 8,15 },{ 0, 5,15 },{ 0,15,10 },{ 0,15, 8 },
        { 0, 8,15 },{ 0,15, 3 },{ 0, 3,15 },{ 0, 5,10 },
        { 0, 6,10 },{ 0,10, 8 },{ 0, 8, 9 },{ 0,15,10 },
        { 0,15, 6 },{ 0, 3,15 },{ 0,15, 8 },{ 0, 5,15 },
        { 0,15, 3 },{ 0,15, 6 },{ 0,15, 6 },{ 0,15, 8 },
        { 0, 3,15 },{ 0,15, 3 },{ 0, 5,15 },{ 0, 5,15 },
        { 0, 5,15 },{ 0, 8,15 },{ 0, 5,15 },{ 0,10,15 },
        { 0, 5,15 },{ 0,10,15 },{ 0, 8,15 },{ 0,13,15 },
        { 0,15, 3 },{ 0,12,15 },{ 0, 3,15 },{ 0, 3, 8 }
    }
};

const int g_aWeights2[] = { 0, 21, 43, 64 };
const int g_aWeights3[] = { 0, 9, 18, 27, 37, 46, 55, 64 };
const int g_aWeights4[] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };
const int* const g_aWeights[5] = { nullptr, nullptr, g_aWeights2, g_aWeights3, g_aWeights4 };

inline bool IsFixUpOffset(unsigned uPartitions, unsigned uShape, unsigned uOffset) {
    for (unsigned p = 0; p <= uPartitions; p++) if (uOffset == g_aFixUp[uPartitions][uShape][p]) return true;
    return false;
}

// LSB-first 128-bit reader (CBits::GetBits equivalent; n <= 32).
struct Bits {
    uint64_t lo = 0, hi = 0; unsigned pos = 0;
    explicit Bits(const uint8_t* b) { for (int i = 7; i >= 0; --i) { lo = lo << 8 | b[i]; hi = hi << 8 | b[i + 8]; } }
    unsigned Get(unsigned n) {
        if (!n) return 0;
        const uint64_t v = pos >= 128 ? 0 : pos >= 64 ? hi >> (pos - 64) : (lo >> pos) | (pos ? hi << (64 - pos) : 0);
        pos += n;
        return unsigned(v & ((1ull << n) - 1));
    }
};

inline void Put16(uint8_t* o, unsigned v) { o[0] = uint8_t(v); o[1] = uint8_t(v >> 8); }

// ---------------------------------------------------------------- BC1-BC5
// Color block -> RGBA8 x16. allow3: BC1 3-color + transparent-black mode when c0 <= c1.
void DecodeColor(const uint8_t* b, uint8_t* out, bool allow3) {
    const unsigned c0 = b[0] | unsigned(b[1]) << 8, c1 = b[2] | unsigned(b[3]) << 8;
    uint8_t p[4][4];
    for (int e = 0; e < 2; ++e) {
        const unsigned c = e ? c1 : c0, r = c >> 11, g = (c >> 5) & 63, bl = c & 31;
        p[e][0] = uint8_t(r << 3 | r >> 2); p[e][1] = uint8_t(g << 2 | g >> 4); p[e][2] = uint8_t(bl << 3 | bl >> 2); p[e][3] = 255;
    }
    if (!allow3 || c0 > c1) {
        for (int ch = 0; ch < 3; ++ch) {
            p[2][ch] = uint8_t((2 * p[0][ch] + p[1][ch] + 1) / 3);
            p[3][ch] = uint8_t((p[0][ch] + 2 * p[1][ch] + 1) / 3);
        }
        p[2][3] = p[3][3] = 255;
    } else {
        for (int ch = 0; ch < 3; ++ch) p[2][ch] = uint8_t((p[0][ch] + p[1][ch]) / 2);
        p[2][3] = 255; p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
    }
    const uint32_t idx = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; ++i) std::memcpy(out + i * 4, p[(idx >> (2 * i)) & 3], 4);
}

// BC4-style 8-byte block -> one byte per texel at out[i*stride]. Signed writes int8 bytes.
void DecodeAlpha(const uint8_t* b, uint8_t* out, unsigned stride, bool sgn) {
    const int r0 = sgn ? int(int8_t(b[0])) : b[0], r1 = sgn ? int(int8_t(b[1])) : b[1];
    const int a0 = r0 == -128 ? -127 : r0, a1 = r1 == -128 ? -127 : r1; // snorm -128 == -127; mode uses raw values
    int p[8] = { a0, a1 };
    auto div = [](int v, int d) { return v >= 0 ? (v + d / 2) / d : -((-v + d / 2) / d); }; // round to nearest
    if (r0 > r1) for (int k = 1; k <= 6; ++k) p[k + 1] = div((7 - k) * a0 + k * a1, 7);
    else {
        for (int k = 1; k <= 4; ++k) p[k + 1] = div((5 - k) * a0 + k * a1, 5);
        p[6] = sgn ? -127 : 0; p[7] = sgn ? 127 : 255;
    }
    uint64_t idx = 0;
    for (int i = 7; i >= 2; --i) idx = idx << 8 | b[i];
    for (unsigned i = 0; i < 16; ++i) out[i * stride] = uint8_t(p[(idx >> (3 * i)) & 7]);
}

// ---------------------------------------------------------------- BC6H (D3DX_BC6H::Decode)
enum Field : uint8_t { NA, M, D, RW, RX, RY, RZ, GW, GX, GY, GZ, BW, BX, BY, BZ };
struct ModeDescriptor { Field m_eField; uint8_t m_uBit; };
struct BC6HModeInfo { uint8_t uMode, uPartitions; bool bTransformed; uint8_t uIndexPrec, RGBPrec[2][2][3]; };

const ModeDescriptor ms_aDesc[14][82] =
{
    {   // Mode 1 (0x00) - 10 5 5 5
        { M, 0}, { M, 1}, {GY, 4}, {BY, 4}, {BZ, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {GZ, 4}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {BZ, 0}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BZ, 1}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {BZ, 2}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {BZ, 3}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 2 (0x01) - 7 6 6 6
        { M, 0}, { M, 1}, {GY, 5}, {GZ, 4}, {GZ, 5}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {BZ, 0}, {BZ, 1}, {BY, 4}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {BY, 5}, {BZ, 2}, {GY, 4}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BZ, 3}, {BZ, 5}, {BZ, 4}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {RX, 5}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {GX, 5}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BX, 5}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {RY, 5}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {RZ, 5}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 3 (0x02) - 11 5 4 4
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {RW,10}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GW,10},
        {BZ, 0}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BW,10},
        {BZ, 1}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {BZ, 2}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {BZ, 3}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 4 (0x06) - 11 4 5 4
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RW,10},
        {GZ, 4}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {GW,10}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BW,10},
        {BZ, 1}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {BZ, 0},
        {BZ, 2}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {GY, 4}, {BZ, 3}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 5 (0x0a) - 11 4 4 5
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RW,10},
        {BY, 4}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GW,10},
        {BZ, 0}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BW,10}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {BZ, 1},
        {BZ, 2}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {BZ, 4}, {BZ, 3}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 6 (0x0e) - 9 5 5 5
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {BY, 4}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GY, 4}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BZ, 4}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {GZ, 4}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {BZ, 0}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BZ, 1}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {BZ, 2}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {BZ, 3}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 7 (0x12) - 8 6 5 5
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {GZ, 4}, {BY, 4}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {BZ, 2}, {GY, 4}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BZ, 3}, {BZ, 4}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {RX, 5}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {BZ, 0}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BZ, 1}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {RY, 5}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {RZ, 5}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 8 (0x16) - 8 5 6 5
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {BZ, 0}, {BY, 4}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GY, 5}, {GY, 4}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {GZ, 5}, {BZ, 4}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {GZ, 4}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {GX, 5}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BZ, 1}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {BZ, 2}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {BZ, 3}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 9 (0x1a) - 8 5 5 6
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {BZ, 1}, {BY, 4}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {BY, 5}, {GY, 4}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BZ, 5}, {BZ, 4}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {GZ, 4}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {BZ, 0}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BX, 5}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {BZ, 2}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {BZ, 3}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 10 (0x1e) - 6 6 6 6
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {GZ, 4}, {BZ, 0}, {BZ, 1}, {BY, 4}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GY, 5}, {BY, 5}, {BZ, 2}, {GY, 4}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {GZ, 5}, {BZ, 3}, {BZ, 5}, {BZ, 4}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {RX, 5}, {GY, 0}, {GY, 1}, {GY, 2}, {GY, 3}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {GX, 5}, {GZ, 0}, {GZ, 1}, {GZ, 2}, {GZ, 3}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BX, 5}, {BY, 0}, {BY, 1}, {BY, 2}, {BY, 3}, {RY, 0}, {RY, 1}, {RY, 2}, {RY, 3}, {RY, 4},
        {RY, 5}, {RZ, 0}, {RZ, 1}, {RZ, 2}, {RZ, 3}, {RZ, 4}, {RZ, 5}, { D, 0}, { D, 1}, { D, 2},
        { D, 3}, { D, 4},
    },

    {   // Mode 11 (0x03) - 10 10
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {RX, 5}, {RX, 6}, {RX, 7}, {RX, 8}, {RX, 9}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {GX, 5}, {GX, 6}, {GX, 7}, {GX, 8}, {GX, 9}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BX, 5}, {BX, 6}, {BX, 7}, {BX, 8}, {BX, 9}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0},
    },

    {   // Mode 12 (0x07) - 11 9
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {RX, 5}, {RX, 6}, {RX, 7}, {RX, 8}, {RW,10}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {GX, 5}, {GX, 6}, {GX, 7}, {GX, 8}, {GW,10}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BX, 5}, {BX, 6}, {BX, 7}, {BX, 8}, {BW,10}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0},
    },

    {   // Mode 13 (0x0b) - 12 8
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RX, 4},
        {RX, 5}, {RX, 6}, {RX, 7}, {RW,11}, {RW,10}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GX, 4},
        {GX, 5}, {GX, 6}, {GX, 7}, {GW,11}, {GW,10}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BX, 4},
        {BX, 5}, {BX, 6}, {BX, 7}, {BW,11}, {BW,10}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0},
    },

    {   // Mode 14 (0x0f) - 16 4
        { M, 0}, { M, 1}, { M, 2}, { M, 3}, { M, 4}, {RW, 0}, {RW, 1}, {RW, 2}, {RW, 3}, {RW, 4},
        {RW, 5}, {RW, 6}, {RW, 7}, {RW, 8}, {RW, 9}, {GW, 0}, {GW, 1}, {GW, 2}, {GW, 3}, {GW, 4},
        {GW, 5}, {GW, 6}, {GW, 7}, {GW, 8}, {GW, 9}, {BW, 0}, {BW, 1}, {BW, 2}, {BW, 3}, {BW, 4},
        {BW, 5}, {BW, 6}, {BW, 7}, {BW, 8}, {BW, 9}, {RX, 0}, {RX, 1}, {RX, 2}, {RX, 3}, {RW,15},
        {RW,14}, {RW,13}, {RW,12}, {RW,11}, {RW,10}, {GX, 0}, {GX, 1}, {GX, 2}, {GX, 3}, {GW,15},
        {GW,14}, {GW,13}, {GW,12}, {GW,11}, {GW,10}, {BX, 0}, {BX, 1}, {BX, 2}, {BX, 3}, {BW,15},
        {BW,14}, {BW,13}, {BW,12}, {BW,11}, {BW,10}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0}, {NA, 0},
        {NA, 0}, {NA, 0},
    },
};

// Mode, Partitions, Transformed, IndexPrec, RGBPrec[region][endpoint][rgb]
const BC6HModeInfo ms_aInfo6[] =
{
    {0x00, 1, true,  3, { { {10,10,10}, {5,5,5} }, { {5,5,5}, {5,5,5} } } }, // Mode 1
    {0x01, 1, true,  3, { { {7,7,7}, {6,6,6} }, { {6,6,6}, {6,6,6} } } }, // Mode 2
    {0x02, 1, true,  3, { { {11,11,11}, {5,4,4} }, { {5,4,4}, {5,4,4} } } }, // Mode 3
    {0x06, 1, true,  3, { { {11,11,11}, {4,5,4} }, { {4,5,4}, {4,5,4} } } }, // Mode 4
    {0x0a, 1, true,  3, { { {11,11,11}, {4,4,5} }, { {4,4,5}, {4,4,5} } } }, // Mode 5
    {0x0e, 1, true,  3, { { {9,9,9}, {5,5,5} }, { {5,5,5}, {5,5,5} } } }, // Mode 6
    {0x12, 1, true,  3, { { {8,8,8}, {6,5,5} }, { {6,5,5}, {6,5,5} } } }, // Mode 7
    {0x16, 1, true,  3, { { {8,8,8}, {5,6,5} }, { {5,6,5}, {5,6,5} } } }, // Mode 8
    {0x1a, 1, true,  3, { { {8,8,8}, {5,5,6} }, { {5,5,6}, {5,5,6} } } }, // Mode 9
    {0x1e, 1, false, 3, { { {6,6,6}, {6,6,6} }, { {6,6,6}, {6,6,6} } } }, // Mode 10
    {0x03, 0, false, 4, { { {10,10,10}, {10,10,10} }, { {0,0,0}, {0,0,0} } } }, // Mode 11
    {0x07, 0, true,  4, { { {11,11,11}, {9,9,9} }, { {0,0,0}, {0,0,0} } } }, // Mode 12
    {0x0b, 0, true,  4, { { {12,12,12}, {8,8,8} }, { {0,0,0}, {0,0,0} } } }, // Mode 13
    {0x0f, 0, true,  4, { { {16,16,16}, {4,4,4} }, { {0,0,0}, {0,0,0} } } }, // Mode 14
};

const int ms_aModeToInfo[32] = { 0, 1, 2, 10, -1, -1, 3, 11, -1, -1, 4, 12, -1, -1, 5, 13, -1, -1, 6, -1, -1, -1, 7, -1, -1, -1, 8, -1, -1, -1, 9, -1 };

inline int SignExtend(int x, int nb) { return (x & (1 << (nb - 1))) ? int(unsigned(x) | (~0u << nb)) : x; }

int Unquantize6(int comp, int uBitsPerComp, bool bSigned) {
    int unq = 0;
    if (bSigned) {
        if (uBitsPerComp >= 16) return comp;
        const bool s = comp < 0; if (s) comp = -comp;
        if (comp == 0) unq = 0;
        else if (comp >= ((1 << (uBitsPerComp - 1)) - 1)) unq = 0x7FFF;
        else unq = ((comp << 15) + 0x4000) >> (uBitsPerComp - 1);
        return s ? -unq : unq;
    }
    if (uBitsPerComp >= 15) unq = comp;
    else if (comp == 0) unq = 0;
    else if (comp == ((1 << uBitsPerComp) - 1)) unq = 0xFFFF;
    else unq = ((comp << 16) + 0x8000) >> uBitsPerComp;
    return unq;
}

// FinishUnquantize + INTColor::INT2F16: returns the f16 bit pattern (signed = sign-magnitude).
inline unsigned FinishUnquantize(int comp, bool bSigned) {
    if (bSigned) return comp < 0 ? 0x8000u | unsigned(((-comp) * 31) >> 5) : unsigned((comp * 31) >> 5);
    return unsigned((comp * 31) >> 6);
}

void FillBC6H(uint8_t* out) { for (int i = 0; i < 16; ++i) { Put16(out + i * 8, 0); Put16(out + i * 8 + 2, 0); Put16(out + i * 8 + 4, 0); Put16(out + i * 8 + 6, 0x3C00); } }

void DecodeBC6H(const uint8_t* blk, uint8_t* out, bool bSigned) {
    Bits bs(blk);
    unsigned uMode = bs.Get(2);
    if (uMode != 0x00 && uMode != 0x01) uMode |= bs.Get(3) << 2;
    if (ms_aModeToInfo[uMode] < 0) { FillBC6H(out); return; } // invalid/reserved: opaque black
    const ModeDescriptor* desc = ms_aDesc[ms_aModeToInfo[uMode]];
    const BC6HModeInfo& info = ms_aInfo6[ms_aModeToInfo[uMode]];

    int ep[2][2][3] = {}; // [region][A/B][rgb]
    unsigned uShape = 0;
    const unsigned uHeaderBits = info.uPartitions > 0 ? 82 : 65;
    while (bs.pos < uHeaderBits) {
        const unsigned uCurBit = bs.pos;
        if (!bs.Get(1)) continue;
        const Field f = desc[uCurBit].m_eField; const unsigned bit = 1u << desc[uCurBit].m_uBit;
        if (f == D) uShape |= bit;
        else if (f >= RW) { const int w = (f - RW) & 3; ep[w >> 1][w & 1][(f - RW) >> 2] |= int(bit); } // W,X,Y,Z = A0,B0,A1,B1
        else { FillBC6H(out); return; } // invalid header bits
    }

    // Sign extend necessary end points
    const uint8_t (*prec)[2][3] = info.RGBPrec;
    if (bSigned) for (int c = 0; c < 3; ++c) ep[0][0][c] = SignExtend(ep[0][0][c], prec[0][0][c]);
    if (bSigned || info.bTransformed)
        for (unsigned p = 0; p <= info.uPartitions; ++p)
            for (int c = 0; c < 3; ++c) {
                if (p != 0) ep[p][0][c] = SignExtend(ep[p][0][c], prec[p][0][c]);
                ep[p][1][c] = SignExtend(ep[p][1][c], prec[p][1][c]);
            }

    // Inverse transform the end points (TransformInverse)
    if (info.bTransformed)
        for (int c = 0; c < 3; ++c) {
            const int nb = prec[0][0][c], wrap = (1 << nb) - 1;
            int* e[3] = { &ep[0][1][c], &ep[1][0][c], &ep[1][1][c] };
            for (int* v : e) { *v = (*v + ep[0][0][c]) & wrap; if (bSigned) *v = SignExtend(*v, nb); }
        }

    // Unquantize end points once per region (reference does it per texel; same result)
    int unq[2][2][3];
    for (int p = 0; p < 2; ++p) for (int e = 0; e < 2; ++e) for (int c = 0; c < 3; ++c)
        unq[p][e][c] = Unquantize6(ep[p][e][c], prec[0][0][c], bSigned);

    const int* aWeights = info.uPartitions > 0 ? g_aWeights3 : g_aWeights4;
    for (unsigned i = 0; i < 16; ++i) {
        const unsigned uNumBits = IsFixUpOffset(info.uPartitions, uShape, i) ? info.uIndexPrec - 1u : info.uIndexPrec;
        const unsigned uIndex = bs.Get(uNumBits);
        const unsigned uRegion = g_aPartitionTable[info.uPartitions][uShape][i];
        const int w = aWeights[uIndex];
        uint8_t* o = out + i * 8;
        for (int c = 0; c < 3; ++c)
            Put16(o + c * 2, FinishUnquantize((unq[uRegion][0][c] * (64 - w) + unq[uRegion][1][c] * w + 32) >> 6, bSigned));
        Put16(o + 6, 0x3C00);
    }
}

// ---------------------------------------------------------------- BC7 (D3DX_BC7::Decode)
struct BC7ModeInfo { uint8_t uPartitions, uPartitionBits, uPBits, uRotationBits, uIndexModeBits, uIndexPrec, uIndexPrec2, RGBAPrec[4], RGBAPrecWithP[4]; };
// uPartitions, uPartitionBits, uPBits, uRotationBits, uIndexModeBits, uIndexPrec, uIndexPrec2, RGBAPrec, RGBAPrecWithP
const BC7ModeInfo ms_aInfo7[8] =
{
    {2, 4, 6, 0, 0, 3, 0, {4,4,4,0}, {5,5,5,0}}, // Mode 0: 3 subsets, RGBP 4441 (unique P), 3-bit idx, 16 partitions
    {1, 6, 2, 0, 0, 3, 0, {6,6,6,0}, {7,7,7,0}}, // Mode 1: 2 subsets, RGBP 6661 (shared P), 3-bit idx
    {2, 6, 0, 0, 0, 2, 0, {5,5,5,0}, {5,5,5,0}}, // Mode 2: 3 subsets, RGB 555, 2-bit idx
    {1, 6, 4, 0, 0, 2, 0, {7,7,7,0}, {8,8,8,0}}, // Mode 3: 2 subsets, RGBP 7771 (unique P), 2-bit idx
    {0, 0, 0, 2, 1, 2, 3, {5,5,5,6}, {5,5,5,6}}, // Mode 4: RGB555 A6, 2/3-bit idx, rotation, index selector
    {0, 0, 0, 2, 0, 2, 2, {7,7,7,8}, {7,7,7,8}}, // Mode 5: RGB777 A8, 2/2-bit idx, rotation
    {0, 0, 2, 0, 0, 4, 0, {7,7,7,7}, {8,8,8,8}}, // Mode 6: RGBAP 77771 (unique P), 4-bit idx
    {1, 6, 4, 0, 0, 2, 0, {5,5,5,5}, {6,6,6,6}}, // Mode 7: 2 subsets, RGBAP 55551 (unique P), 2-bit idx
};

inline unsigned Unquantize7(unsigned comp, unsigned uPrec) { comp = (comp << (8 - uPrec)) & 0xFF; return comp | (comp >> uPrec); }

void DecodeBC7(const uint8_t* blk, uint8_t* out) {
    Bits bs(blk);
    unsigned uMode = 0;
    while (uMode < 8 && !bs.Get(1)) ++uMode;
    if (uMode >= 8) { std::memset(out, 0, 64); return; } // reserved mode 8: transparent black
    const BC7ModeInfo& mi = ms_aInfo7[uMode];
    const unsigned uPartitions = mi.uPartitions, uNumEndPts = (uPartitions + 1) << 1;
    const unsigned uIndexPrec = mi.uIndexPrec, uIndexPrec2 = mi.uIndexPrec2;
    const unsigned uShape = bs.Get(mi.uPartitionBits), uRotation = bs.Get(mi.uRotationBits), uIndexMode = bs.Get(mi.uIndexModeBits);

    unsigned c[6][4];
    for (int ch = 0; ch < 4; ++ch)
        for (unsigned i = 0; i < uNumEndPts; ++i) c[i][ch] = mi.RGBAPrec[ch] ? bs.Get(mi.RGBAPrec[ch]) : 255;
    unsigned P[6];
    for (unsigned i = 0; i < mi.uPBits; ++i) P[i] = bs.Get(1);
    if (mi.uPBits)
        for (unsigned i = 0; i < uNumEndPts; ++i) {
            const unsigned pi = i * mi.uPBits / uNumEndPts;
            for (int ch = 0; ch < 4; ++ch) if (mi.RGBAPrec[ch] != mi.RGBAPrecWithP[ch]) c[i][ch] = (c[i][ch] << 1) | P[pi];
        }
    for (unsigned i = 0; i < uNumEndPts; ++i)
        for (int ch = 0; ch < 4; ++ch) if (mi.RGBAPrecWithP[ch]) c[i][ch] = Unquantize7(c[i][ch], mi.RGBAPrecWithP[ch]);

    unsigned w1[16], w2[16] = {};
    for (unsigned i = 0; i < 16; ++i) w1[i] = bs.Get(IsFixUpOffset(uPartitions, uShape, i) ? uIndexPrec - 1 : uIndexPrec);
    if (uIndexPrec2) for (unsigned i = 0; i < 16; ++i) w2[i] = bs.Get(i ? uIndexPrec2 : uIndexPrec2 - 1);

    for (unsigned i = 0; i < 16; ++i) {
        const unsigned r = g_aPartitionTable[uPartitions][uShape][i];
        const unsigned* e0 = c[r << 1]; const unsigned* e1 = c[(r << 1) + 1];
        // LDRColorA::Interpolate(c0, c1, wc, wa, wcprec, waprec)
        unsigned wc = w1[i], wa = w1[i], pc = uIndexPrec, pa = uIndexPrec;
        if (uIndexPrec2) { if (uIndexMode == 0) { wa = w2[i]; pa = uIndexPrec2; } else { wc = w2[i]; pc = uIndexPrec2; } }
        const unsigned Wc = unsigned(g_aWeights[pc][wc]), Wa = unsigned(g_aWeights[pa][wa]);
        uint8_t px[4];
        for (int ch = 0; ch < 3; ++ch) px[ch] = uint8_t((e0[ch] * (64 - Wc) + e1[ch] * Wc + 32) >> 6);
        px[3] = uint8_t((e0[3] * (64 - Wa) + e1[3] * Wa + 32) >> 6);
        if (uRotation) { const uint8_t t = px[uRotation - 1]; px[uRotation - 1] = px[3]; px[3] = t; } // 1:r<->a 2:g<->a 3:b<->a
        std::memcpy(out + i * 4, px, 4);
    }
}

} // namespace

// ---------------------------------------------------------------- public API
unsigned DecodedBytesPerTexel(Kind k) {
    switch (k) {
    case Kind::BC4U: case Kind::BC4S: return 1;
    case Kind::BC5U: case Kind::BC5S: return 2;
    case Kind::BC6HU: case Kind::BC6HS: return 8;
    default: return 4;
    }
}

unsigned BlockBytes(Kind k) { return (k == Kind::BC1 || k == Kind::BC4U || k == Kind::BC4S) ? 8 : 16; }

void DecodeBlock(Kind k, const uint8_t* b, uint8_t* out) {
    switch (k) {
    case Kind::BC1: DecodeColor(b, out, true); break;
    case Kind::BC2:
        DecodeColor(b + 8, out, false);
        for (int i = 0; i < 16; ++i) out[i * 4 + 3] = uint8_t(((b[i >> 1] >> ((i & 1) * 4)) & 15) * 17);
        break;
    case Kind::BC3: DecodeColor(b + 8, out, false); DecodeAlpha(b, out + 3, 4, false); break;
    case Kind::BC4U: DecodeAlpha(b, out, 1, false); break;
    case Kind::BC4S: DecodeAlpha(b, out, 1, true); break;
    case Kind::BC5U: DecodeAlpha(b, out, 2, false); DecodeAlpha(b + 8, out + 1, 2, false); break;
    case Kind::BC5S: DecodeAlpha(b, out, 2, true); DecodeAlpha(b + 8, out + 1, 2, true); break;
    case Kind::BC6HU: DecodeBC6H(b, out, false); break;
    case Kind::BC6HS: DecodeBC6H(b, out, true); break;
    case Kind::BC7: DecodeBC7(b, out); break;
    }
}

void DecodeRegion(Kind k, const uint8_t* src, size_t srcRowBytes, unsigned blocksWide, unsigned blocksHigh,
                  uint8_t* dst, size_t dstRowBytes, unsigned width, unsigned height) {
    const unsigned bpp = DecodedBytesPerTexel(k), bb = BlockBytes(k);
    uint8_t tmp[16 * 8];
    for (unsigned by = 0; by < blocksHigh && by * 4 < height; ++by) {
        const uint8_t* row = src + size_t(by) * srcRowBytes;
        const unsigned rows = height - by * 4 < 4 ? height - by * 4 : 4;
        for (unsigned bx = 0; bx < blocksWide && bx * 4 < width; ++bx) {
            const unsigned cols = width - bx * 4 < 4 ? width - bx * 4 : 4;
            uint8_t* d = dst + size_t(by) * 4 * dstRowBytes + size_t(bx) * 4 * bpp;
            DecodeBlock(k, row + size_t(bx) * bb, tmp);
            for (unsigned y = 0; y < rows; ++y) std::memcpy(d + y * dstRowBytes, tmp + y * 4 * bpp, cols * bpp);
        }
    }
}

}} // namespace cp2077::bc
