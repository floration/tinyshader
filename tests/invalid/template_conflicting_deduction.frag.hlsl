template<typename T> T f(T a, T b) { return a + b; }
float4 main(float x : X, int k : K) : SV_Target { return f(x, k); }
