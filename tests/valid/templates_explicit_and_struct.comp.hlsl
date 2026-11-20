// tinyshader-only: accepts ">>" closing nested template arguments (DXC requires "> >")
template<typename T> struct Pair { T first; T second; };
template<typename K, typename V> struct Entry { K key; V value; };
template<typename T> struct Box { T item; float weight; };

template<typename T> T maxOf(T a, T b) { return a > b ? a : b; }
template<typename T> T convert(float x) { return (T)x; }
template<typename T> T sumPair(Pair<T> p) { return p.first + p.second; }
template<typename T, typename U> T mix2(T a, U b) { return a + (T)b; }

RWStructuredBuffer<float4> outb;
StructuredBuffer<Entry<uint, float3> > table;

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Pair<float> pf;
    pf.first = 1.0;
    pf.second = 2.0;
    Pair<int> pi = { 3, 4 };
    Box<Pair<float>> nested;
    nested.item = pf;
    nested.weight = 0.5;
    Entry<uint, float3> e = table[id.x];

    float a = maxOf<float>(pf.first, 3);   // explicit T, 3 converts
    int b = convert<int>(2.7);             // T only in the return type
    float c = sumPair(pf);                 // deduced from Pair<T>
    float d = mix2<float>(a, b);           // first explicit, second deduced
    outb[id.x] = float4(e.value * a, c + d + (float)(pi.first + pi.second + b) + nested.item.second * nested.weight + (float)e.key);
}
