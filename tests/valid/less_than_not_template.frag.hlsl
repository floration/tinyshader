float4 main(float a : A, float b : B) : SV_Target { bool c = a < b; float d = a<b ? a : b; return c ? d : 0; }
