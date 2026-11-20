struct PSIn
{
    float4 pos : SV_Position;
    nointerpolation float3 flat_color : COLOR0;
    noperspective float2 screen_uv : TEXCOORD0;
    centroid float4 tint : COLOR1;
    linear float weight : TEXCOORD1;
    uint id : TEXCOORD2;
};
float4 main(PSIn i, sample float2 msaa_uv : TEXCOORD3) : SV_Target
{
    float w2 = i.weight * 2;
    return float4(i.flat_color, 1) * i.tint + float4(i.screen_uv + msaa_uv, w2, (float)i.id);
}
