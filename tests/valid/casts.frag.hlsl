struct Light { float3 dir; float power; float w[2]; };
float4 main(float4 v : V, int m : M, float x : X) : SV_Target
{
    float a = (float)m;
    int b = (int)x;
    float3 s = (float3)x;
    float2 t = (float2)v;
    float f = (float)v;
    uint u = (uint)(x * 2);
    bool flag = (bool)m;
    float fb = (float)flag;
    int3 i3 = (int3)v.xyz;
    Light l = (Light)0;
    float p = (a) - b;
    return float4(s + float3(t, f) + (float3)i3 + l.dir, a + b + u + fb + l.power + p + l.w[1]);
}
