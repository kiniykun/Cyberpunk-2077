static const char kGiRGBOutputWGSL[]=R"CPGIRGB(struct Cp2077StorageWords_588 { words: array<u32>, }
@group(0) @binding(588) var<storage, read> cp2077_storage_words_588: Cp2077StorageWords_588;
struct Cp2077StorageWords_587 { words: array<u32>, }
@group(0) @binding(587) var<storage, read> cp2077_storage_words_587: Cp2077StorageWords_587;
struct Cp2077StorageWords_586 { words: array<u32>, }
@group(0) @binding(586) var<storage, read> cp2077_storage_words_586: Cp2077StorageWords_586;
struct Cp2077StorageWords_585 { words: array<u32>, }
@group(0) @binding(585) var<storage, read> cp2077_storage_words_585: Cp2077StorageWords_585;
struct SSBO {
    member: array<u32>,
}

struct SSBO_1 {
    member: array<vec2<u32>>,
}

struct SSBO_2 {
    member: array<vec4<u32>>,
}

struct unnamed {
    member: array<vec4<f32>, 53>,
}

struct unnamed_1 {
    member: array<vec4<f32>, 98>,
}

struct unnamed_2 {
    member: array<vec4<f32>, 15>,
}

@group(0) @binding(576) 
var global: texture_2d<f32>;
@group(0) @binding(578) 
var global_1: texture_2d<f32>;
@group(0) @binding(581) 
var global_2: texture_3d<u32>;
@group(0) @binding(582) 
var global_3: texture_3d<u32>;
@group(0) @binding(583) 
var global_4: texture_2d<u32>;







@group(0) @binding(513) 
var<uniform> global_12: unnamed;
@group(0) @binding(524) 
var<uniform> global_13: unnamed_1;
@group(0) @binding(518) 
var<uniform> global_14: unnamed_2;
var<private> SV_Position_1: vec4<f32>;
var<private> SV_Target: vec3<f32>;

