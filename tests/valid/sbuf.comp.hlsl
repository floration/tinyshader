RWStructuredBuffer<float> buf; [numthreads(64,1,1)] void main(uint3 id : SV_DispatchThreadID) { buf[id.x] = buf[id.x] * 2.0 + 1.0; }
