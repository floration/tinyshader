struct Light { float3 dir; float power; };
cbuffer Frame : register(b0)
{
    float4x4 view_proj;
    float3   eye;
    float    time;
    float2   jitter;
    float    exposure;
    float2   crosses;
    Light    sun;
    float    weights[3];
    float3   tint;
    int      mode;
    float4   extra[2];
};
float4 main(float2 uv : TEXCOORD0) : SV_Target
{
    float4 p = mul(view_proj, float4(eye, time));
    float w = weights[0] + weights[1] + weights[2];
    return p + float4(jitter, exposure, crosses.x + crosses.y) + float4(sun.dir * sun.power, w)
             + float4(tint, float(mode)) + extra[0] + extra[1];
}
