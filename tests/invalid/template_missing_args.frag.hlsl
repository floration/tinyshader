template<typename T> struct Pair { T a; T b; };
float4 main() : SV_Target { Pair p; p.a = 1; return p.a; }
