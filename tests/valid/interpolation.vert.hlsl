struct VSOut
{
    float4 pos : SV_Position;
    nointerpolation float3 flat_color : COLOR0;
    noperspective float2 screen_uv : TEXCOORD0;
};
VSOut main(float3 p : POSITION)
{
    VSOut o;
    o.pos = float4(p, 1);
    o.flat_color = p;
    o.screen_uv = p.xy;
    return o;
}
