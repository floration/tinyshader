float4 main(uint m : M, float x : X) : SV_Target { float4 c = 0;
  for (int i = 0; i < 4; i++) {
    switch (m + i) {
      case 0: c.x += 1; // falls through
      case 1: c.y += x; break;
      case 2: case 3: c.z = 2; break;
      case 7: continue;
      default: c.w = 1;
    }
  }
  switch (m) { case 5: return c * 2; default: break; }
  return c; }
