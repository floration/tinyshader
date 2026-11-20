float4 main(float2 uv : TEXCOORD0) : SV_Target { float a = 0; for (int i = 0; i < 8; i++) { if (i == 3) continue; a += sin(uv.x * i); } return float4(a,a,a,1); }
