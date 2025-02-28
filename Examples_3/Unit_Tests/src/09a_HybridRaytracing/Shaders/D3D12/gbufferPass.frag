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


cbuffer cbPerPass : register(b0, UPDATE_FREQ_PER_FRAME)
{
	float4x4	projView;
}

cbuffer cbPerProp : register(b1)
{
	float4x4	world;
	float		roughness;
	float		metallic;
	int			pbrMaterials;
	float		pad;
}

cbuffer cbTextureRootConstants : register(b2) 
{
	uint albedoMap;
	uint normalMap;
	uint metallicMap;
	uint roughnessMap;
	uint aoMap;
}

SamplerState samplerLinear : register(s2);

// material parameters
Texture2D textureMaps[] : register(t3);


struct PsIn
{    
    float3 normal : TEXCOORD0;
	float3 pos	  : TEXCOORD1;
	float2 uv	  : TEXCOORD2;
};

struct PSOut
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
};


PSOut main(PsIn input) : SV_TARGET
{	
	PSOut Out = (PSOut) 0;

	//load albedo
	float3 albedo = textureMaps[albedoMap].Sample(samplerLinear, input.uv).rgb;

	float3 N = normalize(input.normal);

	Out.albedo = float4(albedo, 1);
	Out.normal = float4(N, 0);

	return Out;
}