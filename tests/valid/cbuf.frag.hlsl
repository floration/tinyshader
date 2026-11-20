cbuffer Cam : register(b0) { float4x4 vp; float3 eye; float t; };
float4 main(float4 p : SV_Position) : SV_Target { return mul(vp, float4(eye*t,1)); }
