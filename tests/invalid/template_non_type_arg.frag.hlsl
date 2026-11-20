template<typename T> struct Pair { T a; T b; };
float4 main() : SV_Target { Pair<3> p; return 0; }
