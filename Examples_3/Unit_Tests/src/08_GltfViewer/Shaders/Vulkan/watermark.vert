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

#version 450 core

precision highp float;
precision highp int; 

layout(location = 0) in vec3 POSITION;
layout(location = 1) in vec2 TEXCOORD;
layout(location = 0) out vec2 vertOutput_TEXCOORD;

struct VSInput
{
    vec3 Position;
    vec2 TexCoord;
};
struct VSOutput
{
    vec4 Position;
    vec2 TexCoord;
};
VSOutput HLSLmain(VSInput input1)
{
    VSOutput Out;
    ((Out).Position = vec4((input1).Position, 1.0));
    ((Out).TexCoord = (input1).TexCoord);
    return Out;
}
void main()
{
    VSInput input1;
    input1.Position = POSITION;
    input1.TexCoord = TEXCOORD;
    VSOutput result = HLSLmain(input1);
    gl_Position = result.Position;
    vertOutput_TEXCOORD = result.TexCoord;
}
