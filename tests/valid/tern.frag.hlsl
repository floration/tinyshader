float4 main(float x : X) : SV_Target { float y = x > 0.5 ? 1.0 : 0.0; return lerp(float4(0,0,0,1), float4(1,1,1,1), y); }
