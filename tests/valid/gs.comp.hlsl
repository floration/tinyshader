groupshared float sh[64]; RWStructuredBuffer<float> o; [numthreads(64,1,1)] void main(uint i : SV_GroupIndex) { sh[i] = i; GroupMemoryBarrierWithGroupSync(); o[i] = sh[63-i]; }
