static const char kDepthGatherWGSL[] = R"CPDEPTH(struct unnamed {
    member: array<vec4<f32>, 1>,
}

struct FragmentOutput {
    @location(0) member: f32,
    @location(1) member_1: u32,
    @location(2) member_2: vec2<f32>,
}

@group(0) @binding(576) 
var global: texture_depth_2d;
@group(0) @binding(577) 
var global_1: texture_2d<f32>;
@group(0) @binding(578) 
var global_2: texture_2d<u32>;
@group(0) @binding(518) 
var<uniform> global_3: unnamed;
@group(0) @binding(779) 
var global_4: sampler;
var<private> SV_Position_1: vec4<f32>;
var<private> SV_Target: f32;
var<private> SV_Target_1_: u32;
var<private> SV_Target_2_: vec2<f32>;

fn main_1() {
    let _e13 = SV_Position_1[0u];
    let _e15 = SV_Position_1[1u];
    let _e18 = global_3.member[0u];
    let _e32 = textureGather(global, global_4, vec2<f32>((_e18.z * _e13), (_e18.w * _e15)));
    let _e39 = max(max(_e32.x, _e32.y), max(_e32.z, _e32.w));
    let _e42 = (_e32.z == _e39);
    let _e46 = (_e32.w == _e39);
    SV_Target = _e39;
    let _e49 = (select(select(select(0u, 1u, (_e32.y == _e39)), 1u, _e42), 0u, _e46) | (u32((_e13 + -0.5f)) << bitcast<u32>(1u)));
    let _e50 = (select((select(0u, 1u, _e42) ^ 1u), 0u, _e46) | (u32((_e15 + -0.5f)) << bitcast<u32>(1u)));
    let _e52 = textureLoad(global_2, vec2<u32>(_e49, _e50), 0u);
    SV_Target_1_ = _e52.x;
    let _e55 = textureLoad(global_1, vec2<u32>(_e49, _e50), 0u);
    SV_Target_2_[0u] = _e55.x;
    SV_Target_2_[1u] = _e55.y;
    return;
}

@fragment 
fn main(@builtin(position) SV_Position: vec4<f32>) -> FragmentOutput {
    SV_Position_1 = SV_Position;
    main_1();
    let _e5 = SV_Target;
    let _e6 = SV_Target_1_;
    let _e7 = SV_Target_2_;
    return FragmentOutput(_e5, _e6, _e7);
}
)CPDEPTH";
static const char kDepthGatherHash[] = "ec60ae5ce16f7df1d4638f080eea84a876b9d717ea9db02077f9816f766cd4db";
static const char kDepthGatherOriginalHash[] = "5331e5512b792835996ce7c7725a08907796087c58438755d7386492da3d82e5";
