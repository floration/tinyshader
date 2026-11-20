struct M { float3x3 normal_mat; float s; float4x3 a; float3x4 b; float2x2 c; float t; };
cbuffer C { float3x3 cn; float k; float4x3 ca; float3x4 cb; float2x2 cc; float2 kk; };
StructuredBuffer<M> ms;
RWStructuredBuffer<float4> outb;
[numthreads(1,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    M m = ms[id.x];
    float3 v = mul(m.normal_mat, float3(1, 2, 3)) * m.s + mul(cn, float3(1, 1, 1)) * k;
    float3 x = mul(m.a, float3(1, 0, 0)).xyz + mul(ca, float3(0, 1, 0)).xyz;
    float3 y = mul(m.b, float4(1, 0, 0, 1)) + mul(cb, float4(0, 1, 0, 1));
    float2 z = mul(m.c, float2(1, 1)) + mul(cc, kk) * m.t;
    outb[id.x] = float4(v + x + y, z.x + z.y);
}
