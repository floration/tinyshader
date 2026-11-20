struct Small { float a; float b; };
cbuffer C : register(b0)
{
    float  f0;
    float2 v2;
    float  f1;
    Small  s;
    float  after_struct;
    float3 v3;
    float  f2;
    float  f3;
    float2 v2b;
};
float4 main() : SV_Target
{
    return float4(f0 + v2.x + v2.y + f1, s.a + s.b + after_struct, v3.x + f2 + f3, v2b.x + v2b.y + v3.y + v3.z);
}
