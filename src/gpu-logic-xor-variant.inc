// Original light-volume pixel algorithm with exact eight-bit XOR output decomposition.
static const char kLogicXorOriginalHash[]="c8a1be2484e972c0f465c4bd8d3696a789237c1f53c06344986dfdd651b989b6";
static const char kLogicXorVariantHash[]="55877bcf8984b6eb66fbb17e86468854c7fa33f30657e9342f0ca4ad6f69ef30";
static const char kLogicXorWGSL[]=R"CPXOR(struct NativeXorBits{@location(0) bit0:f32,@location(1) bit1:f32,@location(2) bit2:f32,@location(3) bit3:f32,@location(4) bit4:f32,@location(5) bit5:f32,@location(6) bit6:f32,@location(7) bit7:f32}
fn NativeXorOutputs(mask:u32)->NativeXorBits{return NativeXorBits(f32((mask>>0u)&1u),f32((mask>>1u)&1u),f32((mask>>2u)&1u),f32((mask>>3u)&1u),f32((mask>>4u)&1u),f32((mask>>5u)&1u),f32((mask>>6u)&1u),f32((mask>>7u)&1u));}
var<private> SV_Position_1: vec4<f32>;
var<private> TEXCOORD_1: u32;
var<private> SV_Target: u32;

fn main_1() {
    let _e3 = TEXCOORD_1;
    SV_Target = _e3;
    return;
}

@fragment 
fn main(@builtin(position) SV_Position: vec4<f32>, @location(1) @interpolate(flat) TEXCOORD: u32) -> NativeXorBits {
    SV_Position_1 = SV_Position;
    TEXCOORD_1 = TEXCOORD;
    main_1();
    let _e5 = SV_Target;
    return NativeXorOutputs(_e5);
}
)CPXOR";
