# Preserve DXBC indexable temporary arrays

On installed VIOGPU58474/Mesa146ce465, the ordinary Microsoft D3D11 runtime
creates an FL10_1 hardware device, but the following legal SM4 vertex shader
leaves all4096 pixels at the blue clear color:

```hlsl
float4 vs(uint i : SV_VertexID) : SV_Position {
    float2 p[3] = {float2(-1,-1), float2(-1,3), float2(3,-1)};
    return float4(p[i],0,1);
}
```

Actual target shader trace `flat58474-draw-shaders-1` contains
`MOV OUT[0].xy, TEMP[TEMP[0].x+1].xyxx`, but the optimized NIR writes the
constant position(-1,-1,0,1) for every vertex. Host submissions retire normally;
this is a shader translation error, not an observed GPU timeout. Exact system
runtime test source is gunyah-guest-drivers-windows1d4e5e4bf60aaf59008ab6be9729ec6bc2dbbd30,
CI34726860051; its three architecture builds and WARP harness tests pass.

The frontend previously declared DXBC indexable temporaries as individual
TGSI temporary registers. `ttn_emit_declaration` only creates addressable NIR
arrays for TGSI declarations with the Array flag; `ttn_src_for_file_and_index`
requires no indirect operand on scalar registers. With assertions disabled,
the original indirect lookup silently became a direct register load.

Declare each DXBC indexable temporary through `ureg_DECL_array_temporary` and
retain its ArrayID on each consecutive element. Existing source/destination
translation then preserves both direct and indirect reads/writes. Ordinary
non-indexable temporaries remain scalar registers. No device capability or
feature-level change accompanies this repair.

Validation gate: rebuild the UMD with this source, package/sign it together
with the KMD, then repeat the unchanged ordinary D3D11 test. Acceptance remains
all16384 exact pixels and four successful Presents; visible host display pixels
are a separate unverified gate. Build success alone does not close this bug.
