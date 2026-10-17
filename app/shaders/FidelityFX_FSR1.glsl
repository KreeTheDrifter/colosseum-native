// AMD FidelityFX Super Resolution 1.0 (EASU upscaling + RCAS sharpening), ported to
// Dolphin's post-processing shader format for Colosseum Native.
//
// Use it when the internal resolution is below the window/screen resolution (for example
// 2x internal on a 4K screen, or on phones). At or above screen resolution use CAS instead.
//
// Dolphin runs one post-processing pass, while FSR 1 is two (EASU, then RCAS on EASU's
// output). With sharpening on, this shader evaluates EASU at the output pixel and its four
// neighbours and applies RCAS to those, which gives the two-pass result at ~5x the EASU
// cost. Ported from ffx_fsr1.h (FsrEasuF, FsrRcasF with FSR_RCAS_DENOISE), 32-bit path.
//
// Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
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

[OptionBool]
GUIName = Sharpening (RCAS)
OptionName = FSR_SHARPEN
DefaultValue = true

[OptionRangeFloat]
GUIName = Sharpness reduction (stops, 0 = strongest)
OptionName = FSR_RCAS_STOPS
MinValue = 0.0
MaxValue = 2.0
StepAmount = 0.05
DefaultValue = 0.25
DependentOption = FSR_SHARPEN

[/configuration]
*/

float3 FsrLoad(int2 p)
{
  int2 size = int2(GetResolution());
  p = clamp(p, int2(0, 0), size - int2(1, 1));
  return texelFetch(samp1, int3(p, int(GetLayer())), 0).rgb;
}

// Luma times 2, as in FSR.
float FsrLuma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void FsrEasuSet(inout float2 dir, inout float len, float w, float lA, float lB, float lC,
                float lD, float lE)
{
  float dc = lD - lC;
  float cb = lC - lB;
  float lenX = max(abs(dc), abs(cb));
  lenX = 1.0 / max(lenX, 1e-8);
  float dirX = lD - lB;
  dir.x += dirX * w;
  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
  lenX *= lenX;
  len += lenX * w;

  float ec = lE - lC;
  float ca = lC - lA;
  float lenY = max(abs(ec), abs(ca));
  lenY = 1.0 / max(lenY, 1e-8);
  float dirY = lE - lA;
  dir.y += dirY * w;
  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
  lenY *= lenY;
  len += lenY * w;
}

void FsrEasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len,
                float lob, float clp, float3 c)
{
  float2 v;
  v.x = (off.x * (dir.x)) + (off.y * dir.y);
  v.y = (off.x * (-dir.y)) + (off.y * dir.x);
  v *= len;
  float d2 = v.x * v.x + v.y * v.y;
  d2 = min(d2, clp);
  float wB = (2.0 / 5.0) * d2 - 1.0;
  float wA = lob * d2 - 1.0;
  wB *= wB;
  wA *= wA;
  wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
  float w = wB * wA;
  aC += c * w;
  aW += w;
}

