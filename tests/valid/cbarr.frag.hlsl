cbuffer Lights { float4 colors[4]; float intensity[3]; float3 ambient; };
float4 main(float2 uv : TEXCOORD0) : SV_Target { float4 c = colors[1] * intensity[2]; return c + float4(ambient, 0); }
