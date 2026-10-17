// AMD FidelityFX Contrast Adaptive Sharpening (CAS), ported to Dolphin's post-processing
// shader format for Colosseum Native. Sharpens and resizes in one pass (the game image is
// usually rendered above window resolution, so this also gives a clean downscale).
//
// Port of ffx_cas.h (CasFilter, 32-bit scaling path) with CAS_BETTER_DIAGONALS,
// CAS_SLOW (per-channel weights) and CAS_GO_SLOWER (exact rcp/sqrt).
//
// Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved.
// Permission is hereby granted, free of charge, to any person obtaining a copy of this
// software and associated documentation files (the "Software"), to deal in the Software
// without restriction, including without limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
// to whom the Software is furnished to do so, subject to the following conditions:
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
// INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
// PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE
// FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

/*
[configuration]

[OptionRangeFloat]
GUIName = Sharpness
OptionName = CAS_SHARPNESS
MinValue = 0.0
MaxValue = 1.0
StepAmount = 0.05
DefaultValue = 0.5

[/configuration]
*/

float3 CasLoad(int2 p)
{
  int2 size = int2(GetResolution());
  p = clamp(p, int2(0, 0), size - int2(1, 1));
  return texelFetch(samp1, int3(p, int(GetLayer())), 0).rgb;
}

float3 Min3(float3 a, float3 b, float3 c) { return min(min(a, b), c); }
float3 Max3(float3 a, float3 b, float3 c) { return max(max(a, b), c); }

// Amplitude of the local contrast window (CAS "amp"), per channel, better diagonals.
float3 CasAmp(float3 mn, float3 mx)
{
  // max() guards pure black, where AMD relies on GPU rcp(0) behaviour.
  return sqrt(clamp(min(mn, float3(2.0, 2.0, 2.0) - mx) / max(mx, float3(1e-5, 1e-5, 1e-5)), 0.0, 1.0));
}

void main()
{
  // CasSetup: sharpness 0..1 maps to a negative lobe of -1/8 .. -1/5.
  float peak = -1.0 / mix(8.0, 5.0, clamp(GetOption(CAS_SHARPNESS), 0.0, 1.0));

  // Position of the output pixel centre in source pixels, minus 0.5 (CasFilter's pp).
  float2 pp = GetCoordinates() * GetResolution() - float2(0.5, 0.5);
  float2 fp = floor(pp);
  pp -= fp;
  int2 sp = int2(fp);

  //  a b c d
  //  e f g h
  //  i j k l
  //  m n o p
  float3 a = CasLoad(sp + int2(-1, -1));
  float3 b = CasLoad(sp + int2(0, -1));
  float3 e = CasLoad(sp + int2(-1, 0));
  float3 f = CasLoad(sp);
  float3 c = CasLoad(sp + int2(1, -1));
  float3 d = CasLoad(sp + int2(2, -1));
  float3 g = CasLoad(sp + int2(1, 0));
  float3 h = CasLoad(sp + int2(2, 0));
  float3 i = CasLoad(sp + int2(-1, 1));
  float3 j = CasLoad(sp + int2(0, 1));
  float3 m = CasLoad(sp + int2(-1, 2));
  float3 n = CasLoad(sp + int2(0, 2));
  float3 k = CasLoad(sp + int2(1, 1));
  float3 l = CasLoad(sp + int2(2, 1));
  float3 o = CasLoad(sp + int2(1, 2));
  float3 p = CasLoad(sp + int2(2, 2));

  // Soft min/max around each of the four nearest pixels (cross + diagonals).
  float3 mnf = Min3(Min3(b, e, f), g, j);
  mnf += Min3(Min3(mnf, a, c), i, k);
  float3 mxf = Max3(Max3(b, e, f), g, j);
  mxf += Max3(Max3(mxf, a, c), i, k);
  float3 mng = Min3(Min3(c, f, g), h, k);
  mng += Min3(Min3(mng, b, d), j, l);
  float3 mxg = Max3(Max3(c, f, g), h, k);
  mxg += Max3(Max3(mxg, b, d), j, l);
  float3 mnj = Min3(Min3(f, i, j), k, n);
  mnj += Min3(Min3(mnj, e, g), m, o);
  float3 mxj = Max3(Max3(f, i, j), k, n);
  mxj += Max3(Max3(mxj, e, g), m, o);
  float3 mnk = Min3(Min3(g, j, k), l, o);
  mnk += Min3(Min3(mnk, f, h), n, p);
  float3 mxk = Max3(Max3(g, j, k), l, o);
  mxk += Max3(Max3(mxk, f, h), n, p);

  float3 wf = CasAmp(mnf, mxf) * peak;
  float3 wg = CasAmp(mng, mxg) * peak;
  float3 wj = CasAmp(mnj, mxj) * peak;
  float3 wk = CasAmp(mnk, mxk) * peak;

  // Bilinear weights, scaled down where the local contrast is high (thin-feature boost).
  const float thinB = 1.0 / 32.0;
  float s = (1.0 - pp.x) * (1.0 - pp.y) / (thinB + (mxf.g - mnf.g));
  float t = pp.x * (1.0 - pp.y) / (thinB + (mxg.g - mng.g));
  float u = (1.0 - pp.x) * pp.y / (thinB + (mxj.g - mnj.g));
  float v = pp.x * pp.y / (thinB + (mxk.g - mnk.g));

  float3 qbe = wf * s;
  float3 qch = wg * t;
  float3 qf = wg * t + wj * u + s;
  float3 qg = wf * s + wk * v + t;
  float3 qj = wf * s + wk * v + u;
  float3 qk = wg * t + wj * u + v;
  float3 qin = wj * u;
  float3 qlo = wk * v;

  float3 rcpW = 1.0 / (2.0 * qbe + 2.0 * qch + 2.0 * qin + 2.0 * qlo + qf + qg + qj + qk);
  float3 color = (b * qbe + e * qbe + c * qch + h * qch + i * qin + n * qin + l * qlo +
                  o * qlo + f * qf + g * qg + j * qj + k * qk) * rcpW;

  SetOutput(float4(clamp(color, 0.0, 1.0), 1.0));
}