fn main_1() {
    var local: array<u32, 2>;
    var phi_342_: u32;
    var phi_350_: u32;
    var phi_331_: u32;
    var phi_2535_: u32;
    var phi_2536_: u32;
    var phi_498_: u32;
    var phi_2534_: bool;
    var phi_2537_: u32;
    var phi_2539_: f32;
    var phi_2541_: f32;
    var phi_2543_: f32;
    var phi_2545_: f32;
    var phi_2547_: u32;
    var phi_2549_: bool;
    var phi_2538_: u32;
    var phi_2540_: f32;
    var phi_2542_: f32;
    var phi_2544_: f32;
    var phi_2546_: f32;
    var phi_2548_: u32;
    var phi_2550_: bool;
    var phi_327_: bool;
    var phi_330_: u32;
    var phi_332_: f32;
    var phi_335_: f32;
    var phi_337_: f32;
    var phi_339_: f32;
    var phi_341_: u32;
    var phi_581_: f32;
    var phi_582_: f32;
    var phi_583_: f32;
    var phi_584_: f32;
    var phi_585_: f32;
    var phi_586_: f32;
    var phi_587_: f32;
    var phi_588_: f32;
    var phi_589_: f32;
    var phi_590_: f32;
    var phi_591_: f32;
    var phi_592_: f32;
    var phi_2551_: u32;
    var phi_2552_: u32;
    var phi_1464_: u32;
    var phi_1466_: u32;
    var phi_2553_: u32;
    var phi_2554_: u32;
    var phi_1631_: u32;
    var phi_1633_: u32;
    var phi_2555_: u32;
    var phi_2556_: u32;
    var phi_1822_: u32;
    var phi_1824_: u32;
    var phi_611_: f32;
    var phi_2557_: u32;
    var phi_2558_: u32;
    var phi_1603_: u32;
    var phi_1605_: u32;
    var phi_2559_: f32;
    var phi_2560_: f32;
    var phi_1792_: f32;
    var phi_1794_: f32;
    var phi_2561_: f32;
    var phi_2562_: f32;
    var phi_2033_: f32;
    var phi_2035_: f32;
    var phi_613_: f32;
    var phi_2563_: f32;
    var phi_2564_: f32;
    var phi_1766_: f32;
    var phi_1768_: f32;
    var phi_2565_: f32;
    var phi_2566_: f32;
    var phi_2001_: f32;
    var phi_2003_: f32;
    var phi_2567_: f32;
    var phi_2568_: f32;
    var phi_2218_: f32;
    var phi_2220_: f32;
    var phi_615_: f32;
    var phi_2569_: f32;
    var phi_2570_: f32;
    var phi_1975_: f32;
    var phi_1977_: f32;
    var phi_2571_: f32;
    var phi_2572_: f32;
    var phi_2186_: f32;
    var phi_2188_: f32;
    var phi_2573_: f32;
    var phi_2574_: f32;
    var phi_2373_: f32;
    var phi_2375_: f32;
    var phi_2575_: f32;
    var phi_2576_: f32;
    var phi_2577_: f32;
    var phi_2578_: f32;
    var phi_610_: f32;
    var phi_612_: f32;
    var phi_614_: f32;
    var phi_616_: f32;
    var phi_2580_: f32;
    var phi_2583_: f32;
    var phi_2586_: f32;
    var phi_2579_: f32;
    var phi_2582_: f32;
    var phi_2585_: f32;
    var phi_2581_: f32;
    var phi_2584_: f32;
    var phi_2587_: f32;
    var phi_300_: f32;
    var phi_304_: f32;
    var phi_308_: f32;

    let _e72 = SV_Position_1[0u];
    let _e74 = SV_Position_1[1u];
    SV_Target[0u] = 0f;
    SV_Target[1u] = 0f;
    SV_Target[2u] = 0f;
    let _e78 = u32(_e72);
    let _e79 = u32(_e74);
    let _e81 = textureLoad(global, vec2<u32>(_e78, _e79), 0u);
    let _e83 = f32(_e78);
    let _e84 = f32(_e79);
    let _e87 = global_13.member[69u];
    let _e94 = global_13.member[70u];
    let _e101 = global_13.member[71u];
    let _e108 = global_13.member[72u];
    let _e128 = (fma(_e101.w, _e81.x, fma(_e94.w, _e84, (_e87.w * _e83))) + _e108.w);
    let _e129 = ((fma(_e101.x, _e81.x, fma(_e94.x, _e84, (_e87.x * _e83))) + _e108.x) / _e128);
    let _e130 = ((fma(_e101.y, _e81.x, fma(_e94.y, _e84, (_e87.y * _e83))) + _e108.y) / _e128);
    let _e131 = ((fma(_e101.z, _e81.x, fma(_e94.z, _e84, (_e87.z * _e83))) + _e108.z) / _e128);
    let _e133 = textureLoad(global_1, vec2<u32>(_e78, _e79), 0u);
    let _e137 = (_e133.x + -0.5f);
    let _e138 = (_e133.y + -0.5f);
    let _e139 = (_e133.z + -0.5f);
    let _e143 = inverseSqrt(dot(vec3<f32>(_e137, _e138, _e139), vec3<f32>(_e137, _e138, _e139)));
    let _e144 = (_e137 * _e143);
    let _e145 = (_e138 * _e143);
    let _e146 = (_e139 * _e143);
    let _e148 = textureLoad(global_4, vec2<u32>(_e78, _e79), 0u);
    let _e152 = global_14.member[9u];
    let _e159 = min(max((_e148.x >> bitcast<u32>(1u)), 0u), 3u);
    if (bitcast<vec4<u32>>(_e152).w == 0u) {
        let _e162 = global_14.member[10u];
        let _e163 = bitcast<vec4<u32>>(_e162);
        let _e168 = global_14.member[8u];
        let _e169 = bitcast<vec4<u32>>(_e168);
        let _e174 = global_14.member[7u];
        let _e181 = global_14.member[6u];
        let _e186 = global_14.member[1u];
        let _e192 = global_14.member[0u];
        let _e196 = (_e192.x * _e129);
        let _e197 = (_e192.y * _e130);
        let _e198 = (_e192.z * _e131);
        let _e205 = f32(_e163.w);
        let _e209 = u32((clamp((_e196 + _e186.x), 0f, 1f) * _e205));
        let _e210 = u32((clamp((_e197 + _e186.y), 0f, 1f) * _e205));
        let _e211 = u32((clamp((_e198 + _e186.z), 0f, 1f) * _e205));
        let _e213 = textureLoad(global_2, vec3<u32>(_e209, _e210, _e211), 0u);
        local[0u] = _e213.x;
        local[1u] = _e213.y;
        let _e219 = textureLoad(global_3, vec3<u32>(_e209, _e210, _e211), 0u);
        let _e226 = local[(_e159 >> bitcast<u32>(1u))];
        let _e230 = select((_e226 >> bitcast<u32>(16u)), (_e226 & 65535u), ((_e159 & 1u) == 0u));
        let _e233 = global_13.member[46u];
        let _e258 = global_12.member[36u];
        let _e262 = (_e129 - _e258.x);
        let _e263 = (_e130 - _e258.y);
        let _e264 = (_e131 - _e258.z);
        phi_300_ = 0f;
        phi_304_ = 0f;
        phi_308_ = 0f;
        if (((_e219.x < 65535u) && (_e230 < 65535u)) && (min(max(clamp(min(min(clamp(((0.5f - abs(((_e186.x + -0.5f) + _e196))) * _e233.y), 0f, 1f), clamp(((0.5f - abs(((_e186.y + -0.5f) + _e197))) * _e233.y), 0f, 1f)), clamp(((0.5f - abs(((_e186.z + -0.5f) + _e198))) * _e233.y), 0f, 1f)), 0f, 1f), (64f - dot(vec3<f32>(_e262, _e263, _e264), vec3<f32>(_e262, _e263, _e264)))), 1f) > 0f)) {
            let _e278 = cp2077_storage_words_588.words[(((_e219.x) * 1u) + 0u)];
            phi_327_ = false;
            phi_330_ = _e169.x;
            phi_332_ = f32();
            phi_335_ = f32();
            phi_337_ = f32();
            phi_339_ = f32();
            phi_341_ = u32();
            if (bitcast<i32>(_e169.x) > bitcast<i32>(0u)) {
                loop {
                    phi_342_ = (_e278 + _e230);
                    phi_350_ = 0u;
                    phi_331_ = 0u;
                    loop {
                        let _e284 = phi_342_;
                        let _e286 = phi_350_;
                        let _e288 = phi_331_;
                        let _e292 = vec2<u32>(cp2077_storage_words_585.words[((((_e284 * 8u)) * 2u) + 0u)], cp2077_storage_words_585.words[((((_e284 * 8u)) * 2u) + 1u)]);
                        let _e296 = ((_e284 * 16u) + 2u);
                        let _e299 = cp2077_storage_words_585.words[(((_e296) * 1u) + 0u)];
                        let _e303 = cp2077_storage_words_585.words[((((_e296 + 1u)) * 1u) + 0u)];
                        let _e307 = cp2077_storage_words_585.words[((((_e296 + 2u)) * 1u) + 0u)];
                        let _e309 = bitcast<vec3<f32>>(vec3<u32>(_e299, _e303, _e307));
                        let _e314 = ((_e284 * 16u) + 5u);
                        let _e317 = cp2077_storage_words_585.words[(((_e314) * 1u) + 0u)];
                        let _e321 = cp2077_storage_words_585.words[((((_e314 + 1u)) * 1u) + 0u)];
                        let _e325 = cp2077_storage_words_585.words[((((_e314 + 2u)) * 1u) + 0u)];
                        let _e327 = bitcast<vec3<f32>>(vec3<u32>(_e317, _e321, _e325));
                        let _e332 = ((_e284 * 16u) + 8u);
                        let _e335 = cp2077_storage_words_585.words[(((_e332) * 1u) + 0u)];
                        let _e339 = cp2077_storage_words_585.words[((((_e332 + 1u)) * 1u) + 0u)];
                        let _e343 = cp2077_storage_words_585.words[((((_e332 + 2u)) * 1u) + 0u)];
                        let _e345 = bitcast<vec3<f32>>(vec3<u32>(_e335, _e339, _e343));
                        let _e353 = vec4<u32>(cp2077_storage_words_585.words[(((((_e284 * 4u) + 3u)) * 4u) + 0u)], cp2077_storage_words_585.words[(((((_e284 * 4u) + 3u)) * 4u) + 1u)], cp2077_storage_words_585.words[(((((_e284 * 4u) + 3u)) * 4u) + 2u)], cp2077_storage_words_585.words[(((((_e284 * 4u) + 3u)) * 4u) + 3u)]);
                        let _e369 = (_e129 - ((f32((_e292.x & 65535u)) + -32767f) * 0.25f));
                        let _e370 = (_e130 - ((f32((_e292.x >> bitcast<u32>(16u))) + -32767f) * 0.25f));
                        let _e371 = (_e131 - ((f32((_e292.y & 65535u)) + -32767f) * 0.25f));
                        let _e374 = fma(_e371, _e345.x, fma(_e370, _e327.x, (_e369 * _e309.x)));
                        let _e377 = fma(_e371, _e345.y, fma(_e370, _e327.y, (_e369 * _e309.y)));
                        let _e380 = fma(_e371, _e345.z, fma(_e370, _e327.z, (_e369 * _e309.z)));
                        let _e383 = (((1f - _e374) - _e377) - _e380);
                        if (((_e374 < _e377) && (_e374 < _e380)) && (_e374 < _e383)) {
                            phi_498_ = _e353.x;
                        } else {
                            phi_2536_ = _e353.y;
                            if ((_e377 < _e380) && (_e377 < _e383)) {
                            } else {
                                phi_2535_ = _e353.z;
                                if (_e380 < _e383) {
                                } else {
                                    phi_2535_ = _e353.w;
                                }
                                let _e396 = phi_2535_;
                                phi_2536_ = _e396;
                            }
                            let _e398 = phi_2536_;
                            phi_498_ = _e398;
                        }
                        let _e400 = phi_498_;
                        let _e401 = (_e400 + _e278);
                        if (((((_e374 >= 0f) && (_e377 >= 0f)) && (_e380 >= 0f)) && (_e383 >= 0f)) || (_e401 == _e286)) {
                        } else {
                            continue;
                        }
                        let _e411 = max(_e374, 0f);
                        let _e412 = max(_e377, 0f);
                        let _e413 = max(_e380, 0f);
                        let _e414 = max(_e383, 0f);
                        let _e417 = dot(vec4<f32>(_e411, _e412, _e413, _e414), vec4(1f));
                        phi_2534_ = true;
                        phi_2537_ = _e284;
                        phi_2539_ = (_e414 / _e417);
                        phi_2541_ = (_e413 / _e417);
                        phi_2543_ = (_e412 / _e417);
                        phi_2545_ = (_e411 / _e417);
                        phi_2547_ = _e288;
                        phi_2549_ = true;
                        break;
                        continuing {
                            let _e422 = (_e288 + 1u);
                            phi_342_ = _e401;
                            phi_350_ = _e284;
                            phi_331_ = _e422;
                            phi_2534_ = false;
                            phi_2537_ = u32();
                            phi_2539_ = f32();
                            phi_2541_ = f32();
                            phi_2543_ = f32();
                            phi_2545_ = f32();
                            phi_2547_ = _e169.x;
                            phi_2549_ = false;
                            break if !((bitcast<i32>(_e422) < bitcast<i32>(_e169.x)));
                        }
                    }
                    let _e428 = phi_2534_;
                    let _e430 = phi_2537_;
                    let _e432 = phi_2539_;
                    let _e434 = phi_2541_;
                    let _e436 = phi_2543_;
                    let _e438 = phi_2545_;
                    let _e440 = phi_2547_;
                    let _e442 = phi_2549_;
                    phi_2538_ = _e430;
                    phi_2540_ = _e432;
                    phi_2542_ = _e434;
                    phi_2544_ = _e436;
                    phi_2546_ = _e438;
                    phi_2548_ = _e440;
                    phi_2550_ = _e442;
                    if _e428 {
                        break;
                    }
                    phi_2538_ = u32();
                    phi_2540_ = f32();
                    phi_2542_ = f32();
                    phi_2544_ = f32();
                    phi_2546_ = f32();
                    phi_2548_ = _e169.x;
                    phi_2550_ = false;
                    break;
                }
                let _e444 = phi_2538_;
                let _e446 = phi_2540_;
                let _e448 = phi_2542_;
                let _e450 = phi_2544_;
                let _e452 = phi_2546_;
                let _e454 = phi_2548_;
                let _e456 = phi_2550_;
                phi_327_ = _e456;
                phi_330_ = _e454;
                phi_332_ = _e452;
                phi_335_ = _e450;
                phi_337_ = _e448;
                phi_339_ = _e446;
                phi_341_ = _e444;
            }
            let _e458 = phi_327_;
            let _e460 = phi_330_;
            let _e462 = phi_332_;
            let _e464 = phi_335_;
            let _e466 = phi_337_;
            let _e468 = phi_339_;
            let _e470 = phi_341_;
            if _e458 {
                if (_e169.y == 0u) {
                    let _e476 = vec2<u32>(cp2077_storage_words_587.words[(((((_e470 * 10u) + 8u)) * 2u) + 0u)], cp2077_storage_words_587.words[(((((_e470 * 10u) + 8u)) * 2u) + 1u)]);
                    let _e486 = (_e219.x << bitcast<u32>(13u));
                    let _e487 = ((_e476.x & 65535u) + _e486);
                    let _e488 = ((_e476.x >> bitcast<u32>(16u)) + _e486);
                    let _e489 = ((_e476.y & 65535u) + _e486);
                    let _e490 = ((_e476.y >> bitcast<u32>(16u)) + _e486);
                    let _e493 = (bitcast<i32>(_e470) > bitcast<i32>(4294967295u));
                    phi_581_ = f32();
                    phi_582_ = f32();
                    phi_583_ = f32();
                    phi_584_ = f32();
                    phi_585_ = f32();
                    phi_586_ = f32();
                    phi_587_ = f32();
                    phi_588_ = f32();
                    phi_589_ = f32();
                    phi_590_ = f32();
                    phi_591_ = f32();
                    phi_592_ = f32();
                    if _e493 {
                        let _e498 = vec2<u32>(cp2077_storage_words_586.words[(((((_e487 * 7u) + 6u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e487 * 7u) + 6u)) * 2u) + 1u)]);
                        let _e518 = vec2<u32>(cp2077_storage_words_586.words[(((((_e488 * 7u) + 6u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e488 * 7u) + 6u)) * 2u) + 1u)]);
                        let _e538 = vec2<u32>(cp2077_storage_words_586.words[(((((_e489 * 7u) + 6u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e489 * 7u) + 6u)) * 2u) + 1u)]);
                        let _e558 = vec2<u32>(cp2077_storage_words_586.words[(((((_e490 * 7u) + 6u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e490 * 7u) + 6u)) * 2u) + 1u)]);
                        phi_581_ = ((f32((_e498.x & 65535u)) + -32767f) * 0.25f);
                        phi_582_ = ((f32((_e498.x >> bitcast<u32>(16u))) + -32767f) * 0.25f);
                        phi_583_ = ((f32((_e498.y & 65535u)) + -32767f) * 0.25f);
                        phi_584_ = ((f32((_e518.x & 65535u)) + -32767f) * 0.25f);
                        phi_585_ = ((f32((_e518.x >> bitcast<u32>(16u))) + -32767f) * 0.25f);
                        phi_586_ = ((f32((_e518.y & 65535u)) + -32767f) * 0.25f);
                        phi_587_ = ((f32((_e538.x & 65535u)) + -32767f) * 0.25f);
                        phi_588_ = ((f32((_e538.x >> bitcast<u32>(16u))) + -32767f) * 0.25f);
                        phi_589_ = ((f32((_e538.y & 65535u)) + -32767f) * 0.25f);
                        phi_590_ = ((f32((_e558.x & 65535u)) + -32767f) * 0.25f);
                        phi_591_ = ((f32((_e558.x >> bitcast<u32>(16u))) + -32767f) * 0.25f);
                        phi_592_ = ((f32((_e558.y & 65535u)) + -32767f) * 0.25f);
                    }
                    let _e575 = phi_581_;
                    let _e577 = phi_582_;
                    let _e579 = phi_583_;
                    let _e581 = phi_584_;
                    let _e583 = phi_585_;
                    let _e585 = phi_586_;
                    let _e587 = phi_587_;
                    let _e589 = phi_588_;
                    let _e591 = phi_589_;
                    let _e593 = phi_590_;
                    let _e595 = phi_591_;
                    let _e597 = phi_592_;
                    phi_610_ = f32();
                    phi_612_ = f32();
                    phi_614_ = f32();
                    phi_616_ = f32();
                    if _e493 {
                        let _e601 = vec4<u32>(cp2077_storage_words_587.words[((((_e470 * 5u)) * 4u) + 0u)], cp2077_storage_words_587.words[((((_e470 * 5u)) * 4u) + 1u)], cp2077_storage_words_587.words[((((_e470 * 5u)) * 4u) + 2u)], cp2077_storage_words_587.words[((((_e470 * 5u)) * 4u) + 3u)]);
                        phi_611_ = 100f;
                        if (_e601.w > 16777215u) {
                        } else {
                            let _e607 = (_e593 - _e581);
                            let _e608 = (_e595 - _e583);
                            let _e609 = (_e597 - _e585);
                            let _e610 = (_e587 - _e581);
                            let _e611 = (_e589 - _e583);
                            let _e612 = (_e591 - _e585);
                            let _e615 = ((_e609 * _e611) - (_e608 * _e612));
                            let _e618 = ((_e607 * _e612) - (_e609 * _e610));
                            let _e621 = ((_e608 * _e610) - (_e607 * _e611));
                            let _e622 = (_e129 - _e575);
                            let _e623 = (_e130 - _e577);
                            let _e624 = (_e131 - _e579);
                            let _e634 = (dot(vec3<f32>(_e615, _e618, _e621), vec3<f32>((_e581 - _e575), (_e583 - _e577), (_e585 - _e579))) / dot(vec3<f32>(_e615, _e618, _e621), vec3<f32>(_e622, _e623, _e624)));
                            let _e638 = ((_e634 * _e622) + _e575);
                            let _e639 = ((_e634 * _e623) + _e577);
                            let _e640 = ((_e634 * _e624) + _e579);
                            let _e641 = (_e581 - _e593);
                            let _e642 = (_e583 - _e595);
                            let _e643 = (_e585 - _e597);
                            let _e644 = (_e638 - _e593);
                            let _e645 = (_e639 - _e595);
                            let _e646 = (_e640 - _e597);
                            let _e649 = ((_e646 * _e642) - (_e645 * _e643));
                            let _e652 = ((_e644 * _e643) - (_e646 * _e641));
                            let _e655 = ((_e645 * _e641) - (_e644 * _e642));
                            let _e661 = dot(vec3<f32>(_e615, _e618, _e621), vec3<f32>(_e615, _e618, _e621));
                            let _e663 = (_e638 - _e581);
                            let _e664 = (_e639 - _e583);
                            let _e665 = (_e640 - _e585);
                            let _e668 = ((_e665 * _e611) - (_e664 * _e612));
                            let _e671 = ((_e663 * _e612) - (_e665 * _e610));
                            let _e674 = ((_e664 * _e610) - (_e663 * _e611));
                            let _e679 = sqrt((dot(vec3<f32>(_e649, _e652, _e655), vec3<f32>(_e649, _e652, _e655)) / _e661));
                            let _e680 = sqrt((dot(vec3<f32>(_e668, _e671, _e674), vec3<f32>(_e668, _e671, _e674)) / _e661));
                            let _e682 = floor((_e680 * 4f));
                            let _e684 = floor((_e679 * 4f));
                            let _e690 = u32(((_e684 + 0.5f) + ((_e682 * 0.5f) * (11f - _e682))));
                            let _e695 = (_e690 + 1u);
                            let _e696 = (_e690 + (5u - u32(_e682)));
                            let _e701 = (((((_e680 + _e679) - (_e682 * 0.25f)) - (_e684 * 0.25f)) * 4f) > 1f);
                            let _e703 = select(_e695, _e696, _e701);
                            let _e704 = select(_e696, _e695, _e701);
                            let _e705 = select(_e690, (_e696 + 1u), _e701);
                            phi_1464_ = 4u;
                            phi_1466_ = 0u;
                            if (_e703 == 0u) {
                            } else {
                                phi_2551_ = 4u;
                                phi_2552_ = 0u;
                                if (_e703 == 14u) {
                                } else {
                                    let _e710 = ((_e703 << bitcast<u32>(1u)) & 30u);
                                    phi_2551_ = ((262755328u >> bitcast<u32>(_e710)) & 3u);
                                    phi_2552_ = ((18377836u >> bitcast<u32>(_e710)) & 3u);
                                }
                                let _e718 = phi_2551_;
                                let _e720 = phi_2552_;
                                phi_1464_ = _e720;
                                phi_1466_ = _e718;
                            }
                            let _e722 = phi_1464_;
                            let _e724 = phi_1466_;
                            let _e725 = f32(_e722);
                            let _e726 = f32(_e724);
                            let _e728 = ((4f - _e725) - _e726);
                            phi_1631_ = 4u;
                            phi_1633_ = 0u;
                            if (_e704 == 0u) {
                            } else {
                                phi_2553_ = 4u;
                                phi_2554_ = 0u;
                                if (_e704 == 14u) {
                                } else {
                                    let _e751 = ((_e704 << bitcast<u32>(1u)) & 30u);
                                    phi_2553_ = ((262755328u >> bitcast<u32>(_e751)) & 3u);
                                    phi_2554_ = ((18377836u >> bitcast<u32>(_e751)) & 3u);
                                }
                                let _e759 = phi_2553_;
                                let _e761 = phi_2554_;
                                phi_1631_ = _e761;
                                phi_1633_ = _e759;
                            }
                            let _e763 = phi_1631_;
                            let _e765 = phi_1633_;
                            let _e766 = f32(_e763);
                            let _e767 = f32(_e765);
                            let _e769 = ((4f - _e766) - _e767);
                            phi_1822_ = 4u;
                            phi_1824_ = 0u;
                            if (_e705 == 0u) {
                            } else {
                                phi_2555_ = 0u;
                                phi_2556_ = 4u;
                                if (_e705 == 14u) {
                                } else {
                                    let _e792 = ((_e705 << bitcast<u32>(1u)) & 30u);
                                    phi_2555_ = ((18377836u >> bitcast<u32>(_e792)) & 3u);
                                    phi_2556_ = ((262755328u >> bitcast<u32>(_e792)) & 3u);
                                }
                                let _e800 = phi_2555_;
                                let _e802 = phi_2556_;
                                phi_1822_ = _e800;
                                phi_1824_ = _e802;
                            }
                            let _e804 = phi_1822_;
                            let _e806 = phi_1824_;
                            let _e807 = f32(_e804);
                            let _e808 = f32(_e806);
                            let _e810 = ((4f - _e807) - _e808);
                            let _e842 = (f32(((select(select(select(_e601.x, _e601.y, (_e703 > 3u)), _e601.z, (_e703 > 7u)), _e601.w, (_e703 > 11u)) >> bitcast<u32>(((_e703 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e856 = (f32(((select(select(select(_e601.x, _e601.y, (_e704 > 3u)), _e601.z, (_e704 > 7u)), _e601.w, (_e704 > 11u)) >> bitcast<u32>(((_e704 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e870 = (f32(((select(select(select(_e601.x, _e601.y, (_e705 > 3u)), _e601.z, (_e705 > 7u)), _e601.w, (_e705 > 11u)) >> bitcast<u32>(((_e705 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e874 = ((((((_e726 * _e593) + (_e725 * _e581)) + (_e728 * _e587)) * 0.25f) - _e575) * _e842);
                            let _e875 = ((((((_e726 * _e595) + (_e725 * _e583)) + (_e728 * _e589)) * 0.25f) - _e577) * _e842);
                            let _e876 = ((((((_e726 * _e597) + (_e725 * _e585)) + (_e728 * _e591)) * 0.25f) - _e579) * _e842);
                            let _e889 = (((((((_e808 * _e593) + (_e807 * _e581)) + (_e810 * _e587)) * 0.25f) - _e575) * _e870) - _e874);
                            let _e890 = (((((((_e808 * _e595) + (_e807 * _e583)) + (_e810 * _e589)) * 0.25f) - _e577) * _e870) - _e875);
                            let _e891 = (((((((_e808 * _e597) + (_e807 * _e585)) + (_e810 * _e591)) * 0.25f) - _e579) * _e870) - _e876);
                            let _e892 = (((((((_e767 * _e593) + (_e766 * _e581)) + (_e769 * _e587)) * 0.25f) - _e575) * _e856) - _e874);
                            let _e893 = (((((((_e767 * _e595) + (_e766 * _e583)) + (_e769 * _e589)) * 0.25f) - _e577) * _e856) - _e875);
                            let _e894 = (((((((_e767 * _e597) + (_e766 * _e585)) + (_e769 * _e591)) * 0.25f) - _e579) * _e856) - _e876);
                            let _e897 = ((_e891 * _e893) - (_e890 * _e894));
                            let _e900 = ((_e889 * _e894) - (_e891 * _e892));
                            let _e903 = ((_e890 * _e892) - (_e889 * _e893));
                            let _e907 = inverseSqrt(dot(vec3<f32>(_e897, _e900, _e903), vec3<f32>(_e897, _e900, _e903)));
                            let _e908 = (_e897 * _e907);
                            let _e909 = (_e900 * _e907);
                            let _e910 = (_e903 * _e907);
                            let _e917 = (dot(vec3<f32>(_e908, _e909, _e910), vec3<f32>(_e874, _e875, _e876)) / dot(vec3<f32>(_e908, _e909, _e910), vec3<f32>(_e622, _e623, _e624)));
                            let _e918 = (_e917 * _e622);
                            let _e919 = (_e917 * _e623);
                            let _e920 = (_e917 * _e624);
                            let _e927 = (_e575 - _e129);
                            let _e928 = (_e577 - _e130);
                            let _e929 = (_e579 - _e131);
                            let _e939 = inverseSqrt(dot(vec3<f32>(_e927, _e928, _e929), vec3<f32>(_e927, _e928, _e929)));
                            phi_611_ = min(max((((_e174.z - sqrt((((_e918 * _e918) + (_e919 * _e919)) + (_e920 * _e920)))) + sqrt((((_e928 * _e928) + (_e927 * _e927)) + (_e929 * _e929)))) - (clamp((-0f - dot(vec3<f32>(_e908, _e909, _e910), vec3<f32>((_e939 * _e927), (_e939 * _e928), (_e939 * _e929)))), 0f, 1f) * _e174.z)), 0f), 100f);
                        }
                        let _e955 = phi_611_;
                        let _e960 = vec4<u32>(cp2077_storage_words_587.words[(((((_e470 * 5u) + 1u)) * 4u) + 0u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 1u)) * 4u) + 1u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 1u)) * 4u) + 2u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 1u)) * 4u) + 3u)]);
                        phi_613_ = 100f;
                        if (_e960.w > 16777215u) {
                        } else {
                            let _e966 = (_e575 - _e587);
                            let _e967 = (_e577 - _e589);
                            let _e968 = (_e579 - _e591);
                            let _e969 = (_e593 - _e587);
                            let _e970 = (_e595 - _e589);
                            let _e971 = (_e597 - _e591);
                            let _e974 = ((_e970 * _e968) - (_e971 * _e967));
                            let _e977 = ((_e971 * _e966) - (_e969 * _e968));
                            let _e980 = ((_e969 * _e967) - (_e970 * _e966));
                            let _e981 = (_e129 - _e581);
                            let _e982 = (_e130 - _e583);
                            let _e983 = (_e131 - _e585);
                            let _e993 = (dot(vec3<f32>(_e974, _e977, _e980), vec3<f32>((_e587 - _e581), (_e589 - _e583), (_e591 - _e585))) / dot(vec3<f32>(_e974, _e977, _e980), vec3<f32>(_e981, _e982, _e983)));
                            let _e997 = ((_e993 * _e981) + _e581);
                            let _e998 = ((_e993 * _e982) + _e583);
                            let _e999 = ((_e993 * _e983) + _e585);
                            let _e1000 = (_e587 - _e575);
                            let _e1001 = (_e589 - _e577);
                            let _e1002 = (_e591 - _e579);
                            let _e1003 = (_e997 - _e575);
                            let _e1004 = (_e998 - _e577);
                            let _e1005 = (_e999 - _e579);
                            let _e1008 = ((_e1005 * _e1001) - (_e1004 * _e1002));
                            let _e1011 = ((_e1003 * _e1002) - (_e1005 * _e1000));
                            let _e1014 = ((_e1004 * _e1000) - (_e1003 * _e1001));
                            let _e1020 = dot(vec3<f32>(_e974, _e977, _e980), vec3<f32>(_e974, _e977, _e980));
                            let _e1022 = (_e997 - _e587);
                            let _e1023 = (_e998 - _e589);
                            let _e1024 = (_e999 - _e591);
                            let _e1027 = ((_e1024 * _e970) - (_e1023 * _e971));
                            let _e1030 = ((_e1022 * _e971) - (_e1024 * _e969));
                            let _e1033 = ((_e1023 * _e969) - (_e1022 * _e970));
                            let _e1038 = sqrt((dot(vec3<f32>(_e1008, _e1011, _e1014), vec3<f32>(_e1008, _e1011, _e1014)) / _e1020));
                            let _e1039 = sqrt((dot(vec3<f32>(_e1027, _e1030, _e1033), vec3<f32>(_e1027, _e1030, _e1033)) / _e1020));
                            let _e1041 = floor((_e1039 * 4f));
                            let _e1043 = floor((_e1038 * 4f));
                            let _e1049 = u32(((_e1043 + 0.5f) + ((_e1041 * 0.5f) * (11f - _e1041))));
                            let _e1054 = (_e1049 + 1u);
                            let _e1055 = (_e1049 + (5u - u32(_e1041)));
                            let _e1060 = (((((_e1039 + _e1038) - (_e1041 * 0.25f)) - (_e1043 * 0.25f)) * 4f) > 1f);
                            let _e1062 = select(_e1054, _e1055, _e1060);
                            let _e1063 = select(_e1055, _e1054, _e1060);
                            let _e1064 = select(_e1049, (_e1055 + 1u), _e1060);
                            phi_1603_ = 4u;
                            phi_1605_ = 0u;
                            if (_e1062 == 0u) {
                            } else {
                                phi_2557_ = 4u;
                                phi_2558_ = 0u;
                                if (_e1062 == 14u) {
                                } else {
                                    let _e1069 = ((_e1062 << bitcast<u32>(1u)) & 30u);
                                    phi_2557_ = ((262755328u >> bitcast<u32>(_e1069)) & 3u);
                                    phi_2558_ = ((18377836u >> bitcast<u32>(_e1069)) & 3u);
                                }
                                let _e1077 = phi_2557_;
                                let _e1079 = phi_2558_;
                                phi_1603_ = _e1079;
                                phi_1605_ = _e1077;
                            }
                            let _e1081 = phi_1603_;
                            let _e1083 = phi_1605_;
                            let _e1084 = f32(_e1081);
                            let _e1085 = f32(_e1083);
                            let _e1087 = ((4f - _e1084) - _e1085);
                            phi_1792_ = 4f;
                            phi_1794_ = 0f;
                            if (_e1063 == 0u) {
                            } else {
                                phi_2559_ = 4f;
                                phi_2560_ = 0f;
                                if (_e1063 == 14u) {
                                } else {
                                    let _e1110 = ((_e1063 << bitcast<u32>(1u)) & 30u);
                                    phi_2559_ = f32(((262755328u >> bitcast<u32>(_e1110)) & 3u));
                                    phi_2560_ = f32(((18377836u >> bitcast<u32>(_e1110)) & 3u));
                                }
                                let _e1120 = phi_2559_;
                                let _e1122 = phi_2560_;
                                phi_1792_ = _e1122;
                                phi_1794_ = _e1120;
                            }
                            let _e1124 = phi_1792_;
                            let _e1126 = phi_1794_;
                            let _e1128 = ((4f - _e1124) - _e1126);
                            phi_2033_ = 4f;
                            phi_2035_ = 0f;
                            if (_e1064 == 0u) {
                            } else {
                                phi_2561_ = 0f;
                                phi_2562_ = 4f;
                                if (_e1064 == 14u) {
                                } else {
                                    let _e1151 = ((_e1064 << bitcast<u32>(1u)) & 30u);
                                    phi_2561_ = f32(((18377836u >> bitcast<u32>(_e1151)) & 3u));
                                    phi_2562_ = f32(((262755328u >> bitcast<u32>(_e1151)) & 3u));
                                }
                                let _e1161 = phi_2561_;
                                let _e1163 = phi_2562_;
                                phi_2033_ = _e1161;
                                phi_2035_ = _e1163;
                            }
                            let _e1165 = phi_2033_;
                            let _e1167 = phi_2035_;
                            let _e1169 = ((4f - _e1165) - _e1167);
                            let _e1201 = (f32(((select(select(select(_e960.x, _e960.y, (_e1062 > 3u)), _e960.z, (_e1062 > 7u)), _e960.w, (_e1062 > 11u)) >> bitcast<u32>(((_e1062 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1215 = (f32(((select(select(select(_e960.x, _e960.y, (_e1063 > 3u)), _e960.z, (_e1063 > 7u)), _e960.w, (_e1063 > 11u)) >> bitcast<u32>(((_e1063 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1229 = (f32(((select(select(select(_e960.x, _e960.y, (_e1064 > 3u)), _e960.z, (_e1064 > 7u)), _e960.w, (_e1064 > 11u)) >> bitcast<u32>(((_e1064 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1233 = ((((((_e1085 * _e575) + (_e1084 * _e587)) + (_e1087 * _e593)) * 0.25f) - _e581) * _e1201);
                            let _e1234 = ((((((_e1085 * _e577) + (_e1084 * _e589)) + (_e1087 * _e595)) * 0.25f) - _e583) * _e1201);
                            let _e1235 = ((((((_e1085 * _e579) + (_e1084 * _e591)) + (_e1087 * _e597)) * 0.25f) - _e585) * _e1201);
                            let _e1248 = (((((((_e1167 * _e575) + (_e1165 * _e587)) + (_e1169 * _e593)) * 0.25f) - _e581) * _e1229) - _e1233);
                            let _e1249 = (((((((_e1167 * _e577) + (_e1165 * _e589)) + (_e1169 * _e595)) * 0.25f) - _e583) * _e1229) - _e1234);
                            let _e1250 = (((((((_e1167 * _e579) + (_e1165 * _e591)) + (_e1169 * _e597)) * 0.25f) - _e585) * _e1229) - _e1235);
                            let _e1251 = (((((((_e1126 * _e575) + (_e1124 * _e587)) + (_e1128 * _e593)) * 0.25f) - _e581) * _e1215) - _e1233);
                            let _e1252 = (((((((_e1126 * _e577) + (_e1124 * _e589)) + (_e1128 * _e595)) * 0.25f) - _e583) * _e1215) - _e1234);
                            let _e1253 = (((((((_e1126 * _e579) + (_e1124 * _e591)) + (_e1128 * _e597)) * 0.25f) - _e585) * _e1215) - _e1235);
                            let _e1256 = ((_e1250 * _e1252) - (_e1249 * _e1253));
                            let _e1259 = ((_e1248 * _e1253) - (_e1250 * _e1251));
                            let _e1262 = ((_e1249 * _e1251) - (_e1248 * _e1252));
                            let _e1266 = inverseSqrt(dot(vec3<f32>(_e1256, _e1259, _e1262), vec3<f32>(_e1256, _e1259, _e1262)));
                            let _e1267 = (_e1256 * _e1266);
                            let _e1268 = (_e1259 * _e1266);
                            let _e1269 = (_e1262 * _e1266);
                            let _e1276 = (dot(vec3<f32>(_e1267, _e1268, _e1269), vec3<f32>(_e1233, _e1234, _e1235)) / dot(vec3<f32>(_e1267, _e1268, _e1269), vec3<f32>(_e981, _e982, _e983)));
                            let _e1277 = (_e1276 * _e981);
                            let _e1278 = (_e1276 * _e982);
                            let _e1279 = (_e1276 * _e983);
                            let _e1286 = (_e581 - _e129);
                            let _e1287 = (_e583 - _e130);
                            let _e1288 = (_e585 - _e131);
                            let _e1298 = inverseSqrt(dot(vec3<f32>(_e1286, _e1287, _e1288), vec3<f32>(_e1286, _e1287, _e1288)));
                            phi_613_ = min(max((((_e174.z - sqrt((((_e1277 * _e1277) + (_e1278 * _e1278)) + (_e1279 * _e1279)))) + sqrt((((_e1287 * _e1287) + (_e1286 * _e1286)) + (_e1288 * _e1288)))) - (clamp((-0f - dot(vec3<f32>(_e1267, _e1268, _e1269), vec3<f32>((_e1298 * _e1286), (_e1298 * _e1287), (_e1298 * _e1288)))), 0f, 1f) * _e174.z)), 0f), 100f);
                        }
                        let _e1314 = phi_613_;
                        let _e1319 = vec4<u32>(cp2077_storage_words_587.words[(((((_e470 * 5u) + 2u)) * 4u) + 0u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 2u)) * 4u) + 1u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 2u)) * 4u) + 2u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 2u)) * 4u) + 3u)]);
                        phi_615_ = 100f;
                        if (_e1319.w > 16777215u) {
                        } else {
                            let _e1325 = (_e581 - _e593);
                            let _e1326 = (_e583 - _e595);
                            let _e1327 = (_e585 - _e597);
                            let _e1328 = (_e575 - _e593);
                            let _e1329 = (_e577 - _e595);
                            let _e1330 = (_e579 - _e597);
                            let _e1333 = ((_e1327 * _e1329) - (_e1330 * _e1326));
                            let _e1336 = ((_e1330 * _e1325) - (_e1327 * _e1328));
                            let _e1339 = ((_e1326 * _e1328) - (_e1329 * _e1325));
                            let _e1340 = (_e129 - _e587);
                            let _e1341 = (_e130 - _e589);
                            let _e1342 = (_e131 - _e591);
                            let _e1352 = (dot(vec3<f32>(_e1333, _e1336, _e1339), vec3<f32>((_e593 - _e587), (_e595 - _e589), (_e597 - _e591))) / dot(vec3<f32>(_e1333, _e1336, _e1339), vec3<f32>(_e1340, _e1341, _e1342)));
                            let _e1356 = ((_e1352 * _e1340) + _e587);
                            let _e1357 = ((_e1352 * _e1341) + _e589);
                            let _e1358 = ((_e1352 * _e1342) + _e591);
                            let _e1359 = (_e593 - _e581);
                            let _e1360 = (_e595 - _e583);
                            let _e1361 = (_e597 - _e585);
                            let _e1362 = (_e1356 - _e581);
                            let _e1363 = (_e1357 - _e583);
                            let _e1364 = (_e1358 - _e585);
                            let _e1367 = ((_e1364 * _e1360) - (_e1363 * _e1361));
                            let _e1370 = ((_e1362 * _e1361) - (_e1364 * _e1359));
                            let _e1373 = ((_e1363 * _e1359) - (_e1362 * _e1360));
                            let _e1379 = dot(vec3<f32>(_e1333, _e1336, _e1339), vec3<f32>(_e1333, _e1336, _e1339));
                            let _e1381 = (_e1356 - _e593);
                            let _e1382 = (_e1357 - _e595);
                            let _e1383 = (_e1358 - _e597);
                            let _e1386 = ((_e1383 * _e1329) - (_e1382 * _e1330));
                            let _e1389 = ((_e1381 * _e1330) - (_e1383 * _e1328));
                            let _e1392 = ((_e1382 * _e1328) - (_e1381 * _e1329));
                            let _e1397 = sqrt((dot(vec3<f32>(_e1367, _e1370, _e1373), vec3<f32>(_e1367, _e1370, _e1373)) / _e1379));
                            let _e1398 = sqrt((dot(vec3<f32>(_e1386, _e1389, _e1392), vec3<f32>(_e1386, _e1389, _e1392)) / _e1379));
                            let _e1400 = floor((_e1398 * 4f));
                            let _e1402 = floor((_e1397 * 4f));
                            let _e1408 = u32(((_e1402 + 0.5f) + ((_e1400 * 0.5f) * (11f - _e1400))));
                            let _e1413 = (_e1408 + 1u);
                            let _e1414 = (_e1408 + (5u - u32(_e1400)));
                            let _e1419 = (((((_e1398 + _e1397) - (_e1400 * 0.25f)) - (_e1402 * 0.25f)) * 4f) > 1f);
                            let _e1421 = select(_e1413, _e1414, _e1419);
                            let _e1422 = select(_e1414, _e1413, _e1419);
                            let _e1423 = select(_e1408, (_e1414 + 1u), _e1419);
                            phi_1766_ = 4f;
                            phi_1768_ = 0f;
                            if (_e1421 == 0u) {
                            } else {
                                phi_2563_ = 4f;
                                phi_2564_ = 0f;
                                if (_e1421 == 14u) {
                                } else {
                                    let _e1428 = ((_e1421 << bitcast<u32>(1u)) & 30u);
                                    phi_2563_ = f32(((262755328u >> bitcast<u32>(_e1428)) & 3u));
                                    phi_2564_ = f32(((18377836u >> bitcast<u32>(_e1428)) & 3u));
                                }
                                let _e1438 = phi_2563_;
                                let _e1440 = phi_2564_;
                                phi_1766_ = _e1440;
                                phi_1768_ = _e1438;
                            }
                            let _e1442 = phi_1766_;
                            let _e1444 = phi_1768_;
                            let _e1446 = ((4f - _e1442) - _e1444);
                            phi_2001_ = 4f;
                            phi_2003_ = 0f;
                            if (_e1422 == 0u) {
                            } else {
                                phi_2565_ = 4f;
                                phi_2566_ = 0f;
                                if (_e1422 == 14u) {
                                } else {
                                    let _e1469 = ((_e1422 << bitcast<u32>(1u)) & 30u);
                                    phi_2565_ = f32(((262755328u >> bitcast<u32>(_e1469)) & 3u));
                                    phi_2566_ = f32(((18377836u >> bitcast<u32>(_e1469)) & 3u));
                                }
                                let _e1479 = phi_2565_;
                                let _e1481 = phi_2566_;
                                phi_2001_ = _e1481;
                                phi_2003_ = _e1479;
                            }
                            let _e1483 = phi_2001_;
                            let _e1485 = phi_2003_;
                            let _e1487 = ((4f - _e1483) - _e1485);
                            phi_2218_ = 4f;
                            phi_2220_ = 0f;
                            if (_e1423 == 0u) {
                            } else {
                                phi_2567_ = 4f;
                                phi_2568_ = 0f;
                                if (_e1423 == 14u) {
                                } else {
                                    let _e1510 = ((_e1423 << bitcast<u32>(1u)) & 30u);
                                    phi_2567_ = f32(((262755328u >> bitcast<u32>(_e1510)) & 3u));
                                    phi_2568_ = f32(((18377836u >> bitcast<u32>(_e1510)) & 3u));
                                }
                                let _e1520 = phi_2567_;
                                let _e1522 = phi_2568_;
                                phi_2218_ = _e1522;
                                phi_2220_ = _e1520;
                            }
                            let _e1524 = phi_2218_;
                            let _e1526 = phi_2220_;
                            let _e1528 = ((4f - _e1524) - _e1526);
                            let _e1560 = (f32(((select(select(select(_e1319.x, _e1319.y, (_e1421 > 3u)), _e1319.z, (_e1421 > 7u)), _e1319.w, (_e1421 > 11u)) >> bitcast<u32>(((_e1421 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1574 = (f32(((select(select(select(_e1319.x, _e1319.y, (_e1422 > 3u)), _e1319.z, (_e1422 > 7u)), _e1319.w, (_e1422 > 11u)) >> bitcast<u32>(((_e1422 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1588 = (f32(((select(select(select(_e1319.x, _e1319.y, (_e1423 > 3u)), _e1319.z, (_e1423 > 7u)), _e1319.w, (_e1423 > 11u)) >> bitcast<u32>(((_e1423 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1592 = ((((((_e1444 * _e581) + (_e1442 * _e593)) + (_e1446 * _e575)) * 0.25f) - _e587) * _e1560);
                            let _e1593 = ((((((_e1444 * _e583) + (_e1442 * _e595)) + (_e1446 * _e577)) * 0.25f) - _e589) * _e1560);
                            let _e1594 = ((((((_e1444 * _e585) + (_e1442 * _e597)) + (_e1446 * _e579)) * 0.25f) - _e591) * _e1560);
                            let _e1607 = (((((((_e1526 * _e581) + (_e1524 * _e593)) + (_e1528 * _e575)) * 0.25f) - _e587) * _e1588) - _e1592);
                            let _e1608 = (((((((_e1526 * _e583) + (_e1524 * _e595)) + (_e1528 * _e577)) * 0.25f) - _e589) * _e1588) - _e1593);
                            let _e1609 = (((((((_e1526 * _e585) + (_e1524 * _e597)) + (_e1528 * _e579)) * 0.25f) - _e591) * _e1588) - _e1594);
                            let _e1610 = (((((((_e1485 * _e581) + (_e1483 * _e593)) + (_e1487 * _e575)) * 0.25f) - _e587) * _e1574) - _e1592);
                            let _e1611 = (((((((_e1485 * _e583) + (_e1483 * _e595)) + (_e1487 * _e577)) * 0.25f) - _e589) * _e1574) - _e1593);
                            let _e1612 = (((((((_e1485 * _e585) + (_e1483 * _e597)) + (_e1487 * _e579)) * 0.25f) - _e591) * _e1574) - _e1594);
                            let _e1615 = ((_e1609 * _e1611) - (_e1608 * _e1612));
                            let _e1618 = ((_e1607 * _e1612) - (_e1609 * _e1610));
                            let _e1621 = ((_e1608 * _e1610) - (_e1607 * _e1611));
                            let _e1625 = inverseSqrt(dot(vec3<f32>(_e1615, _e1618, _e1621), vec3<f32>(_e1615, _e1618, _e1621)));
                            let _e1626 = (_e1615 * _e1625);
                            let _e1627 = (_e1618 * _e1625);
                            let _e1628 = (_e1621 * _e1625);
                            let _e1635 = (dot(vec3<f32>(_e1626, _e1627, _e1628), vec3<f32>(_e1592, _e1593, _e1594)) / dot(vec3<f32>(_e1626, _e1627, _e1628), vec3<f32>(_e1340, _e1341, _e1342)));
                            let _e1636 = (_e1635 * _e1340);
                            let _e1637 = (_e1635 * _e1341);
                            let _e1638 = (_e1635 * _e1342);
                            let _e1645 = (_e587 - _e129);
                            let _e1646 = (_e589 - _e130);
                            let _e1647 = (_e591 - _e131);
                            let _e1657 = inverseSqrt(dot(vec3<f32>(_e1645, _e1646, _e1647), vec3<f32>(_e1645, _e1646, _e1647)));
                            phi_615_ = min(max((((_e174.z - sqrt((((_e1636 * _e1636) + (_e1637 * _e1637)) + (_e1638 * _e1638)))) + sqrt((((_e1646 * _e1646) + (_e1645 * _e1645)) + (_e1647 * _e1647)))) - (clamp((-0f - dot(vec3<f32>(_e1626, _e1627, _e1628), vec3<f32>((_e1657 * _e1645), (_e1657 * _e1646), (_e1657 * _e1647)))), 0f, 1f) * _e174.z)), 0f), 100f);
                        }
                        let _e1673 = phi_615_;
                        let _e1678 = vec4<u32>(cp2077_storage_words_587.words[(((((_e470 * 5u) + 3u)) * 4u) + 0u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 3u)) * 4u) + 1u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 3u)) * 4u) + 2u)], cp2077_storage_words_587.words[(((((_e470 * 5u) + 3u)) * 4u) + 3u)]);
                        phi_2575_ = 100f;
                        phi_2576_ = _e1673;
                        phi_2577_ = _e1314;
                        phi_2578_ = _e955;
                        if (_e1678.w > 16777215u) {
                        } else {
                            let _e1684 = (_e587 - _e575);
                            let _e1685 = (_e589 - _e577);
                            let _e1686 = (_e591 - _e579);
                            let _e1687 = (_e581 - _e575);
                            let _e1688 = (_e583 - _e577);
                            let _e1689 = (_e585 - _e579);
                            let _e1692 = ((_e1686 * _e1688) - (_e1685 * _e1689));
                            let _e1695 = ((_e1684 * _e1689) - (_e1686 * _e1687));
                            let _e1698 = ((_e1685 * _e1687) - (_e1684 * _e1688));
                            let _e1699 = (_e129 - _e593);
                            let _e1700 = (_e130 - _e595);
                            let _e1701 = (_e131 - _e597);
                            let _e1711 = (dot(vec3<f32>(_e1692, _e1695, _e1698), vec3<f32>((_e575 - _e593), (_e577 - _e595), (_e579 - _e597))) / dot(vec3<f32>(_e1692, _e1695, _e1698), vec3<f32>(_e1699, _e1700, _e1701)));
                            let _e1715 = ((_e1711 * _e1699) + _e593);
                            let _e1716 = ((_e1711 * _e1700) + _e595);
                            let _e1717 = ((_e1711 * _e1701) + _e597);
                            let _e1718 = (_e575 - _e587);
                            let _e1719 = (_e577 - _e589);
                            let _e1720 = (_e579 - _e591);
                            let _e1721 = (_e1715 - _e587);
                            let _e1722 = (_e1716 - _e589);
                            let _e1723 = (_e1717 - _e591);
                            let _e1726 = ((_e1723 * _e1719) - (_e1722 * _e1720));
                            let _e1729 = ((_e1721 * _e1720) - (_e1723 * _e1718));
                            let _e1732 = ((_e1722 * _e1718) - (_e1721 * _e1719));
                            let _e1738 = dot(vec3<f32>(_e1692, _e1695, _e1698), vec3<f32>(_e1692, _e1695, _e1698));
                            let _e1740 = (_e1715 - _e575);
                            let _e1741 = (_e1716 - _e577);
                            let _e1742 = (_e1717 - _e579);
                            let _e1745 = ((_e1742 * _e1688) - (_e1741 * _e1689));
                            let _e1748 = ((_e1740 * _e1689) - (_e1742 * _e1687));
                            let _e1751 = ((_e1741 * _e1687) - (_e1740 * _e1688));
                            let _e1756 = sqrt((dot(vec3<f32>(_e1726, _e1729, _e1732), vec3<f32>(_e1726, _e1729, _e1732)) / _e1738));
                            let _e1757 = sqrt((dot(vec3<f32>(_e1745, _e1748, _e1751), vec3<f32>(_e1745, _e1748, _e1751)) / _e1738));
                            let _e1759 = floor((_e1757 * 4f));
                            let _e1761 = floor((_e1756 * 4f));
                            let _e1767 = u32(((_e1761 + 0.5f) + ((_e1759 * 0.5f) * (11f - _e1759))));
                            let _e1772 = (_e1767 + 1u);
                            let _e1773 = (_e1767 + (5u - u32(_e1759)));
                            let _e1778 = (((((_e1757 + _e1756) - (_e1759 * 0.25f)) - (_e1761 * 0.25f)) * 4f) > 1f);
                            let _e1780 = select(_e1772, _e1773, _e1778);
                            let _e1781 = select(_e1773, _e1772, _e1778);
                            let _e1782 = select(_e1767, (_e1773 + 1u), _e1778);
                            phi_1975_ = 0f;
                            phi_1977_ = 4f;
                            if (_e1780 == 0u) {
                            } else {
                                phi_2569_ = 0f;
                                phi_2570_ = 4f;
                                if (_e1780 == 14u) {
                                } else {
                                    let _e1787 = ((_e1780 << bitcast<u32>(1u)) & 30u);
                                    phi_2569_ = f32(((18377836u >> bitcast<u32>(_e1787)) & 3u));
                                    phi_2570_ = f32(((262755328u >> bitcast<u32>(_e1787)) & 3u));
                                }
                                let _e1797 = phi_2569_;
                                let _e1799 = phi_2570_;
                                phi_1975_ = _e1799;
                                phi_1977_ = _e1797;
                            }
                            let _e1801 = phi_1975_;
                            let _e1803 = phi_1977_;
                            let _e1805 = ((4f - _e1803) - _e1801);
                            phi_2186_ = 0f;
                            phi_2188_ = 4f;
                            if (_e1781 == 0u) {
                            } else {
                                phi_2571_ = 0f;
                                phi_2572_ = 4f;
                                if (_e1781 == 14u) {
                                } else {
                                    let _e1828 = ((_e1781 << bitcast<u32>(1u)) & 30u);
                                    phi_2571_ = f32(((18377836u >> bitcast<u32>(_e1828)) & 3u));
                                    phi_2572_ = f32(((262755328u >> bitcast<u32>(_e1828)) & 3u));
                                }
                                let _e1838 = phi_2571_;
                                let _e1840 = phi_2572_;
                                phi_2186_ = _e1840;
                                phi_2188_ = _e1838;
                            }
                            let _e1842 = phi_2186_;
                            let _e1844 = phi_2188_;
                            let _e1846 = ((4f - _e1844) - _e1842);
                            phi_2373_ = 0f;
                            phi_2375_ = 4f;
                            if (_e1782 == 0u) {
                            } else {
                                phi_2573_ = 0f;
                                phi_2574_ = 4f;
                                if (_e1782 == 14u) {
                                } else {
                                    let _e1869 = ((_e1782 << bitcast<u32>(1u)) & 30u);
                                    phi_2573_ = f32(((18377836u >> bitcast<u32>(_e1869)) & 3u));
                                    phi_2574_ = f32(((262755328u >> bitcast<u32>(_e1869)) & 3u));
                                }
                                let _e1879 = phi_2573_;
                                let _e1881 = phi_2574_;
                                phi_2373_ = _e1881;
                                phi_2375_ = _e1879;
                            }
                            let _e1883 = phi_2373_;
                            let _e1885 = phi_2375_;
                            let _e1887 = ((4f - _e1885) - _e1883);
                            let _e1919 = (f32(((select(select(select(_e1678.x, _e1678.y, (_e1780 > 3u)), _e1678.z, (_e1780 > 7u)), _e1678.w, (_e1780 > 11u)) >> bitcast<u32>(((_e1780 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1933 = (f32(((select(select(select(_e1678.x, _e1678.y, (_e1781 > 3u)), _e1678.z, (_e1781 > 7u)), _e1678.w, (_e1781 > 11u)) >> bitcast<u32>(((_e1781 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1947 = (f32(((select(select(select(_e1678.x, _e1678.y, (_e1782 > 3u)), _e1678.z, (_e1782 > 7u)), _e1678.w, (_e1782 > 11u)) >> bitcast<u32>(((_e1782 << bitcast<u32>(3u)) & 24u))) & 255u)) * 0.003921569f);
                            let _e1951 = ((((((_e1803 * _e575) + (_e1801 * _e587)) + (_e1805 * _e581)) * 0.25f) - _e593) * _e1919);
                            let _e1952 = ((((((_e1803 * _e577) + (_e1801 * _e589)) + (_e1805 * _e583)) * 0.25f) - _e595) * _e1919);
                            let _e1953 = ((((((_e1803 * _e579) + (_e1801 * _e591)) + (_e1805 * _e585)) * 0.25f) - _e597) * _e1919);
                            let _e1966 = (((((((_e1885 * _e575) + (_e1883 * _e587)) + (_e1887 * _e581)) * 0.25f) - _e593) * _e1947) - _e1951);
                            let _e1967 = (((((((_e1885 * _e577) + (_e1883 * _e589)) + (_e1887 * _e583)) * 0.25f) - _e595) * _e1947) - _e1952);
                            let _e1968 = (((((((_e1885 * _e579) + (_e1883 * _e591)) + (_e1887 * _e585)) * 0.25f) - _e597) * _e1947) - _e1953);
                            let _e1969 = (((((((_e1844 * _e575) + (_e1842 * _e587)) + (_e1846 * _e581)) * 0.25f) - _e593) * _e1933) - _e1951);
                            let _e1970 = (((((((_e1844 * _e577) + (_e1842 * _e589)) + (_e1846 * _e583)) * 0.25f) - _e595) * _e1933) - _e1952);
                            let _e1971 = (((((((_e1844 * _e579) + (_e1842 * _e591)) + (_e1846 * _e585)) * 0.25f) - _e597) * _e1933) - _e1953);
                            let _e1974 = ((_e1968 * _e1970) - (_e1967 * _e1971));
                            let _e1977 = ((_e1966 * _e1971) - (_e1968 * _e1969));
                            let _e1980 = ((_e1967 * _e1969) - (_e1966 * _e1970));
                            let _e1984 = inverseSqrt(dot(vec3<f32>(_e1974, _e1977, _e1980), vec3<f32>(_e1974, _e1977, _e1980)));
                            let _e1985 = (_e1974 * _e1984);
                            let _e1986 = (_e1977 * _e1984);
                            let _e1987 = (_e1980 * _e1984);
                            let _e1994 = (dot(vec3<f32>(_e1985, _e1986, _e1987), vec3<f32>(_e1951, _e1952, _e1953)) / dot(vec3<f32>(_e1985, _e1986, _e1987), vec3<f32>(_e1699, _e1700, _e1701)));
                            let _e1995 = (_e1994 * _e1699);
                            let _e1996 = (_e1994 * _e1700);
                            let _e1997 = (_e1994 * _e1701);
                            let _e2004 = (_e593 - _e129);
                            let _e2005 = (_e595 - _e130);
                            let _e2006 = (_e597 - _e131);
                            let _e2016 = inverseSqrt(dot(vec3<f32>(_e2004, _e2005, _e2006), vec3<f32>(_e2004, _e2005, _e2006)));
                            phi_2575_ = min(max((((_e174.z - sqrt((((_e1995 * _e1995) + (_e1996 * _e1996)) + (_e1997 * _e1997)))) + sqrt((((_e2005 * _e2005) + (_e2004 * _e2004)) + (_e2006 * _e2006)))) - (clamp((-0f - dot(vec3<f32>(_e1985, _e1986, _e1987), vec3<f32>((_e2016 * _e2004), (_e2016 * _e2005), (_e2016 * _e2006)))), 0f, 1f) * _e174.z)), 0f), 100f);
                            phi_2576_ = _e1673;
                            phi_2577_ = _e1314;
                            phi_2578_ = _e955;
                        }
                        let _e2032 = phi_2575_;
                        let _e2034 = phi_2576_;
                        let _e2036 = phi_2577_;
                        let _e2038 = phi_2578_;
                        phi_610_ = _e2038;
                        phi_612_ = _e2036;
                        phi_614_ = _e2034;
                        phi_616_ = _e2032;
                    }
                    let _e2040 = phi_610_;
                    let _e2042 = phi_612_;
                    let _e2044 = phi_614_;
                    let _e2046 = phi_616_;
                    let _e2047 = (_e129 - _e575);
                    let _e2048 = (_e130 - _e577);
                    let _e2049 = (_e131 - _e579);
                    let _e2056 = (_e129 - _e581);
                    let _e2057 = (_e130 - _e583);
                    let _e2058 = (_e131 - _e585);
                    let _e2065 = (_e129 - _e587);
                    let _e2066 = (_e130 - _e589);
                    let _e2067 = (_e131 - _e591);
                    let _e2074 = (_e129 - _e593);
                    let _e2075 = (_e130 - _e595);
                    let _e2076 = (_e131 - _e597);
                    let _e2083 = (_e575 - _e129);
                    let _e2084 = (_e577 - _e130);
                    let _e2085 = (_e579 - _e131);
                    let _e2089 = inverseSqrt(dot(vec3<f32>(_e2083, _e2084, _e2085), vec3<f32>(_e2083, _e2084, _e2085)));
                    let _e2096 = (_e581 - _e129);
                    let _e2097 = (_e583 - _e130);
                    let _e2098 = (_e585 - _e131);
                    let _e2102 = inverseSqrt(dot(vec3<f32>(_e2096, _e2097, _e2098), vec3<f32>(_e2096, _e2097, _e2098)));
                    let _e2109 = (_e587 - _e129);
                    let _e2110 = (_e589 - _e130);
                    let _e2111 = (_e591 - _e131);
                    let _e2115 = inverseSqrt(dot(vec3<f32>(_e2109, _e2110, _e2111), vec3<f32>(_e2109, _e2110, _e2111)));
                    let _e2122 = (_e593 - _e129);
                    let _e2123 = (_e595 - _e130);
                    let _e2124 = (_e597 - _e131);
                    let _e2128 = inverseSqrt(dot(vec3<f32>(_e2122, _e2123, _e2124), vec3<f32>(_e2122, _e2123, _e2124)));
                    let _e2151 = (((_e2040 * _e181.w) - (dot(vec3<f32>(_e144, _e145, _e146), vec3<f32>((_e2089 * _e2083), (_e2089 * _e2084), (_e2089 * _e2085))) * _e174.y)) + (sqrt((((_e2048 * _e2048) + (_e2047 * _e2047)) + (_e2049 * _e2049))) * _e174.x));
                    let _e2152 = (((_e2042 * _e181.w) - (dot(vec3<f32>(_e144, _e145, _e146), vec3<f32>((_e2102 * _e2096), (_e2102 * _e2097), (_e2102 * _e2098))) * _e174.y)) + (sqrt((((_e2057 * _e2057) + (_e2056 * _e2056)) + (_e2058 * _e2058))) * _e174.x));
                    let _e2153 = (((_e2044 * _e181.w) - (dot(vec3<f32>(_e144, _e145, _e146), vec3<f32>((_e2115 * _e2109), (_e2115 * _e2110), (_e2115 * _e2111))) * _e174.y)) + (sqrt((((_e2066 * _e2066) + (_e2065 * _e2065)) + (_e2067 * _e2067))) * _e174.x));
                    let _e2154 = (((_e2046 * _e181.w) - (dot(vec3<f32>(_e144, _e145, _e146), vec3<f32>((_e2128 * _e2122), (_e2128 * _e2123), (_e2128 * _e2124))) * _e174.y)) + (sqrt((((_e2075 * _e2075) + (_e2074 * _e2074)) + (_e2076 * _e2076))) * _e174.x));
                    let _e2157 = min(min(_e2151, _e2152), min(_e2153, _e2154));
                    let _e2163 = (1f / _e174.w);
                    let _e2165 = clamp(((clamp((_e174.w - abs((_e2151 - _e2157))), 0f, 1f) * _e462) * _e2163), 0f, 1f);
                    let _e2170 = vec2<u32>(cp2077_storage_words_586.words[((((_e487 * 7u)) * 2u) + 0u)], cp2077_storage_words_586.words[((((_e487 * 7u)) * 2u) + 1u)]);
                    let _e2177 = vec2<u32>(cp2077_storage_words_586.words[(((((_e487 * 7u) + 1u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e487 * 7u) + 1u)) * 2u) + 1u)]);
                    let _e2184 = vec2<u32>(cp2077_storage_words_586.words[(((((_e487 * 7u) + 2u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e487 * 7u) + 2u)) * 2u) + 1u)]);
                    let _e2191 = vec2<u32>(cp2077_storage_words_586.words[(((((_e487 * 7u) + 3u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e487 * 7u) + 3u)) * 2u) + 1u)]);
                    let _e2198 = vec2<u32>(cp2077_storage_words_586.words[(((((_e487 * 7u) + 4u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e487 * 7u) + 4u)) * 2u) + 1u)]);
                    let _e2205 = vec2<u32>(cp2077_storage_words_586.words[(((((_e487 * 7u) + 5u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e487 * 7u) + 5u)) * 2u) + 1u)]);
                    let _e2208 = max(_e144, 0f);
                    let _e2209 = max(_e145, 0f);
                    let _e2210 = max(_e146, 0f);
                    let _e2211 = (_e2208 * _e2208);
                    let _e2212 = (_e2209 * _e2209);
                    let _e2213 = (_e2210 * _e2210);
                    let _e2217 = max((-0f - _e144), 0f);
                    let _e2218 = max((-0f - _e145), 0f);
                    let _e2219 = max((-0f - _e146), 0f);
                    let _e2220 = (_e2217 * _e2217);
                    let _e2221 = (_e2218 * _e2218);
                    let _e2222 = (_e2219 * _e2219);
                    let _e2313 = clamp(((clamp((_e174.w - abs((_e2152 - _e2157))), 0f, 1f) * _e464) * _e2163), 0f, 1f);
                    let _e2318 = vec2<u32>(cp2077_storage_words_586.words[((((_e488 * 7u)) * 2u) + 0u)], cp2077_storage_words_586.words[((((_e488 * 7u)) * 2u) + 1u)]);
                    let _e2325 = vec2<u32>(cp2077_storage_words_586.words[(((((_e488 * 7u) + 1u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e488 * 7u) + 1u)) * 2u) + 1u)]);
                    let _e2332 = vec2<u32>(cp2077_storage_words_586.words[(((((_e488 * 7u) + 2u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e488 * 7u) + 2u)) * 2u) + 1u)]);
                    let _e2339 = vec2<u32>(cp2077_storage_words_586.words[(((((_e488 * 7u) + 3u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e488 * 7u) + 3u)) * 2u) + 1u)]);
                    let _e2346 = vec2<u32>(cp2077_storage_words_586.words[(((((_e488 * 7u) + 4u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e488 * 7u) + 4u)) * 2u) + 1u)]);
                    let _e2353 = vec2<u32>(cp2077_storage_words_586.words[(((((_e488 * 7u) + 5u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e488 * 7u) + 5u)) * 2u) + 1u)]);
                    let _e2449 = clamp(((clamp((_e174.w - abs((_e2153 - _e2157))), 0f, 1f) * _e466) * _e2163), 0f, 1f);
                    let _e2454 = vec2<u32>(cp2077_storage_words_586.words[((((_e489 * 7u)) * 2u) + 0u)], cp2077_storage_words_586.words[((((_e489 * 7u)) * 2u) + 1u)]);
                    let _e2461 = vec2<u32>(cp2077_storage_words_586.words[(((((_e489 * 7u) + 1u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e489 * 7u) + 1u)) * 2u) + 1u)]);
                    let _e2468 = vec2<u32>(cp2077_storage_words_586.words[(((((_e489 * 7u) + 2u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e489 * 7u) + 2u)) * 2u) + 1u)]);
                    let _e2475 = vec2<u32>(cp2077_storage_words_586.words[(((((_e489 * 7u) + 3u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e489 * 7u) + 3u)) * 2u) + 1u)]);
                    let _e2482 = vec2<u32>(cp2077_storage_words_586.words[(((((_e489 * 7u) + 4u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e489 * 7u) + 4u)) * 2u) + 1u)]);
                    let _e2489 = vec2<u32>(cp2077_storage_words_586.words[(((((_e489 * 7u) + 5u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e489 * 7u) + 5u)) * 2u) + 1u)]);
                    let _e2585 = clamp(((clamp((_e174.w - abs((_e2154 - _e2157))), 0f, 1f) * _e468) * _e2163), 0f, 1f);
                    let _e2590 = vec2<u32>(cp2077_storage_words_586.words[((((_e490 * 7u)) * 2u) + 0u)], cp2077_storage_words_586.words[((((_e490 * 7u)) * 2u) + 1u)]);
                    let _e2597 = vec2<u32>(cp2077_storage_words_586.words[(((((_e490 * 7u) + 1u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e490 * 7u) + 1u)) * 2u) + 1u)]);
                    let _e2604 = vec2<u32>(cp2077_storage_words_586.words[(((((_e490 * 7u) + 2u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e490 * 7u) + 2u)) * 2u) + 1u)]);
                    let _e2611 = vec2<u32>(cp2077_storage_words_586.words[(((((_e490 * 7u) + 3u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e490 * 7u) + 3u)) * 2u) + 1u)]);
                    let _e2618 = vec2<u32>(cp2077_storage_words_586.words[(((((_e490 * 7u) + 4u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e490 * 7u) + 4u)) * 2u) + 1u)]);
                    let _e2625 = vec2<u32>(cp2077_storage_words_586.words[(((((_e490 * 7u) + 5u)) * 2u) + 0u)], cp2077_storage_words_586.words[(((((_e490 * 7u) + 5u)) * 2u) + 1u)]);
                    let _e2715 = (1f / ((((_e2165 + 0.0001f) + _e2313) + _e2449) + _e2585));
                    phi_2580_ = (((((((((((unpack2x16float(_e2325.x).x * _e2220) + (unpack2x16float(_e2318.x).x * _e2211)) + (unpack2x16float(_e2332.x).x * _e2212)) + (unpack2x16float(_e2339.x).x * _e2221)) + (unpack2x16float(_e2346.x).x * _e2213)) + (unpack2x16float(_e2353.x).x * _e2222)) * _e2313) + (((((((unpack2x16float(_e2177.x).x * _e2220) + (unpack2x16float(_e2170.x).x * _e2211)) + (unpack2x16float(_e2184.x).x * _e2212)) + (unpack2x16float(_e2191.x).x * _e2221)) + (unpack2x16float(_e2198.x).x * _e2213)) + (unpack2x16float(_e2205.x).x * _e2222)) * _e2165)) + (((((((unpack2x16float(_e2461.x).x * _e2220) + (unpack2x16float(_e2454.x).x * _e2211)) + (unpack2x16float(_e2468.x).x * _e2212)) + (unpack2x16float(_e2475.x).x * _e2221)) + (unpack2x16float(_e2482.x).x * _e2213)) + (unpack2x16float(_e2489.x).x * _e2222)) * _e2449)) + (((((((unpack2x16float(_e2597.x).x * _e2220) + (unpack2x16float(_e2590.x).x * _e2211)) + (unpack2x16float(_e2604.x).x * _e2212)) + (unpack2x16float(_e2611.x).x * _e2221)) + (unpack2x16float(_e2618.x).x * _e2213)) + (unpack2x16float(_e2625.x).x * _e2222)) * _e2585)) * _e2715);
                    phi_2583_ = (((((((((((unpack2x16float(_e2325.y).x * _e2220) + (unpack2x16float(_e2318.y).x * _e2211)) + (unpack2x16float(_e2332.y).x * _e2212)) + (unpack2x16float(_e2339.y).x * _e2221)) + (unpack2x16float(_e2346.y).x * _e2213)) + (unpack2x16float(_e2353.y).x * _e2222)) * _e2313) + (((((((unpack2x16float(_e2177.y).x * _e2220) + (unpack2x16float(_e2170.y).x * _e2211)) + (unpack2x16float(_e2184.y).x * _e2212)) + (unpack2x16float(_e2191.y).x * _e2221)) + (unpack2x16float(_e2198.y).x * _e2213)) + (unpack2x16float(_e2205.y).x * _e2222)) * _e2165)) + (((((((unpack2x16float(_e2461.y).x * _e2220) + (unpack2x16float(_e2454.y).x * _e2211)) + (unpack2x16float(_e2468.y).x * _e2212)) + (unpack2x16float(_e2475.y).x * _e2221)) + (unpack2x16float(_e2482.y).x * _e2213)) + (unpack2x16float(_e2489.y).x * _e2222)) * _e2449)) + (((((((unpack2x16float(_e2597.y).x * _e2220) + (unpack2x16float(_e2590.y).x * _e2211)) + (unpack2x16float(_e2604.y).x * _e2212)) + (unpack2x16float(_e2611.y).x * _e2221)) + (unpack2x16float(_e2618.y).x * _e2213)) + (unpack2x16float(_e2625.y).x * _e2222)) * _e2585)) * _e2715);
                    phi_2586_ = (((((((((((unpack2x16float((_e2325.x >> bitcast<u32>(16u))).x * _e2220) + (unpack2x16float((_e2318.x >> bitcast<u32>(16u))).x * _e2211)) + (unpack2x16float((_e2332.x >> bitcast<u32>(16u))).x * _e2212)) + (unpack2x16float((_e2339.x >> bitcast<u32>(16u))).x * _e2221)) + (unpack2x16float((_e2346.x >> bitcast<u32>(16u))).x * _e2213)) + (unpack2x16float((_e2353.x >> bitcast<u32>(16u))).x * _e2222)) * _e2313) + (((((((unpack2x16float((_e2177.x >> bitcast<u32>(16u))).x * _e2220) + (unpack2x16float((_e2170.x >> bitcast<u32>(16u))).x * _e2211)) + (unpack2x16float((_e2184.x >> bitcast<u32>(16u))).x * _e2212)) + (unpack2x16float((_e2191.x >> bitcast<u32>(16u))).x * _e2221)) + (unpack2x16float((_e2198.x >> bitcast<u32>(16u))).x * _e2213)) + (unpack2x16float((_e2205.x >> bitcast<u32>(16u))).x * _e2222)) * _e2165)) + (((((((unpack2x16float((_e2461.x >> bitcast<u32>(16u))).x * _e2220) + (unpack2x16float((_e2454.x >> bitcast<u32>(16u))).x * _e2211)) + (unpack2x16float((_e2468.x >> bitcast<u32>(16u))).x * _e2212)) + (unpack2x16float((_e2475.x >> bitcast<u32>(16u))).x * _e2221)) + (unpack2x16float((_e2482.x >> bitcast<u32>(16u))).x * _e2213)) + (unpack2x16float((_e2489.x >> bitcast<u32>(16u))).x * _e2222)) * _e2449)) + (((((((unpack2x16float((_e2597.x >> bitcast<u32>(16u))).x * _e2220) + (unpack2x16float((_e2590.x >> bitcast<u32>(16u))).x * _e2211)) + (unpack2x16float((_e2604.x >> bitcast<u32>(16u))).x * _e2212)) + (unpack2x16float((_e2611.x >> bitcast<u32>(16u))).x * _e2221)) + (unpack2x16float((_e2618.x >> bitcast<u32>(16u))).x * _e2213)) + (unpack2x16float((_e2625.x >> bitcast<u32>(16u))).x * _e2222)) * _e2585)) * _e2715);
                } else {
                    let _e2719 = f32(_e169.x);
                    let _e2725 = ((min(max((f32(_e460) * 4f), 0f), _e2719) / _e2719) * 255f);
                    phi_2580_ = _e2725;
                    phi_2583_ = 0f;
                    phi_2586_ = (255f - _e2725);
                }
                let _e2728 = phi_2580_;
                let _e2730 = phi_2583_;
                let _e2732 = phi_2586_;
                phi_2581_ = _e2728;
                phi_2584_ = _e2730;
                phi_2587_ = _e2732;
            } else {
                phi_2579_ = 0f;
                phi_2582_ = 0f;
                phi_2585_ = 0f;
                if (_e163.y == 0u) {
                } else {
                    let _e2741 = ((fract((((floor(_e130) + floor(_e129)) + floor(_e131)) * 0.5f)) * 2f) != 0f);
                    phi_2579_ = select(255f, 128f, _e2741);
                    phi_2582_ = select(0f, 32f, _e2741);
                    phi_2585_ = select(255f, 64f, _e2741);
                }
                let _e2746 = phi_2579_;
                let _e2748 = phi_2582_;
                let _e2750 = phi_2585_;
                phi_2581_ = _e2746;
                phi_2584_ = _e2748;
                phi_2587_ = _e2750;
            }
            let _e2752 = phi_2581_;
            let _e2754 = phi_2584_;
            let _e2756 = phi_2587_;
            phi_300_ = _e2752;
            phi_304_ = _e2756;
            phi_308_ = _e2754;
        }
        let _e2758 = phi_300_;
        let _e2760 = phi_304_;
        let _e2762 = phi_308_;
        SV_Target[0u] = ((_e2758 * 64f) * _e181.z);
        SV_Target[1u] = ((_e2760 * 64f) * _e181.z);
        SV_Target[2u] = ((_e2762 * 64f) * _e181.z);
    } else {
        if (_e159 == 1u) {
            SV_Target[0u] = 0f;
            SV_Target[1u] = 255f;
            SV_Target[2u] = 0f;
        }
        if (_e159 == 2u) {
            SV_Target[0u] = 128f;
            SV_Target[1u] = 0f;
            SV_Target[2u] = 0f;
        } else {
            if (_e159 == 3u) {
                SV_Target[0u] = 128f;
                SV_Target[1u] = 0f;
                SV_Target[2u] = 128f;
            } else {
                if (_e159 == 4u) {
                    SV_Target[0u] = 64f;
                    SV_Target[1u] = 0f;
                    SV_Target[2u] = 128f;
                } else {
                    SV_Target[0u] = 0f;
                    SV_Target[1u] = 128f;
                    SV_Target[2u] = 0f;
                }
            }
        }
    }
    return;
}

@fragment 
fn main(@builtin(position) SV_Position: vec4<f32>) -> @location(0) vec4<f32> {
    SV_Position_1 = SV_Position;
    main_1();
    let _e3 = SV_Target;
    return vec4<f32>(_e3, 0f);
}
)CPGIRGB";
static const char kGiRGBOutputHash[]="6437b84bbfc22f70dc3329e6c37e739de103de61de0a41a97123c794507e9cbb";
