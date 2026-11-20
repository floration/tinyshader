struct S { float a; int b; float2 c; float d[2]; };
float4 main(float x : X) : SV_Target { S s = (S)x; return s.a + s.b + s.c.y + s.d[1]; }
