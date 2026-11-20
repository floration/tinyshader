struct L { float3 dir; float3 col; }; float3 shade(L l, float3 n) { return l.col * saturate(dot(n, -l.dir)); }
float4 main(float3 n : NORMAL) : SV_Target { L l; l.dir = float3(0,-1,0); l.col = 1; return float4(shade(l, normalize(n)), 1); }
