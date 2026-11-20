Texture2D tex; SamplerState s;
float4 main(float2 uv : TEXCOORD0) : SV_Target { return tex.Sample(s, uv) * 2.0; }
