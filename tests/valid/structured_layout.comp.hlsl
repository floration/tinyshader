struct Particle { float3 pos; float life; float2 uv; float mass; float3 vel; float4x4 xf; float w[2]; };
struct Small { float a; float b; };
RWStructuredBuffer<Particle> particles;
StructuredBuffer<Small> smalls;
StructuredBuffer<float3> points;
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    Particle p = particles[id.x];
    Small s = smalls[id.x];
    p.pos += p.vel * s.a + points[id.x] * s.b;
    p.life -= p.w[0] + p.w[1] + p.mass + p.uv.x + p.xf[0][0];
    particles[id.x] = p;
}
