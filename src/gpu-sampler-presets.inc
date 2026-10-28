// Generated from original gpuApiSamplerCommon.h; no preset remapping.
static_assert(SAMPSTATEPRESET_Max == 32, "Original sampler table coverage changed");
static bool BuildBrowserSamplerPreset(eSamplerStatePreset preset, SamplerStateDesc& out) {
    switch (preset) {
    case SAMPSTATEPRESET_WrapPointNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapLinearNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapAnisoNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Aniso; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapPointMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_Point;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapLinearMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapAnisoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Aniso; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapLowAnisoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_AnisoLow; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapPointMipLinear:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_WrapLinearMipPoint:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Point;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampPointNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampLinearNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampAnisoNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Aniso; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampPointMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_Point;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampLinearMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampAnisoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Aniso; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorPointNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Mirror;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorLinearNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Mirror;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorAnisoNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Mirror;
        out.filterMin = TEXFILTERMIN_Aniso; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorPointMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Mirror;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_Point;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorLinearMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Mirror;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorAnisoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Mirror;
        out.filterMin = TEXFILTERMIN_Aniso; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorOncePointNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_MirrorOnce;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorOnceLinearNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_MirrorOnce;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorOnceAnisoNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_MirrorOnce;
        out.filterMin = TEXFILTERMIN_Aniso; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorOncePointMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_MirrorOnce;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_Point;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_MirrorOnceLinearMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_MirrorOnce;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_BorderPointNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Border;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_BorderLinearNoMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Border;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_None;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampLinearMipNoBias:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampPointNoMipCompareLess:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Point; out.filterMag = TEXFILTERMAG_Point; out.filterMip = TEXFILTERMIP_Point;
        out.comparisonFunction = COMP_Less; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_ClampLinearNoMipCompareLess:
        out.addressU = out.addressV = out.addressW = TEXADDR_Clamp;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Point;
        out.comparisonFunction = COMP_Less; out.pointZFilter = false;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    case SAMPSTATEPRESET_AtlasLinearMip:
        out.addressU = out.addressV = out.addressW = TEXADDR_Wrap;
        out.filterMin = TEXFILTERMIN_Linear; out.filterMag = TEXFILTERMAG_Linear; out.filterMip = TEXFILTERMIP_Linear;
        out.comparisonFunction = COMP_None; out.pointZFilter = true;
        out.mipLODBias = 0.f; out.allowSettingsBias = false; return true;
    default: return false;
    }
}
