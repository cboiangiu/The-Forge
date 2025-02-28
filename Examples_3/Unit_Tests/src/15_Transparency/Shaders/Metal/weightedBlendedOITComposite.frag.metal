/*
 * Copyright (c) 2018-2021 The Forge Interactive Inc.
 *
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
*/

/* Write your header comments here */
#include <metal_stdlib>
using namespace metal;

inline void clip(float x) {
    if (x < 0.0) discard_fragment();
}
struct Fragment_Shader
{
    struct VSOutput
    {
        float4 Position [[position]];
        float4 UV;
    };
    sampler PointSampler;
    texture2d<float> AccumulationTexture;
    texture2d<float> RevealageTexture;
    float MaxComponent(    float4 v)
    {
        return max(max(max(v.x, v.y), v.z), v.w);
    };
    float4 main(    VSOutput input)
    {
        float revealage = RevealageTexture.sample(PointSampler, input.UV.xy).r;
        clip(((1.0 - revealage) - 0.000010000000));
        float4 accumulation = AccumulationTexture.sample(PointSampler, input.UV.xy);
        if (isinf(MaxComponent(abs(accumulation))))
        {
            (accumulation.rgb = (float3)(accumulation.a));
        }
        float3 averageColor = (accumulation.rgb / (float3)(max(accumulation.a, 0.000010000000)));
        return float4(averageColor, (1.0 - revealage));
    };

    Fragment_Shader(
sampler PointSampler,texture2d<float> AccumulationTexture,texture2d<float> RevealageTexture) :
PointSampler(PointSampler),AccumulationTexture(AccumulationTexture),RevealageTexture(RevealageTexture) {}
};

struct FSData
{
    sampler PointSampler;
    texture2d<float> AccumulationTexture;
    texture2d<float> RevealageTexture;
};

fragment float4 stageMain(
Fragment_Shader::VSOutput input [[stage_in]],
constant FSData& fsData [[buffer(UPDATE_FREQ_NONE)]]
)
{
    Fragment_Shader::VSOutput input0;
    input0.Position = float4(input.Position.xyz, 1.0 / input.Position.w);
    input0.UV = input.UV;
    Fragment_Shader main(fsData.PointSampler, fsData.AccumulationTexture, fsData.RevealageTexture);
    return main.main(input0);
}
