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
#version 450 core

precision highp float;
precision highp int; 
vec4 MulMat(mat4 lhs, vec4 rhs)
{
    vec4 dst;
	dst[0] = lhs[0][0]*rhs[0] + lhs[0][1]*rhs[1] + lhs[0][2]*rhs[2] + lhs[0][3]*rhs[3];
	dst[1] = lhs[1][0]*rhs[0] + lhs[1][1]*rhs[1] + lhs[1][2]*rhs[2] + lhs[1][3]*rhs[3];
	dst[2] = lhs[2][0]*rhs[0] + lhs[2][1]*rhs[1] + lhs[2][2]*rhs[2] + lhs[2][3]*rhs[3];
	dst[3] = lhs[3][0]*rhs[0] + lhs[3][1]*rhs[1] + lhs[3][2]*rhs[2] + lhs[3][3]*rhs[3];
    return dst;
}


layout(location = 0) in uvec4 POSITION;
layout(location = 1) in ivec4 NORMAL;
layout(location = 2) in uvec2 TEXCOORD0;
layout(location = 3) in uvec4 COLOR;
layout(location = 4) in uvec2 TEXCOORD1;
layout(location = 5) in uvec2 TEXCOORD2;

layout(UPDATE_FREQ_PER_FRAME, binding = 2) uniform ShadowUniformBuffer
{
    mat4 ViewProjMat;
};

layout(std140, UPDATE_FREQ_PER_DRAW, binding = 0) uniform cbPerProp
{
	uniform mat4  world;
	uniform mat4  InvTranspose;
	uniform int   unlit;
	uniform int   hasAlbedoMap;
	uniform int   hasNormalMap;
	uniform int   hasMetallicRoughnessMap;
	uniform int   hasAOMap;
	uniform int   hasEmissiveMap;
	uniform vec4  centerOffset;
	uniform vec4  posOffset;
	uniform vec2  uvOffset;
	uniform vec2  uvScale;
  uniform float posScale;
  uniform uint  textureMapInfo;

	uniform uint  sparseTextureMapInfo;
  uniform float padding00;
  uniform float padding01;
  uniform float padding02;
};

struct VsIn
{
    uvec4 position;
    ivec4 normal;
    uvec2 texCoord;
    uvec4 baseColor;
    uvec2 metallicRoughness;
    uvec2 alphaSettings;
};

struct PsIn
{
    vec4 Position;
};

PsIn HLSLmain(VsIn input1)
{
    PsIn output1;
    float unormPositionScale = (float((1 << 16)) - 1.0);
    vec4 inPos = vec4((input1.position.xyz / vec3(unormPositionScale)) * posScale + posOffset.xyz, 1.0);
	inPos += centerOffset;
	vec4 worldPosition = world * inPos;	
	worldPosition.xyz /= posScale;
    output1.Position = ViewProjMat * worldPosition;
    return output1;
}

void main()
{
    VsIn input1;
    input1.position = POSITION;
    input1.normal = NORMAL;
    input1.texCoord = TEXCOORD0;
    input1.baseColor = COLOR;
    input1.metallicRoughness = TEXCOORD1;
    input1.alphaSettings = TEXCOORD2;
    PsIn result = HLSLmain(input1);
    gl_Position = result.Position;
}
