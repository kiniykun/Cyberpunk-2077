"""[nailfix] Give write-only typed pixel UAVs an explicit storage format (glass, vehicle_glass, water...).

dxil-spirv only emits a typed-UAV image format when the UAV is also read (get_uav_image_format); a write-only
RWTexture2D<uint> becomes OpTypeImage ... Sampled=2 Format=Unknown, which naga rejects ("image read/write without
format": WGSL has no format-less storage textures). For a scalar 32-bit integer RWTexture2D(Array) the DXIL typed
format is R32_UINT / R32_SINT, so the matching SPIR-V format is R32ui (33) / R32i (24) and naga then emits
texture_storage_2d<r32uint, write>. Only images whose sampled type is a 32-bit integer are changed, and only when the
DXIL resource table declares every UAV texture as a scalar integer RWTexture2D/RWTexture2DArray (so a float or
vector UAV can never be guessed). Original instructions and texel values are unchanged.
"""
import re
import struct

R32I, R32UI, R8UI = 24, 33, 39
# Native render-target formats of known pixel UAV slots (renderGraphNodes.cpp: distortionMaskTiles /
# frostedGlassBlurMaskTiles are TEXFMT_R8_Uint, bound by the transparent pass at UnorderedAccessViewBase(3) = u3/u4).
KNOWN_UAV_SLOT_FORMATS = {3: R8UI, 4: R8UI}
UAV_LINE = re.compile(r'^;\s+UAV\s+(\S+)\s+(\S+)\s+\S+\s+u(\d+)', re.M)
SCALAR_INT_UAV = re.compile(r'%"class\.RWTexture2D(?:Array)?<(unsigned int|int)>"')
ANY_UAV_TEXTURE = re.compile(r'%"class\.RWTexture[0-9A-Za-z]*<([^>]*)>"')


def dxil_uav_kinds(dump_text):
    kinds = set(ANY_UAV_TEXTURE.findall(dump_text))
    scalar = set(SCALAR_INT_UAV.findall(dump_text))
    return kinds, scalar


def set_integer_uav_formats(data, dump_text):
    kinds, scalar = dxil_uav_kinds(dump_text)
    if not kinds or kinds - {'unsigned int', 'int'}:
        return data, []
    # The texel format must equal the bound texture's format (WebGPU storage views cannot reinterpret), and the
    # DXIL only says 'u32'. Admit only UAVs whose register maps to a known native render target format.
    slots = [(fmt, dim, int(reg)) for fmt, dim, reg in UAV_LINE.findall(dump_text)]
    if not slots or any(fmt != 'u32' or dim != '2d' or reg not in KNOWN_UAV_SLOT_FORMATS for fmt, dim, reg in slots):
        return data, []
    formats = {KNOWN_UAV_SLOT_FORMATS[reg] for _, _, reg in slots}
    if len(formats) != 1:
        return data, []
    slot_format = formats.pop()
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    ints = {}
    pos = 5
    while pos < len(words):
        n, op = words[pos] >> 16, words[pos] & 0xFFFF
        if not n:
            raise ValueError('Invalid SPIR-V instruction')
        if op == 21:  # OpTypeInt result width signedness
            ints[words[pos + 1]] = (words[pos + 2], words[pos + 3])
        pos += n
    audits = []
    pos = 5
    while pos < len(words):
        n, op = words[pos] >> 16, words[pos] & 0xFFFF
        if op == 25 and n >= 9:  # OpTypeImage result sampledType dim depth arrayed ms sampled format [access]
            result, sampled_type, dim, depth, arrayed, ms, sampled, fmt = words[pos + 1:pos + 9]
            info = ints.get(sampled_type)
            if sampled == 2 and fmt == 0 and dim == 1 and ms == 0 and info and info[0] == 32:
                new = slot_format if not info[1] else None
                if new is not None and 'unsigned int' in scalar:
                    words[pos + 8] = new
                    audits.append({'ImageType': result, 'Arrayed': arrayed, 'Format': {R8UI: 'R8ui', R32UI: 'R32ui'}.get(new, str(new)),
                                   'Scope': 'write-only typed UAV; DXIL RWTexture2D scalar int; instructions unchanged'})
        pos += n
    if not audits:
        return data, []
    return struct.pack('<%dI' % len(words), *words), audits
