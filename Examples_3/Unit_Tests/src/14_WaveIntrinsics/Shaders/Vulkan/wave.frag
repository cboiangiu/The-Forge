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

#version 450

#ifdef TARGET_SWITCH
	#extension GL_ARB_shader_ballot : require
	#extension GL_KHR_shader_subgroup_basic : require
	#extension GL_ARB_gpu_shader_int64 : require
//	#extension GL_NV_shader_thread_group : require
#else
	#extension GL_KHR_shader_subgroup_basic : require
	#extension GL_KHR_shader_subgroup_arithmetic : require
	#extension GL_KHR_shader_subgroup_ballot : require
	#extension GL_KHR_shader_subgroup_quad : require
#endif

layout(UPDATE_FREQ_PER_FRAME, binding = 0, std140) uniform SceneConstantBuffer
{
	layout(row_major) mat4 orthProjMatrix;
	vec2 mousePosition;
	vec2 resolution;
	float time;
	uint renderMode;
	uint laneSize;
	uint padding;
};

layout(location = 0) in vec4 in_var_COLOR;
layout(location = 0) out vec4 out_var_SV_TARGET;

// use this to generate grid-like texture
float texPattern(vec2 position)
{
	float scale = 0.13;
	float t = sin(position.x * scale) + cos(position.y * scale);
	float c = smoothstep(0.0, 0.2, t*t);

	return c;
}

