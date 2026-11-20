float4 main(int m : M) : SV_Target { float4 c; switch (m) { case 0: c = 1; break; default: c = 0; break; } return c; }