// EASU at a position given in source pixels (pixel centres at +0.5).
float3 FsrEasu(float2 srcPos)
{
  float2 pp = srcPos - float2(0.5, 0.5);
  float2 fp = floor(pp);
  pp -= fp;
  int2 sp = int2(fp);

  //    b c
  //  e f g h
  //  i j k l
  //    n o
  float3 b = FsrLoad(sp + int2(0, -1));
  float3 c = FsrLoad(sp + int2(1, -1));
  float3 e = FsrLoad(sp + int2(-1, 0));
  float3 f = FsrLoad(sp);
  float3 g = FsrLoad(sp + int2(1, 0));
  float3 h = FsrLoad(sp + int2(2, 0));
  float3 i = FsrLoad(sp + int2(-1, 1));
  float3 j = FsrLoad(sp + int2(0, 1));
  float3 k = FsrLoad(sp + int2(1, 1));
  float3 l = FsrLoad(sp + int2(2, 1));
  float3 n = FsrLoad(sp + int2(0, 2));
  float3 o = FsrLoad(sp + int2(1, 2));

  float bL = FsrLuma(b), cL = FsrLuma(c), eL = FsrLuma(e), fL = FsrLuma(f);
  float gL = FsrLuma(g), hL = FsrLuma(h), iL = FsrLuma(i), jL = FsrLuma(j);
  float kL = FsrLuma(k), lL = FsrLuma(l), nL = FsrLuma(n), oL = FsrLuma(o);

  float2 dir = float2(0.0, 0.0);
  float len = 0.0;
  FsrEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
  FsrEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
  FsrEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  FsrEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

  float2 dir2 = dir * dir;
  float dirR = dir2.x + dir2.y;
  bool zro = dirR < (1.0 / 32768.0);
  dirR = inversesqrt(max(dirR, 1e-12));
  dirR = zro ? 1.0 : dirR;
  dir.x = zro ? 1.0 : dir.x;
  dir *= float2(dirR, dirR);

  len = len * 0.5;
  len *= len;
  float stretch = (dir.x * dir.x + dir.y * dir.y) / max(max(abs(dir.x), abs(dir.y)), 1e-8);
  float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
  float clp = 1.0 / lob;

  float3 min4 = min(min(f, g), min(j, k));
  float3 max4 = max(max(f, g), max(j, k));

  float3 aC = float3(0.0, 0.0, 0.0);
  float aW = 0.0;
  FsrEasuTap(aC, aW, float2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
  FsrEasuTap(aC, aW, float2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
  FsrEasuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
  FsrEasuTap(aC, aW, float2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
  FsrEasuTap(aC, aW, float2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
  FsrEasuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
  FsrEasuTap(aC, aW, float2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
  FsrEasuTap(aC, aW, float2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
  FsrEasuTap(aC, aW, float2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
  FsrEasuTap(aC, aW, float2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
  FsrEasuTap(aC, aW, float2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
  FsrEasuTap(aC, aW, float2(0.0, 2.0) - pp, dir, len2, lob, clp, n);

  return min(max4, max(min4, aC / aW));
}

#define FSR_RCAS_LIMIT (0.25 - (1.0 / 16.0))

void main()
{
  float2 src = GetCoordinates() * GetResolution();
  float3 e = FsrEasu(src);

  if (GetOption(FSR_SHARPEN) == 0)
  {
    SetOutput(float4(e, 1.0));
    return;
  }

  // One output pixel, in source pixels (screen-space derivative of the source position).
  float2 step = float2(abs(dFdx(src.x)), abs(dFdy(src.y)));
  float3 b = FsrEasu(src - float2(0.0, step.y));
  float3 d = FsrEasu(src - float2(step.x, 0.0));
  float3 f = FsrEasu(src + float2(step.x, 0.0));
  float3 h = FsrEasu(src + float2(0.0, step.y));

  float bL = FsrLuma(b), dL = FsrLuma(d), eL = FsrLuma(e), fL = FsrLuma(f), hL = FsrLuma(h);

  // Noise detection (FSR_RCAS_DENOISE).
  float nz = 0.25 * bL + 0.25 * dL + 0.25 * fL + 0.25 * hL - eL;
  float range = max(max(max(bL, dL), max(eL, fL)), hL) - min(min(min(bL, dL), min(eL, fL)), hL);
  nz = clamp(abs(nz) / max(range, 1e-8), 0.0, 1.0);
  nz = -0.5 * nz + 1.0;

  float3 mn4 = min(min(b, d), min(f, h));
  float3 mx4 = max(max(b, d), max(f, h));
  float2 peakC = float2(1.0, -4.0);
  float3 hitMin = min(mn4, e) / max(4.0 * mx4, float3(1e-8, 1e-8, 1e-8));
  float3 hitMax = (peakC.x - max(mx4, e)) / min(4.0 * mn4 + peakC.y, float3(-1e-8, -1e-8, -1e-8));
  float3 lobeRGB = max(-hitMin, hitMax);
  float sharpness = exp2(-GetOption(FSR_RCAS_STOPS));
  float lobe = max(-FSR_RCAS_LIMIT, min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * sharpness;
  lobe *= nz;

  float3 color = (lobe * b + lobe * d + lobe * h + lobe * f + e) / (4.0 * lobe + 1.0);
  SetOutput(float4(clamp(color, 0.0, 1.0), 1.0));
}
