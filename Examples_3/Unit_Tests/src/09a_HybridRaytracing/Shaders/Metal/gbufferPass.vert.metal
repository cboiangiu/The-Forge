	/*
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

#include <metal_stdlib>
#include <metal_atomic>
using namespace metal;

inline float3x3 matrix_ctor(float4x4 m) {
        return float3x3(m[0].xyz, m[1].xyz, m[2].xyz);
}
struct Vertex_Shader
{
    struct Uniforms_cbPerPass {

        float4x4 projView;
    };
    constant Uniforms_cbPerPass & cbPerPass;
    struct Uniforms_cbPerProp {

        float4x4 world;
        float roughness;
        float metallic;
        int pbrMaterials;
        float pad;
    };
    constant Uniforms_cbPerProp & cbPerProp;
	struct VsIn
	{
		float3 position [[attribute(0)]];
		float3 normal [[attribute(1)]];
		float2 texCoord [[attribute(2)]];
	};
    struct PsIn
    {
        float3 normal;
        float3 pos;
        float2 uv;
        uint materialID [[flat]];
        float4 position [[position]];
    };

	PsIn main(Vertex_Shader::VsIn In)
    {
		Vertex_Shader::PsIn Out;

        Out.position = ((cbPerPass.projView)*(((cbPerProp.world)*(float4(In.position.xyz, 1.0)))));
        Out.normal = (((matrix_ctor(cbPerProp.world))*(In.normal.xyz))).xyz;

        Out.pos = (((cbPerProp.world)*(float4(In.position.xyz, 1.0)))).xyz;
        Out.uv = In.texCoord.xy;

        return Out;
    };

    Vertex_Shader(constant Uniforms_cbPerPass & cbPerPass,
    constant Uniforms_cbPerProp & cbPerProp) : cbPerPass(cbPerPass),
    cbPerProp(cbPerProp) {}
};

struct VSData
{
    constant Vertex_Shader::Uniforms_cbPerProp & cbPerProp [[id(0)]];
};

struct VSDataPerFrame
{
    constant Vertex_Shader::Uniforms_cbPerPass & cbPerPass [[id(0)]];
};

vertex Vertex_Shader::PsIn stageMain(
	Vertex_Shader::VsIn vin [[stage_in]],
    constant VSData& vsData                     [[buffer(UPDATE_FREQ_NONE)]],
    constant VSDataPerFrame& vsDataPerFrame     [[buffer(UPDATE_FREQ_PER_FRAME)]]
)
{
    Vertex_Shader main(vsDataPerFrame.cbPerPass, vsData.cbPerProp);
    return main.main(vin);
}
