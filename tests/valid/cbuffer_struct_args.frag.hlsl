struct Light { float3 dir; float power; float2 cone; };
cbuffer L { Light sun; Light lamps[2]; float ambient; };
struct PC { float a; float2 b; float3 c; float d; };
[[vk::push_constant]] PC pc;
float3 shade(Light l, float3 n) { return n * saturate(dot(n, -l.dir)) * l.power; }
float4 main(float3 n : NORMAL) : SV_Target
{
    Light copy = lamps[1];
    float3 c = shade(sun, n) + shade(copy, n) + shade(lamps[0], n);
    return float4(c * ambient + pc.c * pc.a + float3(pc.b, pc.d), 1);
}
