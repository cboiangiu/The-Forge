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

cbuffer uniformBlock : register(b0, UPDATE_FREQ_PER_FRAME)
{
	float4x4 ProjectionViewMat;
    float4x4 ModelMatrixCapsule;
    float4x4 ModelMatrixCube;
};

struct VSOutput {
	float4 Position : SV_POSITION;
    float4 TexCoord : TEXCOORD;
};

VSOutput main(float4 Position : POSITION)
{
	VSOutput result;
 
    float4 p = float4(Position.x*9, Position.y*9, Position.z*9, 1.0);
    float4x4 m =  ProjectionViewMat;
    p = mul(m,p);
    result.Position = p.xyww;
    result.TexCoord = float4(Position.x, Position.y, Position.z,Position.w);
	return result;
}
