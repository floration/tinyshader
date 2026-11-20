float4 main(int m : M, float x : X) : SV_Target { switch (m) { case x: return 1; default: return 0; } }
