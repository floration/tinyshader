struct Light { float3 dir; float power; };
cbuffer L { Light sun; };
Light getSun() { return sun; }
float4 main() : SV_Target { Light l = getSun(); return float4(l.dir, l.power); }
