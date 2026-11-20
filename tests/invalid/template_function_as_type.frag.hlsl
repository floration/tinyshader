template<typename T> T id(T x) { return x; }
float4 main() : SV_Target { id<float> v; return 0; }
