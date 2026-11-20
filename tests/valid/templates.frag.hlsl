template<typename T> T twice(T x) { return x + x; }
template<typename T> T sq(T x) { return x * x; }
template<typename T> T sum3(T a, T b, T c) { return twice(a) + b + sq(c); }
template<typename T, typename U> T scaled(T v, U s) { return v * (T)s; }
template<typename T> T pick(bool c, T a, T b) { if (c) { return a; } return b; }

float4 main(float x : X, float3 n : NORMAL, int k : K) : SV_Target
{
    float a = twice(x);
    float3 b = twice(n);
    int c = twice(k);
    float3 d = sum3(n, n, n);
    float3 e = scaled(n, k);
    float f = pick(x > 0.5, a, x);
    return float4(b + d + e, a + float(c) + f + twice(x));
}
