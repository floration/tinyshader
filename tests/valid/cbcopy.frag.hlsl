cbuffer K { float w[3]; };
float4 main(float x : X) : SV_Target { float local[3] = w; return local[0] + local[2] * x; }
