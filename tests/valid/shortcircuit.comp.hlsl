struct Hit { float t; uint id; };
RWStructuredBuffer<uint> counters;
StructuredBuffer<float> data;
RWStructuredBuffer<float> outb;

bool bump(uint i) { counters[i] += 1; return true; }

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint n = 100;
    uint i = tid.x;

    // Bounds guard: data[i] must not be read when i >= n
    float v = (i < n && data[i] > 0.5) ? data[i] : 0.0;

    // Right side has a side effect and must run only when needed
    bool a = (v > 1.0) || bump(0);
    bool b = (v < 0.0) && bump(1);

    // Short-circuit inside loop conditions
    uint k = 0;
    while (k < n && data[k] != 0.0) { k++; }
    for (uint j = 0; j < 4 && (j < k || bump(2)); j++) { v += 1.0; }
    uint m = 0;
    do { m++; } while (m < 3 && (data[m] > 0.0 || m == 1));

    // Ternary with struct result and nested logic
    Hit h0 = { 1.0, 1 };
    Hit h1 = { 2.0, 2 };
    Hit h = h1;
    if (a && !b) h = h0;
    float pickt = (a && !b) ? h0.t : h1.t;

    outb[i] = v + pickt + h.t + (float)h.id + (float)k + (float)m;
}
