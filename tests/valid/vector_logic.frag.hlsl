float4 main(float4 v : V, float3 n : NORMAL, int3 k : K) : SV_Target
{
    bool4 pos = v > 0;
    bool3 eq = n == float3(0, 1, 0);
    float3 mask = (float3)(n < 0.5);
    float4 sel = select(pos, v, -v);
    float3 lit = select(n > 0, 1.0, 0.0);
    bool anyneg = any(v < 0);
    bool allpos = all(pos);
    bool3 bad = or(isnan(n), isinf(n));
    bool samek = all(k == int3(1, 2, 3));
    half h = 0.5;
    half3 hv = n * h;
    float r = anyneg ? 1.0 : 0.0;
    if (any(bad) || !allpos) r += 2;
    if (all(eq != bool3(true, false, true))) r += 4;
    if (samek && any(k)) r += 8;
    return sel * r + float4(mask + lit + hv, 0);
}
