template<typename T, typename U> T g(T a) { return a; }
float4 main(float x : X) : SV_Target { return g(x); }