void main()
{
	vec4 outputColor;

	// Add grid-like texture pattern on top of the color
	float texP = texPattern(gl_FragCoord.xy);
	outputColor = texP * in_var_COLOR;

	switch (renderMode)
	{
	case 1:
	{
		// Just pass through the color we generate before
		break;
	}
	case 2:
	{
		// Example of query intrinsics: WaveGetLaneIndex
		// Gradiently color the wave block by their lane id. Black for the smallest lane id and White for the largest lane id.
		outputColor = vec4(float(gl_SubgroupInvocationID) / float(laneSize));
		break;
	}
	case 3:
	{
		// Example of query intrinsics: WaveIsFirstLane
		// Mark the first lane as white pixel
#ifdef TARGET_SWITCH
		if (gl_SubGroupInvocationARB == 0)
#else
		if (subgroupElect())
#endif
			outputColor = vec4(1., 1., 1., 1.);
		break;
	}
	case 4:
	{
		// Example of query intrinsics: WaveIsFirstLane
		// Mark the first active lane as white pixel. Mark the last active lane as red pixel.
#ifdef TARGET_SWITCH
		if (gl_SubGroupInvocationARB == 0)
#else
		if (subgroupElect())
#endif
			outputColor = vec4(1., 1., 1., 1.);
#ifdef TARGET_SWITCH
		if (gl_SubGroupInvocationARB == (gl_SubGroupSizeARB - 1))
#else
		if (gl_SubgroupInvocationID == subgroupMax(gl_SubgroupInvocationID))
#endif
			outputColor = vec4(1., 0., 0., 1.);
		break;
	}
	case 5:
	{
		// Example of vote intrinsics: WaveActiveBallot
		// Active lanes ratios (# of total activelanes / # of total lanes).
#ifdef TARGET_SWITCH
/*
		uint64 activeLaneMask = ballotARB(true);
		uint numActiveLanes = bitCount(activeLaneMask.x) + bitCount(activeLaneMask.y) + bitCount(activeLaneMask.z) + bitCount(activeLaneMask.w);
		float activeRatio = float(numActiveLanes) / float(laneSize);
		outputColor = vec4(activeRatio, activeRatio, activeRatio, 1.0);
*/
#else
#ifdef TARGET_ANDROID
		//Due to driver bugs Android currently crashes while calling subgroupBallot(true)
		uvec4 activeLaneMask = subgroupBallot(false);
#else
		uvec4 activeLaneMask = subgroupBallot(true);
#endif //TARGET_ANDROID
		uint numActiveLanes = bitCount(activeLaneMask.x) + bitCount(activeLaneMask.y) + bitCount(activeLaneMask.z) + bitCount(activeLaneMask.w);
		float activeRatio = float(numActiveLanes) / float(laneSize);
		outputColor = vec4(activeRatio, activeRatio, activeRatio, 1.0);
#endif
		break;
	}
	case 6:
	{
		// Example of wave broadcast intrinsics: WaveReadLaneFirst
		// Broadcast the color in first lan to the wave.
#ifdef TARGET_SWITCH
		outputColor = readFirstInvocationARB(outputColor);
#else
		outputColor = subgroupBroadcastFirst(outputColor);
#endif
		break;
	}
	case 7:
	{
		// Example of wave reduction intrinsics: WaveActiveSum
		// Paint the wave with the averaged color inside the wave.
#ifdef TARGET_SWITCH
/*
		uint64 activeLaneMask = ballotARB(true);
		uint numActiveLanes = bitCount(activeLaneMask.x) + bitCount(activeLaneMask.y) + bitCount(activeLaneMask.z) + bitCount(activeLaneMask.w);
		// shuffle?
		//vec4 avgColor = subgroupAdd(outputColor) / float(numActiveLanes);
		//outputColor = avgColor;
*/
#else
#ifdef TARGET_ANDROID
		//Due to driver bugs Android currently crashes while calling subgroupBallot(true)
		uvec4 activeLaneMask = subgroupBallot(false);
#else
		uvec4 activeLaneMask = subgroupBallot(true);
#endif //TARGET_ANDROID
		uint numActiveLanes = bitCount(activeLaneMask.x) + bitCount(activeLaneMask.y) + bitCount(activeLaneMask.z) + bitCount(activeLaneMask.w);
		vec4 avgColor = subgroupAdd(outputColor) / float(numActiveLanes);
		outputColor = avgColor;
#endif
		break;
	}
	case 8:
	{
		// Example of wave scan intrinsics: WavePrefixSum
		// First, compute the prefix sum of distance each lane to first lane.
		// Then, use the prefix sum value to color each pixel.
#ifdef TARGET_SWITCH
#else
		vec4 basePos = subgroupBroadcastFirst(gl_FragCoord);
		vec4 prefixSumPos = subgroupExclusiveAdd(gl_FragCoord - basePos);

		// Get the number of total active lanes.
#ifdef TARGET_ANDROID
		//Due to driver bugs Android currently crashes while calling subgroupBallot(true)
		uvec4 activeLaneMask = subgroupBallot(false);
#else
		uvec4 activeLaneMask = subgroupBallot(true);
#endif //TARGET_ANDROID
		uint numActiveLanes = bitCount(activeLaneMask.x) + bitCount(activeLaneMask.y) + bitCount(activeLaneMask.z) + bitCount(activeLaneMask.w);

		outputColor = prefixSumPos / numActiveLanes;
#endif
		break;
	}
	case 9:
	{
		// Example of Quad-Wide shuffle intrinsics: QuadReadAcrossX and QuadReadAcrossY
		// Color pixels based on their quad id:
		//  q0 -> red
		//  q1 -> green
		//  q2 -> blue
		//  q3 -> white
		//
		//   -------------> x
		//  |   [0] [1]
		//  |   [2] [3]
		//  V
		//  Y
		//
#if defined(TARGET_SWITCH) || defined(TARGET_ANDROID)
/*
		// NV_shader_thread_group ?
		float dx = quadSwizzleXNV(gl_FragCoord.x) - gl_FragCoord.x;
		float dy = quadSwizzleYNV(gl_FragCoord.y) - gl_FragCoord.y;
*/
		float dx = 0.0;
		float dy = 0.0;
#else
		float dx = subgroupQuadSwapHorizontal(gl_FragCoord.x) - gl_FragCoord.x;
		float dy = subgroupQuadSwapVertical(gl_FragCoord.y) - gl_FragCoord.y;
#endif

		// q0
		if (dx > 0 && dy > 0)
			outputColor = vec4(1, 0, 0, 1);
		// q1
		else if (dx < 0 && dy > 0)
			outputColor = vec4(0, 1, 0, 1);
		// q2
		else if (dx > 0 && dy < 0)
			outputColor = vec4(0, 0, 1, 1);
		// q3
		else if (dx < 0 && dy < 0)
			outputColor = vec4(1, 1, 1, 1);
		else
			outputColor = vec4(0, 0, 0, 1);

		break;
	}

	default:
	{
		break;
	}
	}

	out_var_SV_TARGET = outputColor;
}
