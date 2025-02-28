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

#define MAX_NUM_OBJECTS 128
#define MAX_NUM_PARTICLES 2048    // Per system
#define CUBES_EACH_ROW 5
#define CUBES_EACH_COL 5
#define CUBE_NUM (CUBES_EACH_ROW * CUBES_EACH_COL + 1)
#define DEBUG_OUTPUT 1       //exclusively used for texture data visulization, such as rendering depth, shadow map etc.
#if defined(DIRECT3D12) || defined(VULKAN) || defined(PROSPERO) && !defined(XBOX)
#define AOIT_ENABLE 1
#endif
#define AOIT_NODE_COUNT 4    // 2, 4 or 8. Higher numbers give better results at the cost of performance
#if AOIT_NODE_COUNT == 2
#define AOIT_RT_COUNT 1
#else
#define AOIT_RT_COUNT (AOIT_NODE_COUNT / 4)
#endif
#define USE_SHADOWS 1
#define PT_USE_REFRACTION 1
#define PT_USE_DIFFUSION 1
#define PT_USE_CAUSTICS (0 & USE_SHADOWS)

//tiny stl
#include "../../../../Common_3/ThirdParty/OpenSource/EASTL/sort.h"
#include "../../../../Common_3/ThirdParty/OpenSource/EASTL/string.h"
#include "../../../../Common_3/ThirdParty/OpenSource/EASTL/vector.h"

//Interfaces
#include "../../../../Common_3/OS/Interfaces/ICameraController.h"
#include "../../../../Common_3/OS/Interfaces/IApp.h"
#include "../../../../Common_3/OS/Interfaces/ILog.h"
#include "../../../../Common_3/OS/Interfaces/IFileSystem.h"
#include "../../../../Common_3/OS/Interfaces/ITime.h"
#include "../../../../Common_3/OS/Interfaces/IProfiler.h"
#include "../../../../Common_3/OS/Interfaces/IInput.h"
#include "../../../../Common_3/OS/Core/ThreadSystem.h"
#include "../../../../Middleware_3/UI/AppUI.h"
#include "../../../../Common_3/Renderer/IRenderer.h"
#include "../../../../Common_3/Renderer/IResourceLoader.h"

//Math
#include "../../../../Common_3/OS/Math/MathTypes.h"

//input
#include "../../../../Common_3/OS/Interfaces/IMemory.h"

namespace eastl
{
	template <>
	struct has_equality<vec3> : eastl::false_type {};
}

const uint32_t gImageCount = 3;

typedef struct ParticleVertex
{
	float3 mPosition;
	float3 mNormal;
	float2 mUV;
} ParticleVertex;

typedef struct Material
{
	float4 mColor;
	float4 mTransmission;
	float  mRefractionRatio;
	float  mCollimation;
	float2 mPadding;
	uint   mTextureFlags;
	uint   mAlbedoTexture;
	uint   mMetallicTexture;
	uint   mRoughnessTexture;
	uint   mEmissiveTexture;
	uint   mPadding2[3];
} Material;

typedef enum MeshResource
{
	MESH_CUBE,
	MESH_SPHERE,
	MESH_PLANE,
	MESH_LION,
	MESH_COUNT,
	/* vvv These meshes have different behaviour to the other meshes vvv */
	MESH_PARTICLE_SYSTEM
} MeshResource;

typedef struct Object
{
	vec3         mPosition;
	vec3         mScale;
	vec3         mOrientation;
	MeshResource mMesh;
	Material     mMaterial;
} Object;

typedef struct ParticleSystem
{
	Buffer* pParticleBuffer;
	Object  mObject;

	eastl::vector<vec3>  mParticlePositions;
	eastl::vector<vec3>  mParticleVelocities;
	eastl::vector<float> mParticleLifetimes;
	size_t               mLifeParticleCount;
} ParticleSystem;

typedef struct Scene
{
	eastl::vector<Object>         mObjects;
	eastl::vector<ParticleSystem> mParticleSystems;
} Scene;

typedef struct DrawCall
{
	uint         mIndex;
	uint         mInstanceCount;
	uint         mInstanceOffset;
	MeshResource mMesh;
} DrawCall;

typedef struct ObjectInfoStruct
{
	mat4   mToWorldMat;
	mat4   mNormalMat;
	uint   mMaterialIndex;
	float3 mPadding;
} ObjectInfoStruct;

typedef struct MaterialUniformBlock
{
	Material mMaterials[MAX_NUM_OBJECTS];
} MaterialUniformBlock;

typedef struct ObjectInfoUniformBlock
{
	ObjectInfoStruct mObjectInfo[MAX_NUM_OBJECTS];
} ObjectInfoUniformBlock;

typedef struct SkyboxUniformBlock
{
	mat4 mViewProject;
} SkyboxUniformBlock;

typedef struct LightUniformBlock
{
	mat4 mLightViewProj;
	vec4 mLightDirection = { -1, -1, -1, 0 };
	vec4 mLightColor = { 1, 0, 0, 1 };
} LightUniformBlock;

typedef struct CameraUniform
{
	mat4 mViewProject;
	mat4 mViewMat;
	vec4 mClipInfo;
	vec4 mPosition;
} CameraUniform;

typedef struct AlphaBlendSettings
{
	bool mSortObjects = true;
	bool mSortParticles = true;
} AlphaBlendSettings;

typedef struct WBOITSettings
{
	float mColorResistance = 1.0f;    // Increase if low-coverage foreground transparents are affecting background transparent color.
	float mRangeAdjustment = 0.3f;    // Change to avoid saturating at the clamp bounds.
	float mDepthRange =
		200.0f;    // Decrease if high-opacity surfaces seem �too transparent�, increase if distant transparents are blending together too much.
	float mOrderingStrength = 4.0f;    // Increase if background is showing through foreground too much.
	float mUnderflowLimit = 1e-2f;     // Increase to reduce underflow artifacts.
	float mOverflowLimit = 3e3f;       // Decrease to reduce overflow artifacts.
} WBOITSettings;

typedef struct WBOITVolitionSettings
{
	float mOpacitySensitivity =
		3.0f;    // Should be greater than 1, so that we only downweight nearly transparent things. Otherwise, everything at the same depth should get equal weight. Can be artist controlled
	float mWeightBias =
		5.0f;    // Must be greater than zero. Weight bias helps prevent distant things from getting hugely lower weight than near things, as well as preventing floating point underflow
	float mPrecisionScalar =
		10000.0f;    // Adjusts where the weights fall in the floating point range, used to balance precision to combat both underflow and overflow
	float mMaximumWeight =
		20.0f;    // Don't weight near things more than a certain amount to both combat overflow and reduce the "overpower" effect of very near vs. very far things
	float mMaximumColorValue = 1000.0f;
	float mAdditiveSensitivity = 10.0f;    // How much we amplify the emissive when deciding whether to consider this additively blended
	float mEmissiveSensitivity = 0.5f;     // Artist controlled value between 0.01 and 1
} WBOITVolitionSettings;

typedef enum WBOITRenderTargets
{
	WBOIT_RT_ACCUMULATION,
	WBOIT_RT_REVEALAGE,
	WBOIT_RT_COUNT
} WBOITRenderTargets;

TinyImageFormat gWBOITRenderTargetFormats[WBOIT_RT_COUNT] = { TinyImageFormat_R16G16B16A16_SFLOAT, TinyImageFormat_R8G8B8A8_UNORM };

typedef enum PTRenderTargets
{
	PT_RT_ACCUMULATION,    // Shared with WBOIT
	PT_RT_MODULATION,
#if PT_USE_REFRACTION != 0
	PT_RT_REFRACTION,
#endif
	PT_RT_COUNT
} PTRenderTargets;

TinyImageFormat gPTRenderTargetFormats[3] = { TinyImageFormat_R16G16B16A16_SFLOAT, TinyImageFormat_R8G8B8A8_UNORM, TinyImageFormat_R16G16_SFLOAT };

typedef enum TextureResource
{
	TEXTURE_SKYBOX_RIGHT,
	TEXTURE_SKYBOX_LEFT,
	TEXTURE_SKYBOX_UP,
	TEXTURE_SKYBOX_DOWN,
	TEXTURE_SKYBOX_FRONT,
	TEXTURE_SKYBOX_BACK,
	TEXTURE_MEASURING_GRID,
	TEXTURE_COUNT
} TextureResource;

/************************************************************************/
// Shaders
/************************************************************************/
Shader* pShaderSkybox = NULL;
#if USE_SHADOWS != 0
Shader* pShaderShadow = NULL;
Shader* pShaderGaussianBlur = NULL;
#if PT_USE_CAUSTICS != 0
Shader* pShaderPTShadow = NULL;
Shader* pShaderPTDownsample = NULL;
Shader* pShaderPTCopyShadowDepth = NULL;
#endif
#endif
Shader* pShaderForward = NULL;
Shader* pShaderWBOITShade = NULL;
Shader* pShaderWBOITComposite = NULL;
Shader* pShaderWBOITVShade = NULL;
Shader* pShaderWBOITVComposite = NULL;
Shader* pShaderPTShade = NULL;
Shader* pShaderPTComposite = NULL;
#if PT_USE_DIFFUSION != 0
Shader* pShaderPTCopyDepth = NULL;
Shader* pShaderPTGenMips = NULL;
#endif
#if AOIT_ENABLE
Shader* pShaderAOITShade = NULL;
Shader* pShaderAOITComposite = NULL;
Shader* pShaderAOITClear = NULL;
#endif
/************************************************************************/
// Root signature
/************************************************************************/
RootSignature* pRootSignatureSkybox = NULL;
#if USE_SHADOWS != 0
RootSignature* pRootSignatureGaussianBlur = NULL;
#if PT_USE_CAUSTICS != 0
RootSignature* pRootSignaturePTDownsample = NULL;
RootSignature* pRootSignaturePTCopyShadowDepth = NULL;
#endif
#endif
RootSignature* pRootSignature = NULL;
RootSignature* pRootSignatureWBOITComposite = NULL;
RootSignature* pRootSignaturePTComposite = NULL;
#if PT_USE_DIFFUSION != 0
RootSignature* pRootSignaturePTCopyDepth = NULL;
RootSignature* pRootSignaturePTGenMips = NULL;
#endif
#if AOIT_ENABLE
RootSignature* pRootSignatureAOITShade = NULL;
RootSignature* pRootSignatureAOITComposite = NULL;
RootSignature* pRootSignatureAOITClear = NULL;
#endif
/************************************************************************/
// Descriptor sets
/************************************************************************/
#define VIEW_CAMERA 0
#define VIEW_SHADOW 1
#define GEOM_OPAQUE 0
#define GEOM_TRANSPARENT 1
#define UNIFORM_SET(f,v,g)(((f) * 4) + ((v) * 2 + (g)))

#define SHADE_FORWARD 0
#define SHADE_PT 1
#define SHADE_PT_SHADOW 2

DescriptorSet* pDescriptorSetSkybox[2] = { NULL };
DescriptorSet* pDescriptorSetGaussianBlur = { NULL };
DescriptorSet* pDescriptorSetUniforms = { NULL };
DescriptorSet* pDescriptorSetShade = { NULL };
DescriptorSet* pDescriptorSetPTGenMips = { NULL };
DescriptorSet* pDescriptorSetWBOITComposite = { NULL };
DescriptorSet* pDescriptorSetPTCopyDepth = { NULL };
DescriptorSet* pDescriptorSetPTComposite = { NULL };
#if PT_USE_CAUSTICS
DescriptorSet* pDescriptorSetPTCopyShadowDepth = { NULL };
DescriptorSet* pDescriptorSetPTDownsample = { NULL };
#endif
#if AOIT_ENABLE
DescriptorSet* pDescriptorSetAOITClear = { NULL };
DescriptorSet* pDescriptorSetAOITShade[2] = { NULL };
DescriptorSet* pDescriptorSetAOITComposite = { NULL };
#endif
/************************************************************************/
// Pipelines
/************************************************************************/
Pipeline* pPipelineSkybox = NULL;
#if USE_SHADOWS != 0
Pipeline* pPipelineShadow = NULL;
Pipeline* pPipelineGaussianBlur = NULL;
#if PT_USE_CAUSTICS != 0
Pipeline* pPipelinePTGaussianBlur = NULL;
Pipeline* pPipelinePTShadow = NULL;
Pipeline* pPipelinePTDownsample = NULL;
Pipeline* pPipelinePTCopyShadowDepth = NULL;
#endif
#endif
Pipeline* pPipelineForward = NULL;
Pipeline* pPipelineTransparentForward = NULL;
Pipeline* pPipelineWBOITShade = NULL;
Pipeline* pPipelineWBOITComposite = NULL;
Pipeline* pPipelineWBOITVShade = NULL;
Pipeline* pPipelineWBOITVComposite = NULL;
Pipeline* pPipelinePTShade = NULL;
Pipeline* pPipelinePTComposite = NULL;
#if PT_USE_DIFFUSION != 0
Pipeline* pPipelinePTCopyDepth = NULL;
Pipeline* pPipelinePTGenMips = NULL;
#endif
#if AOIT_ENABLE
Pipeline* pPipelineAOITShade = NULL;
Pipeline* pPipelineAOITComposite = NULL;
Pipeline* pPipelineAOITClear = NULL;
#endif

/************************************************************************/
// Render targets
/************************************************************************/
RenderTarget* pRenderTargetScreen = NULL;
RenderTarget* pRenderTargetDepth = NULL;
#if PT_USE_DIFFUSION != 0
RenderTarget* pRenderTargetPTDepthCopy = NULL;
#endif
RenderTarget* pRenderTargetWBOIT[WBOIT_RT_COUNT] = {};
RenderTarget* pRenderTargetPT[PT_RT_COUNT] = {};
RenderTarget* pRenderTargetPTBackground = NULL;
#if USE_SHADOWS != 0
RenderTarget* pRenderTargetShadowVariance[2] = { NULL };
RenderTarget* pRenderTargetShadowDepth = NULL;
#if PT_USE_CAUSTICS != 0
RenderTarget* pRenderTargetPTShadowVariance[3] = { NULL };
RenderTarget* pRenderTargetPTShadowFinal[2][3] = { { NULL } };
#endif
#endif
/************************************************************************/
// AOIT Resources
/************************************************************************/
#if AOIT_ENABLE
Texture* pTextureAOITClearMask;
Buffer*  pBufferAOITDepthData;
Buffer*  pBufferAOITColorData;
#endif
/************************************************************************/
// Samplers
/************************************************************************/
Sampler* pSamplerPoint = NULL;
Sampler* pSamplerPointClamp = NULL;
Sampler* pSamplerBilinear = NULL;
Sampler* pSamplerTrilinearAniso = NULL;
Sampler* pSamplerSkybox = NULL;
Sampler* pSamplerShadow = NULL;    // Only created when USE_SHADOWS != 0
/************************************************************************/
// Resources
/************************************************************************/
Buffer*   pBufferSkyboxVertex = NULL;
Geometry* pMeshes[MESH_COUNT] = {};
Texture*  pTextures[TEXTURE_COUNT] = {};

/************************************************************************/
// Uniform buffers
/************************************************************************/
Buffer* pBufferMaterials[gImageCount] = { NULL };
Buffer* pBufferOpaqueObjectTransforms[gImageCount] = { NULL };
Buffer* pBufferTransparentObjectTransforms[gImageCount] = { NULL };
Buffer* pBufferSkyboxUniform[gImageCount] = { NULL };
Buffer* pBufferLightUniform[gImageCount] = { NULL };
Buffer* pBufferCameraUniform[gImageCount] = { NULL };
Buffer* pBufferCameraLightUniform[gImageCount] = { NULL };
Buffer* pBufferWBOITSettings[gImageCount] = { NULL };

typedef enum TransparencyType
{
	TRANSPARENCY_TYPE_ALPHA_BLEND,
	TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT,
	TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION,
	TRANSPARENCY_TYPE_PHENOMENOLOGICAL,
#if AOIT_ENABLE
	TRANSPARENCY_TYPE_ADAPTIVE_OIT
#endif
} TransparencyType;

struct
{
	float3 mLightPosition = { 0, 10, 10 };    //light position, will be changed by GUI editor if not iOS
} gLightCpuSettings;

/************************************************************************/

VirtualJoystickUI gVirtualJoystick;

// Constants
uint32_t     gFrameIndex = 0;
float        gCurrentTime = 0.0f;

VertexLayout vertexLayoutDefault = {};

MaterialUniformBlock   gMaterialUniformData;
ObjectInfoUniformBlock gObjectInfoUniformData;
ObjectInfoUniformBlock gTransparentObjectInfoUniformData;
SkyboxUniformBlock     gSkyboxUniformData;
LightUniformBlock      gLightUniformData;
CameraUniform          gCameraUniformData;
CameraUniform          gCameraLightUniformData;
AlphaBlendSettings     gAlphaBlendSettings;
WBOITSettings          gWBOITSettingsData;
WBOITVolitionSettings  gWBOITVolitionSettingsData;

Scene                     gScene;
eastl::vector<DrawCall> gOpaqueDrawCalls;
eastl::vector<DrawCall> gTransparentDrawCalls;
vec3                      gObjectsCenter = { 0, 0, 0 };

ICameraController* pCameraController = NULL;
ICameraController* pLightView = NULL;

/// UI
UIApp         gAppUI;
GuiComponent* pGuiWindow = NULL;
TextDrawDesc  gFrameTimeDraw = TextDrawDesc(0, 0xff00ffff, 18);
ProfileToken  gGpuProfileToken;
HiresTimer    gCpuTimer;

Renderer* pRenderer = NULL;

Queue*   pGraphicsQueue = NULL;
CmdPool* pCmdPools[gImageCount];
Cmd*     pCmds[gImageCount];

SwapChain* pSwapChain = NULL;
Fence*     pRenderCompleteFences[gImageCount] = { NULL };
Semaphore* pImageAcquiredSemaphore = NULL;
Semaphore* pRenderCompleteSemaphores[gImageCount] = { NULL };

uint32_t gTransparencyType = TRANSPARENCY_TYPE_PHENOMENOLOGICAL;

void AddObject(
	MeshResource mesh, vec3 position, vec4 color, vec3 translucency = vec3(0.0f), float eta = 1.0f, float collimation = 0.0f,
	vec3 scale = vec3(1.0f), vec3 orientation = vec3(0.0f))
{
	gScene.mObjects.push_back(
		{ position, scale, orientation, mesh, { v4ToF4(color), float4(v3ToF3(translucency), 0.0f), eta, collimation } });
}

void AddObject(MeshResource mesh, vec3 position, TextureResource texture, vec3 scale = vec3(1.0f), vec3 orientation = vec3(0.0f))
{
	gScene.mObjects.push_back(
		{ position, scale, orientation, mesh, { float4(1.0f), float4(0.0f), 1.0f, 0.0f, float2(0.0f), 1, (uint)texture, 0, 0 } });
}

void AddParticleSystem(vec3 position, vec4 color, vec3 translucency = vec3(0.0f), vec3 scale = vec3(1.0f), vec3 orientation = vec3(0.0f))
{
	Buffer*        pParticleBuffer = NULL;
	BufferLoadDesc particleBufferDesc = {};
	particleBufferDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
	particleBufferDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
	particleBufferDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
	particleBufferDesc.mDesc.mSize = sizeof(ParticleVertex) * 6 * MAX_NUM_PARTICLES;
	particleBufferDesc.ppBuffer = &pParticleBuffer;
	addResource(&particleBufferDesc, NULL);

	gScene.mParticleSystems.push_back(ParticleSystem{
		pParticleBuffer,
		Object{ position, scale, orientation, MESH_PARTICLE_SYSTEM, { v4ToF4(color), float4(v3ToF3(translucency), 0.0f), 1.0f, 1.0f } },
		eastl::vector<vec3>(MAX_NUM_PARTICLES), eastl::vector<vec3>(MAX_NUM_PARTICLES), eastl::vector<float>(MAX_NUM_PARTICLES), 0 });
}

static void CreateScene()
{
	// Set plane
	AddObject(MESH_CUBE, vec3(0.0f), vec4(1.0f), vec3(0.0f), 1.0f, 1.0f, vec3(100.0f, 0.5f, 100.0f));

	// Set cubes
	const float cubeDist = 3.0f;
	vec3        curTrans = { -cubeDist * (CUBES_EACH_ROW - 1) / 2.f, 2.3f, -cubeDist * (CUBES_EACH_COL - 1) / 2.f };

	for (int i = 0; i < CUBES_EACH_ROW; ++i)
	{
		curTrans.setX(-cubeDist * (CUBES_EACH_ROW - 1) / 2.f);

		for (int j = 0; j < CUBES_EACH_COL; j++)
		{
			AddObject(
				MESH_CUBE, curTrans,
				vec4(float(i + 1) / CUBES_EACH_ROW, 1.0f - float(i + 1) / CUBES_EACH_ROW, 0.0f, float(j + 1) / CUBES_EACH_COL), vec3(0.0f),
				1.0f, 1.0f, vec3(1.0f));
			curTrans.setX(curTrans.getX() + cubeDist);
		}

		curTrans.setZ(curTrans.getZ() + cubeDist);
	}

	AddObject(MESH_CUBE, vec3(15.0f, 4.0f, 5.0f), vec4(1.0f, 0.0f, 0.0f, 0.9f), vec3(0.0f), 1.0f, 1.0f, vec3(4.0f, 4.0f, 0.1f));
	AddObject(MESH_CUBE, vec3(15.0f, 4.0f, 0.0f), vec4(0.0f, 1.0f, 0.0f, 0.9f), vec3(0.0f), 1.0f, 1.0f, vec3(4.0f, 4.0f, 0.1f));
	AddObject(MESH_CUBE, vec3(15.0f, 4.0f, -5.0f), vec4(0.0f, 0.0f, 1.0f, 0.9f), vec3(0.0f), 1.0f, 1.0f, vec3(4.0f, 4.0f, 0.1f));

	AddObject(MESH_CUBE, vec3(-15.0f, 4.0f, 5.0f), vec4(1.0f, 0.0f, 0.0f, 0.5f), vec3(0.0f), 1.0f, 1.0f, vec3(4.0f, 4.0f, 0.1f));
	AddObject(MESH_CUBE, vec3(-15.0f, 4.0f, 0.0f), vec4(0.0f, 1.0f, 0.0f, 0.5f), vec3(0.0f), 1.0f, 1.0f, vec3(4.0f, 4.0f, 0.1f));
	AddObject(MESH_CUBE, vec3(-15.0f, 4.0f, -5.0f), vec4(0.0f, 0.0f, 1.0f, 0.5f), vec3(0.0f), 1.0f, 1.0f, vec3(4.0f, 4.0f, 0.1f));

	for (int i = 0; i < 25; ++i)
		AddObject(
			MESH_CUBE, vec3(i * 2.0f - 25.0f, 4.0f, 25.0f), vec4(3.0f, 3.0f, 10.0f, 0.1f), vec3(0.0f), 1.0f, 1.0f, vec3(0.1f, 4.0f, 4.0f));

	AddObject(MESH_CUBE, vec3(1.0f, 5.0f, -22.0f), vec4(1.0f, 0.0f, 0.0f, 1.0f), vec3(0.0f), 1.0f, 0.0f, vec3(0.5f, 0.5f, 0.01f));
	AddObject(MESH_CUBE, vec3(-1.0f, 5.0f, -35.0f), vec4(0.0f, 1.0f, 0.0f, 1.0f), vec3(0.0f), 1.0f, 0.0f, vec3(1.0f, 1.0f, 0.005f));
	AddObject(MESH_SPHERE, vec3(0.0f, 5.0f, -25.0f), vec4(0.3f, 0.3f, 1.0f, 0.9f), vec3(0.3f, 0.3f, 1.0f), 1.5f, 0.0f, vec3(4.0f));

	AddObject(MESH_LION, vec3(10.0f, 0.0f, -25.0f), vec4(1.0f), vec3(0.0f), 1.0f, 0.0f, vec3(0.25f), vec3(0.0f, PI, 0.0f));
	AddObject(
		MESH_CUBE, vec3(7.0f, 5.0f, -22.0f), vec4(1.0f, 0.3f, 0.3f, 0.9f), vec3(1.0f, 0.3f, 0.3f), 1.0f, 0.0f, vec3(1.5f, 4.0f, 0.005f));
	AddObject(
		MESH_CUBE, vec3(10.0f, 5.0f, -22.0f), vec4(0.3f, 1.0f, 0.3f, 0.9f), vec3(0.3f, 1.0f, 0.3f), 1.0f, 0.5f, vec3(1.5f, 4.0f, 0.005f));
	AddObject(
		MESH_CUBE, vec3(13.0f, 5.0f, -22.0f), vec4(0.3f, 0.3f, 1.0f, 0.9f), vec3(0.3f, 0.3f, 1.0f), 1.0f, 0.9f, vec3(1.5f, 4.0f, 0.005f));

	AddParticleSystem(vec3(30.0f, 5.0f, 20.0f), vec4(1.0f, 0.0f, 0.0f, 0.5f));
	AddParticleSystem(vec3(30.0f, 5.0f, 25.0f), vec4(1.0f, 1.0f, 0.0f, 0.5f));

	AddObject(
		MESH_PLANE, vec3(-15.0f - 5.0f, 10.0f, -25.0f), TEXTURE_MEASURING_GRID, vec3(10.0f, 1.0f, 10.0f),
		vec3(-90.0f * (PI / 180.0f), PI, 0.0f));
	AddObject(
		MESH_SPHERE, vec3(-17.5f - 5.0f, 5.0f, -20.0f), vec4(0.3f, 0.3f, 1.0f, 0.9f), vec3(0.3f, 0.3f, 1.0f), 1.001f, 1.0f, vec3(1.0f));
	AddObject(MESH_SPHERE, vec3(-15.0f - 5.0f, 5.0f, -20.0f), vec4(0.3f, 0.3f, 1.0f, 0.9f), vec3(0.3f, 0.3f, 1.0f), 1.3f, 1.0f, vec3(1.0f));
	AddObject(MESH_SPHERE, vec3(-12.5f - 5.0f, 5.0f, -20.0f), vec4(0.3f, 0.3f, 1.0f, 0.9f), vec3(0.3f, 0.3f, 1.0f), 1.5f, 1.0f, vec3(1.0f));
}

bool DistanceCompare(const float3& a, const float3& b)
{
	if (a.getX() < b.getX())
		return false;
	else if (a.getX() > b.getX())
		return true;
	if (a.getY() < b.getY())
		return false;
	else if (a.getY() > b.getY())
		return true;

	return false;
}

bool MeshCompare(const float2& a, const float2& b)
{
	return a.getX() > b.getX();
}

void SwapParticles(ParticleSystem* pParticleSystem, size_t a, size_t b)
{
	vec3  pos = pParticleSystem->mParticlePositions[a];
	vec3  vel = pParticleSystem->mParticleVelocities[a];
	float life = pParticleSystem->mParticleLifetimes[a];

	pParticleSystem->mParticlePositions[a] = pParticleSystem->mParticlePositions[b];
	pParticleSystem->mParticleVelocities[a] = pParticleSystem->mParticleVelocities[b];
	pParticleSystem->mParticleLifetimes[a] = pParticleSystem->mParticleLifetimes[b];

	pParticleSystem->mParticlePositions[b] = pos;
	pParticleSystem->mParticleVelocities[b] = vel;
	pParticleSystem->mParticleLifetimes[b] = life;
}

//////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////
struct GuiController
{
	static void AddGui();
	static void RemoveGui();
	static void UpdateDynamicUI();

	static DynamicUIWidgets alphaBlendDynamicWidgets;
	static DynamicUIWidgets weightedBlendedOitDynamicWidgets;
	static DynamicUIWidgets weightedBlendedOitVolitionDynamicWidgets;

	static TransparencyType currentTransparencyType;
};
DynamicUIWidgets GuiController::alphaBlendDynamicWidgets;
DynamicUIWidgets GuiController::weightedBlendedOitDynamicWidgets;
DynamicUIWidgets GuiController::weightedBlendedOitVolitionDynamicWidgets;
TransparencyType  GuiController::currentTransparencyType;

//////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////

const char* gTestScripts[] = { "Test_AlphaBlend.lua", "Test_WeightedBlendedOIT.lua", "Test_WeightedBlendedOITVolition.lua", "Test_Phenomenological.lua", "Test_AdaptiveOIT.lua" };
uint32_t gScriptIndexes[] = { 0, 1, 2, 3, 4 };
uint32_t gCurrentScriptIndex = 0;
void RunScript()
{
	gAppUI.RunTestScript(gTestScripts[gCurrentScriptIndex]);
}

bool gTestGraphicsReset = false;
void testGraphicsReset()
{
	gTestGraphicsReset = !gTestGraphicsReset;
}

class Transparency: public IApp
{
public:
	bool Init() override
	{
		// FILE PATHS
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_SOURCES, "Shaders");
		fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG,   RD_SHADER_BINARIES, "CompiledShaders");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_GPU_CONFIG, "GPUCfg");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_TEXTURES, "Textures");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_MESHES, "Meshes");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_FONTS, "Fonts");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SCRIPTS, "Scripts");
	
		CameraMotionParameters cmp{ 16.0f, 60.0f, 20.0f };
		vec3                   camPos{ 0, 5, -15 };
		vec3                   lookAt{ 0, 5, 0 };

		pLightView = createGuiCameraController(camPos, lookAt);
		pCameraController = createFpsCameraController(camPos, lookAt);
		pCameraController->setMotionParameters(cmp);

		if (!initInputSystem(pWindow))
			return false;

		// App Actions
		InputActionDesc actionDesc = { InputBindings::BUTTON_FULLSCREEN, [](InputActionContext* ctx) { toggleFullscreen(((IApp*)ctx->pUserData)->pWindow); return true; }, this };
		addInputAction(&actionDesc);
		actionDesc = { InputBindings::BUTTON_EXIT, [](InputActionContext* ctx) { requestShutdown(); return true; } };
		addInputAction(&actionDesc);
		actionDesc =
		{
			InputBindings::BUTTON_ANY, [](InputActionContext* ctx)
			{
				bool capture = gAppUI.OnButton(ctx->mBinding, ctx->mBool, ctx->pPosition);
				setEnableCaptureInput(capture && INPUT_ACTION_PHASE_CANCELED != ctx->mPhase);
				return true;
			}, this
		};
		addInputAction(&actionDesc);
		typedef bool (*CameraInputHandler)(InputActionContext* ctx, uint32_t index);
		static CameraInputHandler onCameraInput = [](InputActionContext* ctx, uint32_t index)
		{
			if (!gAppUI.IsFocused() && *ctx->pCaptured)
			{
				gVirtualJoystick.OnMove(index, ctx->mPhase != INPUT_ACTION_PHASE_CANCELED, ctx->pPosition);
				index ? pCameraController->onRotate(ctx->mFloat2) : pCameraController->onMove(ctx->mFloat2);
			}
			return true;
		};
		actionDesc = { InputBindings::FLOAT_RIGHTSTICK, [](InputActionContext* ctx) { return onCameraInput(ctx, 1); }, NULL, 20.0f, 200.0f, 1.0f };
		addInputAction(&actionDesc);
		actionDesc = { InputBindings::FLOAT_LEFTSTICK, [](InputActionContext* ctx) { return onCameraInput(ctx, 0); }, NULL, 20.0f, 200.0f, 1.0f };
		addInputAction(&actionDesc);
		actionDesc = { InputBindings::BUTTON_NORTH, [](InputActionContext* ctx) { pCameraController->resetView(); return true; } };
		addInputAction(&actionDesc);

		return true;
	}

	void Exit() override
	{
		exitInputSystem();
		destroyCameraController(pCameraController);
		destroyCameraController(pLightView);
	}

	bool Load() override
	{
		if (mSettings.mResetGraphics || !pRenderer) 
		{
			RendererDesc settings = { NULL };
			initRenderer(GetName(), &settings, &pRenderer);

			QueueDesc queueDesc = {};
			queueDesc.mType = QUEUE_TYPE_GRAPHICS;
			queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;

			addQueue(pRenderer, &queueDesc, &pGraphicsQueue);
			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				CmdPoolDesc cmdPoolDesc = {};
				cmdPoolDesc.pQueue = pGraphicsQueue;
				addCmdPool(pRenderer, &cmdPoolDesc, &pCmdPools[i]);
				CmdDesc cmdDesc = {};
				cmdDesc.pPool = pCmdPools[i];
				addCmd(pRenderer, &cmdDesc, &pCmds[i]);
			}

			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				addFence(pRenderer, &pRenderCompleteFences[i]);
				addSemaphore(pRenderer, &pRenderCompleteSemaphores[i]);
			}
			addSemaphore(pRenderer, &pImageAcquiredSemaphore);

			initResourceLoaderInterface(pRenderer);

			LoadModels();

			if (!gVirtualJoystick.Init(pRenderer, "circlepad"))
				return false;

			CreateSamplers();
			CreateShaders();
			CreateRootSignatures();
			CreateResources();
			CreateUniformBuffers();
			CreateDescriptorSets();

			CreateScene();

			/************************************************************************/
			// Add GPU profiler
			/************************************************************************/
			if (!gAppUI.Init(pRenderer))
				return false;
			gAppUI.AddTestScripts(gTestScripts, sizeof(gTestScripts) / sizeof(gTestScripts[0]));

			gAppUI.LoadFont("TitilliumText/TitilliumText-Bold.otf");

			initProfiler();
			initProfilerUI(&gAppUI, mSettings.mWidth, mSettings.mHeight);

			gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

			GuiDesc guiDesc = {};
			guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.25f);

			pGuiWindow = gAppUI.AddGuiComponent(GetName(), &guiDesc);
			GuiController::AddGui();
			// Reset graphics with a button.
			ButtonWidget testGPUReset("ResetGraphicsDevice");
			testGPUReset.pOnEdited = testGraphicsReset;
			pGuiWindow->AddWidget(testGPUReset);
		}

		if (!CreateRenderTargetsAndSwapChain())
			return false;
		if (!gAppUI.Load(pSwapChain->ppRenderTargets))
			return false;
		if (!gVirtualJoystick.Load(pSwapChain->ppRenderTargets[0]))
			return false;

		CreatePipelines();

		waitForAllResourceLoads();

		PrepareDescriptorSets();

		return true;
	}

	void Unload() override
	{
		waitQueueIdle(pGraphicsQueue);

		gVirtualJoystick.Unload();

		gAppUI.Unload();

		DestroyPipelines();

		DestroyRenderTargetsAndSwapChian();

		if (mSettings.mQuit || mSettings.mResetGraphics) 
		{
			GuiController::RemoveGui();
			exitProfilerUI();

			exitProfiler();

			gAppUI.Exit();

			for (size_t i = 0; i < gScene.mParticleSystems.size(); ++i)
				removeResource(gScene.mParticleSystems[i].pParticleBuffer);

			gVirtualJoystick.Exit();

			DestroySamplers();
			DestroyShaders();
			DestroyDescriptorSets();
			DestroyRootSignatures();
			DestroyResources();
			DestroyUniformBuffers();

			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				removeFence(pRenderer, pRenderCompleteFences[i]);
				removeSemaphore(pRenderer, pRenderCompleteSemaphores[i]);
			}
			removeSemaphore(pRenderer, pImageAcquiredSemaphore);

			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				removeCmd(pRenderer, pCmds[i]);
				removeCmdPool(pRenderer, pCmdPools[i]);
			}

			exitResourceLoaderInterface(pRenderer);
			removeQueue(pRenderer, pGraphicsQueue);
			removeRenderer(pRenderer);

			gScene.mParticleSystems.set_capacity(0);
			gScene.mObjects.set_capacity(0);
			gOpaqueDrawCalls.set_capacity(0);
			gTransparentDrawCalls.set_capacity(0);
		}
	}

	void Update(float deltaTime) override
	{
		updateInputSystem(mSettings.mWidth, mSettings.mHeight);

		gCpuTimer.Reset();

		gCurrentTime += deltaTime;

		// Dynamic UI elements
		GuiController::UpdateDynamicUI();
		/************************************************************************/
		// Camera Update
		/************************************************************************/
		const float zNear = 1.0f;
		const float zFar = 4000.0f;
		pCameraController->update(deltaTime);
		mat4        viewMat = pCameraController->getViewMatrix();
		const float aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
		const float horizontal_fov = PI / 2.0f;
		mat4        projMat = mat4::perspective(horizontal_fov, aspectInverse, zNear, zFar);    //view matrix
		vec3        camPos = pCameraController->getViewPosition();
		mat4        vpMatrix = projMat * viewMat;
		/************************************************************************/
		// Light Update
		/************************************************************************/
		const float lightZNear = -100.0f;
		const float lightZFar = 100.0f;
		vec3 lightPos = vec3(gLightCpuSettings.mLightPosition.x, gLightCpuSettings.mLightPosition.y, gLightCpuSettings.mLightPosition.z);
		vec3 lightDir = normalize(gObjectsCenter - lightPos);
		pLightView->moveTo(lightDir * lightZNear);
		pLightView->lookAt(gObjectsCenter);
		mat4 lightViewMat = pLightView->getViewMatrix();
		mat4 lightProjMat = mat4::orthographic(-50.0f, 50.0f, -50.0f, 50.0f, 0.0f, lightZFar - lightZNear);
		mat4 lightVPMatrix = lightProjMat * lightViewMat;
		/************************************************************************/
		// Scene Update
		/************************************************************************/
		UpdateScene(deltaTime, viewMat, camPos);
		/************************************************************************/
		// Update Cameras
		/************************************************************************/
		gCameraUniformData.mViewProject = vpMatrix;
		gCameraUniformData.mViewMat = viewMat;
		gCameraUniformData.mClipInfo = vec4(zNear * zFar, zNear - zFar, zFar, 0.0f);
		gCameraUniformData.mPosition = vec4(pCameraController->getViewPosition(), 1);

		gCameraLightUniformData.mViewProject = lightVPMatrix;
		gCameraLightUniformData.mViewMat = lightViewMat;
		gCameraLightUniformData.mClipInfo = vec4(lightZNear * lightZFar, lightZNear - lightZFar, lightZFar, 0.0f);
		gCameraLightUniformData.mPosition = vec4(lightPos, 1);

		/************************************************************************/
		// Update Skybox
		/************************************************************************/
		viewMat.setTranslation(vec3(0, 0, 0));
		gSkyboxUniformData.mViewProject = projMat * viewMat;
		/************************************************************************/
		// Light Matrix Update
		/************************************************************************/
		gLightUniformData.mLightDirection = vec4(lightDir, 0);
		gLightUniformData.mLightViewProj = lightVPMatrix;
		gLightUniformData.mLightColor = vec4(1, 1, 1, 1);

		////////////////////////////////////////////////////////////////
		gAppUI.Update(deltaTime);
	}

	void UpdateParticleSystems(float deltaTime, mat4 viewMat, vec3 camPos)
	{
		const float             particleSize = 0.2f;
		const vec3              camRight = vec3((float)viewMat[0][0], viewMat[1][0], viewMat[2][0]) * particleSize;
		const vec3              camUp = vec3((float)viewMat[0][1], viewMat[1][1], viewMat[2][1]) * particleSize;

		for (size_t i = 0; i < gScene.mParticleSystems.size(); ++i)
		{
			ParticleSystem* pParticleSystem = &gScene.mParticleSystems[i];

			BufferUpdateDesc particleBufferUpdateDesc = { pParticleSystem->pParticleBuffer };
			particleBufferUpdateDesc.mSize = sizeof(ParticleVertex) * 6 * pParticleSystem->mLifeParticleCount;
			beginUpdateResource(&particleBufferUpdateDesc);
			ParticleVertex* particleVertexData = (ParticleVertex*)particleBufferUpdateDesc.pMappedData;

			// Remove dead particles
			for (size_t j = 0; j < pParticleSystem->mLifeParticleCount; ++j)
			{
				float* pLifetime = &pParticleSystem->mParticleLifetimes[j];
				*pLifetime -= deltaTime;

				if (*pLifetime < 0.0f)
				{
					--pParticleSystem->mLifeParticleCount;
					if (j != pParticleSystem->mLifeParticleCount)
						SwapParticles(pParticleSystem, j, pParticleSystem->mLifeParticleCount);
					--j;
				}
			}

			// Spawn new particles
			size_t newParticleCount = (size_t)max(deltaTime * 25.0f, 1.0f);
			for (size_t j = 0; j < newParticleCount && pParticleSystem->mLifeParticleCount < MAX_NUM_PARTICLES; ++j)
			{
				size_t pi = pParticleSystem->mLifeParticleCount;
				pParticleSystem->mParticleVelocities[pi] = normalize(
					vec3(sin(gCurrentTime + pi) * 0.97f, cos(gCurrentTime * gCurrentTime + pi), sin(gCurrentTime * pi)) *
					cos(gCurrentTime + deltaTime * pi));
				pParticleSystem->mParticlePositions[pi] = pParticleSystem->mParticleVelocities[pi];
				pParticleSystem->mParticleLifetimes[pi] = (sin(gCurrentTime + pi) + 1.0f) * 3.0f + 10.0f;
				++pParticleSystem->mLifeParticleCount;
			}

			// Update particles
			for (size_t j = 0; j < pParticleSystem->mLifeParticleCount; ++j)
			{
				pParticleSystem->mParticlePositions[j] += pParticleSystem->mParticleVelocities[j] * deltaTime;
				pParticleSystem->mParticleVelocities[j] *= 1.0f - 0.2f * deltaTime;
			}

			// Update vertex buffers
			if (gTransparencyType == TRANSPARENCY_TYPE_ALPHA_BLEND && gAlphaBlendSettings.mSortParticles)
			{
				eastl::vector<float2> sortedArray;

				for (size_t j = 0; j < pParticleSystem->mLifeParticleCount; ++j)
					sortedArray.push_back({ (float)distSqr(Point3(camPos), Point3(pParticleSystem->mParticlePositions[j])), (float)j });

				eastl::quick_sort(sortedArray.begin(), sortedArray.end(), MeshCompare);

				for (uint j = 0; j < sortedArray.size(); ++j)
				{
					vec3 pos = pParticleSystem->mParticlePositions[(int)sortedArray[sortedArray.size() - j - 1][1]];
					particleVertexData[j * 6 + 0] = { v3ToF3(pos - camUp - camRight), float3(0.0f, 1.0f, 0.0f), float2(0.0f, 0.0f) };
					particleVertexData[j * 6 + 1] = { v3ToF3(pos + camUp - camRight), float3(0.0f, 1.0f, 0.0f), float2(0.0f, 1.0f) };
					particleVertexData[j * 6 + 2] = { v3ToF3(pos - camUp + camRight), float3(0.0f, 1.0f, 0.0f), float2(1.0f, 0.0f) };
					particleVertexData[j * 6 + 3] = { v3ToF3(pos + camUp + camRight), float3(0.0f, 1.0f, 0.0f), float2(1.0f, 1.0f) };
					particleVertexData[j * 6 + 4] = { v3ToF3(pos - camUp + camRight), float3(0.0f, 1.0f, 0.0f), float2(1.0f, 0.0f) };
					particleVertexData[j * 6 + 5] = { v3ToF3(pos + camUp - camRight), float3(0.0f, 1.0f, 0.0f), float2(0.0f, 1.0f) };
				}
			}
			else
			{
				for (uint j = 0; j < pParticleSystem->mLifeParticleCount; ++j)
				{
					vec3 pos = pParticleSystem->mParticlePositions[j];
					particleVertexData[j * 6 + 0] = { v3ToF3(pos - camUp - camRight), float3(0.0f, 1.0f, 0.0f), float2(0.0f, 0.0f) };
					particleVertexData[j * 6 + 1] = { v3ToF3(pos + camUp - camRight), float3(0.0f, 1.0f, 0.0f), float2(0.0f, 1.0f) };
					particleVertexData[j * 6 + 2] = { v3ToF3(pos - camUp + camRight), float3(0.0f, 1.0f, 0.0f), float2(1.0f, 0.0f) };
					particleVertexData[j * 6 + 3] = { v3ToF3(pos + camUp + camRight), float3(0.0f, 1.0f, 0.0f), float2(1.0f, 1.0f) };
					particleVertexData[j * 6 + 4] = { v3ToF3(pos - camUp + camRight), float3(0.0f, 1.0f, 0.0f), float2(1.0f, 0.0f) };
					particleVertexData[j * 6 + 5] = { v3ToF3(pos + camUp - camRight), float3(0.0f, 1.0f, 0.0f), float2(0.0f, 1.0f) };
				}
			}

			endUpdateResource(&particleBufferUpdateDesc, NULL);
		}
	}

	void CreateDrawCalls(
		float* pSortedObjects, uint objectCount, uint sizeOfObject, ObjectInfoUniformBlock* pObjectUniformBlock,
		MaterialUniformBlock* pMaterialUniformBlock, uint* pMaterialCount, eastl::vector<DrawCall>* pDrawCalls)
	{
		const uint meshIndexOffset = sizeOfObject - 2;
		const uint objectIndexOffset = sizeOfObject - 1;

		uint         instanceCount = 0;
		uint         instanceOffset = 0;
		MeshResource prevMesh = (MeshResource)0xFFFFFFFF;
		for (uint i = 0; i < objectCount; ++i)
		{
			uint          sortedObjectIndex = (objectCount - i - 1) * sizeOfObject;
			const Object* pObj = NULL;
			MeshResource  mesh = (MeshResource)(int)pSortedObjects[sortedObjectIndex + meshIndexOffset];
			int           index = (int)pSortedObjects[sortedObjectIndex + objectIndexOffset];
			if (mesh < MESH_COUNT)
				pObj = &gScene.mObjects[index];
			else
				pObj = &gScene.mParticleSystems[index].mObject;

			pObjectUniformBlock->mObjectInfo[i].mToWorldMat =
				mat4::translation(pObj->mPosition) * mat4::rotationZYX(pObj->mOrientation) * mat4::scale(pObj->mScale);
			pObjectUniformBlock->mObjectInfo[i].mNormalMat = mat4::rotationZYX(pObj->mOrientation);
			pObjectUniformBlock->mObjectInfo[i].mMaterialIndex = *pMaterialCount;
			pMaterialUniformBlock->mMaterials[*pMaterialCount] = pObj->mMaterial;
			++(*pMaterialCount);
			++instanceCount;

			if (mesh == MESH_PARTICLE_SYSTEM)
			{
				if (instanceCount > 1)
				{
					pDrawCalls->push_back({ 0, instanceCount - 1, instanceOffset, prevMesh });
					instanceOffset += instanceCount - 1;
					instanceCount = 1;
				}

				pDrawCalls->push_back({ (uint)index, instanceCount, instanceOffset, MESH_PARTICLE_SYSTEM });
				instanceOffset += instanceCount;
				instanceCount = 0;
			}
			else if (mesh != prevMesh && instanceCount > 1)
			{
				pDrawCalls->push_back({ 0, instanceCount - 1, instanceOffset, prevMesh });
				instanceOffset += instanceCount - 1;
				instanceCount = 1;
			}

			prevMesh = mesh;
		}

		if (instanceCount > 0)
			pDrawCalls->push_back({ 0, instanceCount, instanceOffset, prevMesh });
	}

	void UpdateScene(float deltaTime, mat4 viewMat, vec3 camPos)
	{
		uint materialCount = 0;

		UpdateParticleSystems(deltaTime, viewMat, camPos);

		// Create list of opaque objects
		gOpaqueDrawCalls.clear();
		uint opaqueObjectCount = 0;
		{
			eastl::vector<float2> sortedArray = {};

			for (size_t i = 0; i < gScene.mObjects.size(); ++i)
			{
				const Object* pObj = &gScene.mObjects[i];
				if (pObj->mMaterial.mColor.getW() == 1.0f)
					sortedArray.push_back({ (float)pObj->mMesh, (float)i });
			}
			for (size_t i = 0; i < gScene.mParticleSystems.size(); ++i)
			{
				const Object* pObj = &gScene.mParticleSystems[i].mObject;
				if (pObj->mMaterial.mColor.getW() == 1.0f)
					sortedArray.push_back({ (float)pObj->mMesh, (float)i });
			}

			opaqueObjectCount = (int)sortedArray.size();
			ASSERT(opaqueObjectCount < MAX_NUM_OBJECTS);
			eastl::quick_sort(sortedArray.begin(), sortedArray.end(), MeshCompare);    // Sorts by mesh

			CreateDrawCalls(
				(float*)sortedArray.data(), opaqueObjectCount, sizeof(sortedArray[0]) / sizeof(float), &gObjectInfoUniformData,
				&gMaterialUniformData, &materialCount, &gOpaqueDrawCalls);
		}

		// Create list of transparent objects
		gTransparentDrawCalls.clear();
		uint transparentObjectCount = 0;
		if (gTransparencyType == TRANSPARENCY_TYPE_ALPHA_BLEND && gAlphaBlendSettings.mSortObjects)
		{
			eastl::vector<float3> sortedArray = {};

			for (size_t i = 0; i < gScene.mObjects.size(); ++i)
			{
				const Object* pObj = &gScene.mObjects[i];
				if (pObj->mMaterial.mColor.getW() < 1.0f)
					sortedArray.push_back({ (float)distSqr(Point3(camPos), Point3(pObj->mPosition)) - (float)pow(maxElem(pObj->mScale), 2),
											(float)pObj->mMesh, (float)i });
			}
			for (size_t i = 0; i < gScene.mParticleSystems.size(); ++i)
			{
				const Object* pObj = &gScene.mParticleSystems[i].mObject;
				if (pObj->mMaterial.mColor.getW() < 1.0f)
					sortedArray.push_back({ (float)distSqr(Point3(camPos), Point3(pObj->mPosition)) - (float)pow(maxElem(pObj->mScale), 2),
											(float)pObj->mMesh, (float)i });
			}

			transparentObjectCount = (int)sortedArray.size();
			ASSERT(transparentObjectCount < MAX_NUM_OBJECTS);
			eastl::quick_sort(sortedArray.begin(), sortedArray.end(), DistanceCompare);     // Sorts by distance first, then by mesh

			CreateDrawCalls(
				(float*)sortedArray.data(), transparentObjectCount, sizeof(sortedArray[0]) / sizeof(float),
				&gTransparentObjectInfoUniformData, &gMaterialUniformData, &materialCount, &gTransparentDrawCalls);
		}
		else
		{
			eastl::vector<float2> sortedArray = {};

			for (size_t i = 0; i < gScene.mObjects.size(); ++i)
			{
				const Object* pObj = &gScene.mObjects[i];
				if (pObj->mMaterial.mColor.getW() < 1.0f)
					sortedArray.push_back({ (float)pObj->mMesh, (float)i });
			}
			for (size_t i = 0; i < gScene.mParticleSystems.size(); ++i)
			{
				const Object* pObj = &gScene.mParticleSystems[i].mObject;
				if (pObj->mMaterial.mColor.getW() < 1.0f)
					sortedArray.push_back({ (float)pObj->mMesh, (float)i });
			}

			transparentObjectCount = (int)sortedArray.size();
			ASSERT(transparentObjectCount < MAX_NUM_OBJECTS);
			eastl::quick_sort(sortedArray.begin(), sortedArray.end(), MeshCompare);    // Sorts by mesh

			CreateDrawCalls(
				(float*)sortedArray.data(), transparentObjectCount, sizeof(sortedArray[0]) / sizeof(float),
				&gTransparentObjectInfoUniformData, &gMaterialUniformData, &materialCount, &gTransparentDrawCalls);
		}
	}

	void DrawSkybox(Cmd* pCmd)
	{
		RenderTarget* rt = pRenderTargetScreen;
		if (gTransparencyType == TRANSPARENCY_TYPE_PHENOMENOLOGICAL)
		{
			rt = pRenderTargetPTBackground;
			RenderTargetBarrier barrier = { rt, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_RENDER_TARGET };
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, &barrier);
		}

		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_DONTCARE;
		loadActions.mClearColorValues[0] = rt->mClearValue;
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;

		cmdBeginDebugMarker(pCmd, 0, 0, 1, "Draw skybox");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Draw Skybox");

		cmdBindRenderTargets(pCmd, 1, &rt, NULL, &loadActions, NULL, NULL, -1, -1);

		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)rt->mWidth, (float)rt->mHeight, 1.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, rt->mWidth, rt->mHeight);

		const uint32_t skyboxStride = sizeof(float) * 4;
		cmdBindPipeline(pCmd, pPipelineSkybox);
		cmdBindDescriptorSet(pCmd, 0, pDescriptorSetSkybox[0]);
		cmdBindDescriptorSet(pCmd, gFrameIndex, pDescriptorSetSkybox[1]);
		cmdBindVertexBuffer(pCmd, 1, &pBufferSkyboxVertex, &skyboxStride, NULL);
		cmdDraw(pCmd, 36, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)rt->mWidth, (float)rt->mHeight, 0.0f, 1.0f);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);
	}

	void ShadowPass(Cmd* pCmd)
	{
#if USE_SHADOWS != 0
		RenderTargetBarrier barriers[2] = {};
		barriers[0].pRenderTarget = pRenderTargetShadowVariance[0];
		barriers[0].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		barriers[0].mNewState = RESOURCE_STATE_RENDER_TARGET;
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, barriers);

		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
		loadActions.mClearColorValues[0] = pRenderTargetShadowVariance[0]->mClearValue;
		loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
		loadActions.mClearDepth = pRenderTargetShadowDepth->mClearValue;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 1, &pRenderTargetShadowVariance[0], pRenderTargetShadowDepth, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(
			pCmd, 0.0f, 0.0f, (float)pRenderTargetShadowVariance[0]->mWidth, (float)pRenderTargetShadowVariance[0]->mHeight,
			0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetShadowVariance[0]->mWidth, pRenderTargetShadowVariance[0]->mHeight);

		// Draw the opaque objects.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Draw shadow map");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render shadow map");

		cmdBindPipeline(pCmd, pPipelineShadow);
		cmdBindDescriptorSet(pCmd, UNIFORM_SET(gFrameIndex, VIEW_SHADOW, GEOM_OPAQUE), pDescriptorSetUniforms);
		DrawObjects(pCmd, &gOpaqueDrawCalls, pRootSignature);
		cmdEndDebugMarker(pCmd);

		// Blur shadow map
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Blur shadow map");
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;

		for (uint32_t i = 0; i < 1; ++i)
		{
			float          axis = 0.0f;

			cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

			barriers[0].pRenderTarget = pRenderTargetShadowVariance[0];
			barriers[0].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			barriers[0].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].pRenderTarget = pRenderTargetShadowVariance[1];
			barriers[1].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].mNewState = RESOURCE_STATE_RENDER_TARGET;
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, barriers);

			cmdBindRenderTargets(pCmd, 1, &pRenderTargetShadowVariance[1], NULL, &loadActions, NULL, NULL, -1, -1);

			cmdBindPipeline(pCmd, pPipelineGaussianBlur);
			cmdBindPushConstants(pCmd, pRootSignatureGaussianBlur, "RootConstant", &axis);
			cmdBindDescriptorSet(pCmd, 0, pDescriptorSetGaussianBlur);
			cmdDraw(pCmd, 3, 0);

			cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

			barriers[0].pRenderTarget = pRenderTargetShadowVariance[1];
			barriers[0].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			barriers[0].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].pRenderTarget = pRenderTargetShadowVariance[0];
			barriers[1].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].mNewState = RESOURCE_STATE_RENDER_TARGET;
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, barriers);

			cmdBindRenderTargets(pCmd, 1, &pRenderTargetShadowVariance[0], NULL, &loadActions, NULL, NULL, -1, -1);
			cmdBindPipeline(pCmd, pPipelineGaussianBlur);

			axis = 1.0f;
			cmdBindPushConstants(pCmd, pRootSignatureGaussianBlur, "RootConstant", &axis);
			cmdBindDescriptorSet(pCmd, 1, pDescriptorSetGaussianBlur);
			cmdDraw(pCmd, 3, 0);
		}

		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);

		barriers[0].pRenderTarget = pRenderTargetShadowVariance[0];
		barriers[0].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
		barriers[0].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, barriers);
#endif
	}

	void StochasticShadowPass(Cmd* pCmd)
	{
#if PT_USE_CAUSTICS != 0
		RenderTargetBarrier barriers[3] = {};
		for (uint32_t i = 0; i < 3; ++i)
		{
			barriers[i].pRenderTarget = pRenderTargetPTShadowVariance[i];
			barriers[i].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[i].mNewState = RESOURCE_STATE_RENDER_TARGET;
		}
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 3, barriers);

		LoadActionsDesc loadActions = {};
		for (uint32_t i = 0; i < 3; ++i)
		{
			loadActions.mLoadActionsColor[i] = LOAD_ACTION_CLEAR;
			loadActions.mClearColorValues[i] = pRenderTargetPTShadowVariance[i]->mClearValue;
		}
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;

		// Copy depth buffer to shadow maps
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render stochastic shadow map", true);
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Copy shadow map");

		for (uint32_t w = 0; w < 3; ++w)
		{
			cmdBindRenderTargets(pCmd, 1, &pRenderTargetPTShadowVariance[w], NULL, &loadActions, NULL, NULL, -1, -1);
			cmdSetViewport(
				pCmd, 0.0f, 0.0f, (float)pRenderTargetPTShadowVariance[0]->mWidth,
				(float)pRenderTargetPTShadowVariance[0]->mHeight, 0.0f, 1.0f);
			cmdSetScissor(pCmd, 0, 0, pRenderTargetPTShadowVariance[0]->mWidth, pRenderTargetPTShadowVariance[0]->mHeight);
			
			cmdBindDescriptorSet(pCmd, 0, pDescriptorSetPTCopyShadowDepth);
			cmdBindPipeline(pCmd, pPipelinePTCopyShadowDepth);
			
			cmdDraw(pCmd, 3, 0);
		}
		cmdEndDebugMarker(pCmd);

		// Start render pass and apply load actions
		for (int i = 0; i < 3; ++i)
			loadActions.mLoadActionsColor[i] = LOAD_ACTION_LOAD;
		cmdBindRenderTargets(pCmd, 3, pRenderTargetPTShadowVariance, NULL, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(
			pCmd, 0.0f, 0.0f, (float)pRenderTargetPTShadowVariance[0]->mWidth, (float)pRenderTargetPTShadowVariance[0]->mHeight,
			0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetPTShadowVariance[0]->mWidth, pRenderTargetPTShadowVariance[0]->mHeight);

		// Draw the opaque objects.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Draw stochastic shadow map");

		cmdBindPipeline(pCmd, pPipelinePTShadow);
		cmdBindDescriptorSet(pCmd, SHADE_PT_SHADOW, pDescriptorSetShade);
		cmdBindDescriptorSet(pCmd, UNIFORM_SET(gFrameIndex, VIEW_SHADOW, GEOM_TRANSPARENT), pDescriptorSetUniforms);
		DrawObjects(pCmd, &gTransparentDrawCalls, pRootSignature);
		cmdEndDebugMarker(pCmd);

		// Downsample shadow map
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Downsample shadow map");
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_DONTCARE;
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;

		for (uint32_t w = 0; w < 3; ++w)
		{
			cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

			barriers[0].pRenderTarget = pRenderTargetPTShadowVariance[w];
			barriers[0].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			barriers[0].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].pRenderTarget = pRenderTargetPTShadowFinal[0][w];
			barriers[1].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].mNewState = RESOURCE_STATE_RENDER_TARGET;
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, barriers);

			cmdBindRenderTargets(pCmd, 1, &pRenderTargetPTShadowFinal[0][w], NULL, &loadActions, NULL, NULL, -1, -1);
			cmdSetViewport(
				pCmd, 0.0f, 0.0f, (float)pRenderTargetPTShadowFinal[0][w]->mWidth,
				(float)pRenderTargetPTShadowFinal[0][w]->mHeight, 0.0f, 1.0f);
			cmdSetScissor(pCmd, 0, 0, pRenderTargetPTShadowFinal[0][w]->mWidth, pRenderTargetPTShadowFinal[0][w]->mHeight);

			cmdBindPipeline(pCmd, pPipelinePTDownsample);
			cmdBindDescriptorSet(pCmd, w, pDescriptorSetPTDownsample);
			cmdDraw(pCmd, 3, 0);
		}
		cmdEndDebugMarker(pCmd);

		// Blur shadow map
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Blur shadow map");
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_DONTCARE;
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;

		for (uint32_t w = 0; w < 3; ++w)
		{
			float axis = 0.0f;

			cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

			barriers[0].pRenderTarget = pRenderTargetPTShadowFinal[0][w];
			barriers[0].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			barriers[0].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].pRenderTarget = pRenderTargetPTShadowFinal[1][w];
			barriers[1].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].mNewState = RESOURCE_STATE_RENDER_TARGET;
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, barriers);

			cmdBindRenderTargets(pCmd, 1, &pRenderTargetPTShadowFinal[1][w], NULL, &loadActions, NULL, NULL, -1, -1);

			cmdBindPipeline(pCmd, pPipelinePTGaussianBlur);
			cmdBindPushConstants(pCmd, pRootSignatureGaussianBlur, "RootConstant", &axis);
			cmdBindDescriptorSet(pCmd, 2 + (w * 2 + 0), pDescriptorSetGaussianBlur);
			cmdDraw(pCmd, 3, 0);

			cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

			barriers[0].pRenderTarget = pRenderTargetPTShadowFinal[1][w];
			barriers[0].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			barriers[0].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].pRenderTarget = pRenderTargetPTShadowFinal[0][w];
			barriers[1].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			barriers[1].mNewState = RESOURCE_STATE_RENDER_TARGET;
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, barriers);

			cmdBindRenderTargets(pCmd, 1, &pRenderTargetPTShadowFinal[0][w], NULL, &loadActions, NULL, NULL, -1, -1);
			cmdBindPipeline(pCmd, pPipelinePTGaussianBlur);

			axis = 1.0f;
			cmdBindPushConstants(pCmd, pRootSignatureGaussianBlur, "RootConstant", &axis);
			cmdBindDescriptorSet(pCmd, 2 + (w * 2 + 1), pDescriptorSetGaussianBlur);
			cmdDraw(pCmd, 3, 0);
		}

		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

		for (uint32_t w = 0; w < 3; ++w)
		{
			barriers[w].pRenderTarget = pRenderTargetPTShadowFinal[0][w];
			barriers[w].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			barriers[w].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 3, barriers);

		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);
#endif
	}

	void DrawObjects(Cmd* pCmd, eastl::vector<DrawCall>* pDrawCalls, RootSignature* pRootSignature)
	{
		static MeshResource boundMesh = (MeshResource)0xFFFFFFFF;
		static uint         vertexCount = 0;
		static uint         indexCount = 0;

		for (size_t i = 0; i < pDrawCalls->size(); ++i)
		{
			DrawCall* dc = &(*pDrawCalls)[i];
			cmdBindPushConstants(pCmd, pRootSignature, "DrawInfoRootConstant", &dc->mInstanceOffset);

			if (dc->mMesh != boundMesh || dc->mMesh > MESH_COUNT)
			{
				if (dc->mMesh == MESH_PARTICLE_SYSTEM)
				{
					const uint32_t stride = sizeof(ParticleVertex);
					cmdBindVertexBuffer(pCmd, 1, &gScene.mParticleSystems[dc->mIndex].pParticleBuffer, &stride, NULL);
					vertexCount = (uint)gScene.mParticleSystems[dc->mIndex].mLifeParticleCount * 6;
					indexCount = 0;
					boundMesh = MESH_PARTICLE_SYSTEM;
				}
				else
				{
					cmdBindVertexBuffer(pCmd, 1, &pMeshes[dc->mMesh]->pVertexBuffers[0], &pMeshes[dc->mMesh]->mVertexStrides[0], NULL);
					if (pMeshes[dc->mMesh]->pIndexBuffer)
						cmdBindIndexBuffer(pCmd, pMeshes[dc->mMesh]->pIndexBuffer, pMeshes[dc->mMesh]->mIndexType, NULL);
					vertexCount = pMeshes[dc->mMesh]->mVertexCount;
					indexCount = pMeshes[dc->mMesh]->mIndexCount;
				}
			}

			if (indexCount > 0)
				cmdDrawIndexedInstanced(pCmd, indexCount, 0, dc->mInstanceCount, 0, 0);
			else
				cmdDrawInstanced(pCmd, vertexCount, 0, dc->mInstanceCount, 0);
		}
	}

	void OpaquePass(Cmd* pCmd)
	{
		RenderTarget* rt = pRenderTargetScreen;
		if (gTransparencyType == TRANSPARENCY_TYPE_PHENOMENOLOGICAL)
			rt = pRenderTargetPTBackground;

		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
		loadActions.mClearDepth = pRenderTargetDepth->mClearValue;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 1, &rt, pRenderTargetDepth, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)rt->mWidth, (float)rt->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, rt->mWidth, rt->mHeight);

		// Draw the opaque objects.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Draw opaque geometry");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render opaque geometry");

		cmdBindPipeline(pCmd, pPipelineForward);
		cmdBindDescriptorSet(pCmd, SHADE_FORWARD, pDescriptorSetShade);
		cmdBindDescriptorSet(pCmd, UNIFORM_SET(gFrameIndex, VIEW_CAMERA, GEOM_OPAQUE), pDescriptorSetUniforms);
		DrawObjects(pCmd, &gOpaqueDrawCalls, pRootSignature);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

#if PT_USE_DIFFUSION != 0
		if (gTransparencyType == TRANSPARENCY_TYPE_PHENOMENOLOGICAL)
		{
			RenderTargetBarrier barrier = { rt, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_UNORDERED_ACCESS };
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, &barrier);

			uint32_t mipSizeX = 1 << (uint32_t)ceil(log2((float)rt->mWidth));
			uint32_t mipSizeY = 1 << (uint32_t)ceil(log2((float)rt->mHeight));
			cmdBindPipeline(pCmd, pPipelinePTGenMips);
			for (uint32_t i = 1; i < rt->mMipLevels; ++i)
			{
				mipSizeX >>= 1;
				mipSizeY >>= 1;
				uint mipSize[2] = { mipSizeX, mipSizeY };
				cmdBindPushConstants(pCmd, pRootSignaturePTGenMips, "RootConstant", mipSize);
				cmdBindDescriptorSet(pCmd, i - 1, pDescriptorSetPTGenMips);

				uint32_t groupCountX = mipSizeX / 16;
				uint32_t groupCountY = mipSizeY / 16;
				if (groupCountX == 0)
					groupCountX = 1;
				if (groupCountY == 0)
					groupCountY = 1;
				cmdDispatch(pCmd, groupCountX, groupCountY, 1);
			}

			barrier = { rt, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
			cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, &barrier);
		}
#endif

		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);
	}

	void AlphaBlendTransparentPass(Cmd* pCmd)
	{
		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 1, &pRenderTargetScreen, pRenderTargetDepth, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)pRenderTargetScreen->mWidth, (float)pRenderTargetScreen->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetScreen->mWidth, pRenderTargetScreen->mHeight);

		// Draw the transparent geometry.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Draw transparent geometry");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render transparent geometry");

		cmdBindPipeline(pCmd, pPipelineTransparentForward);
		cmdBindDescriptorSet(pCmd, SHADE_FORWARD, pDescriptorSetShade);
		cmdBindDescriptorSet(pCmd, UNIFORM_SET(gFrameIndex, VIEW_CAMERA, GEOM_TRANSPARENT), pDescriptorSetUniforms);
		DrawObjects(pCmd, &gTransparentDrawCalls, pRootSignature);

		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);
	}

	void WeightedBlendedOrderIndependentTransparencyPass(Cmd* pCmd, bool volition)
	{
		Pipeline*      pShadePipeline = volition ? pPipelineWBOITVShade : pPipelineWBOITShade;
		Pipeline*      pCompositePipeline = volition ? pPipelineWBOITVComposite : pPipelineWBOITComposite;

		RenderTargetBarrier textureBarriers[WBOIT_RT_COUNT] = {};
		for (int i = 0; i < WBOIT_RT_COUNT; ++i)
		{
			textureBarriers[i].pRenderTarget = pRenderTargetWBOIT[i];
			textureBarriers[i].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			textureBarriers[i].mNewState = RESOURCE_STATE_RENDER_TARGET;
		}
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, WBOIT_RT_COUNT, textureBarriers);

		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
		loadActions.mClearColorValues[0] = pRenderTargetWBOIT[WBOIT_RT_ACCUMULATION]->mClearValue;
		loadActions.mLoadActionsColor[1] = LOAD_ACTION_CLEAR;
		loadActions.mClearColorValues[1] = pRenderTargetWBOIT[WBOIT_RT_REVEALAGE]->mClearValue;
		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, WBOIT_RT_COUNT, pRenderTargetWBOIT, pRenderTargetDepth, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(
			pCmd, 0.0f, 0.0f, (float)pRenderTargetWBOIT[0]->mWidth, (float)pRenderTargetWBOIT[0]->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetWBOIT[0]->mWidth, pRenderTargetWBOIT[0]->mHeight);

		// Draw the transparent geometry.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Draw transparent geometry (WBOIT)");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render transparent geometry (WBOIT)");

		cmdBindPipeline(pCmd, pShadePipeline);
		cmdBindDescriptorSet(pCmd, SHADE_FORWARD, pDescriptorSetShade);
		cmdBindDescriptorSet(pCmd, UNIFORM_SET(gFrameIndex, VIEW_CAMERA, GEOM_TRANSPARENT), pDescriptorSetUniforms);
		DrawObjects(pCmd, &gTransparentDrawCalls, pRootSignature);

		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);

		// Composite WBOIT buffers
		for (int i = 0; i < WBOIT_RT_COUNT; ++i)
		{
			textureBarriers[i].pRenderTarget = pRenderTargetWBOIT[i];
			textureBarriers[i].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			textureBarriers[i].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, WBOIT_RT_COUNT, textureBarriers);

		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 1, &pRenderTargetScreen, NULL, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)pRenderTargetScreen->mWidth, (float)pRenderTargetScreen->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetScreen->mWidth, pRenderTargetScreen->mHeight);

		// Draw the transparent geometry.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Composite WBOIT buffers");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Composite WBOIT buffers");

		cmdBindPipeline(pCmd, pCompositePipeline);
		cmdBindDescriptorSet(pCmd, 0, pDescriptorSetWBOITComposite);
		cmdDraw(pCmd, 3, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);
	}

	void PhenomenologicalTransparencyPass(Cmd* pCmd)
	{
		RenderTargetBarrier  textureBarriers[PT_RT_COUNT + 1] = {};
		LoadActionsDesc loadActions = {};

#if PT_USE_DIFFUSION != 0
		// Copy depth buffer
		textureBarriers[0].pRenderTarget = pRenderTargetDepth;
		textureBarriers[0].mCurrentState = RESOURCE_STATE_DEPTH_WRITE;
		textureBarriers[0].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		textureBarriers[1].pRenderTarget = pRenderTargetPTDepthCopy;
		textureBarriers[1].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		textureBarriers[1].mNewState = RESOURCE_STATE_RENDER_TARGET;
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, textureBarriers);

		loadActions.mLoadActionsColor[0] = LOAD_ACTION_DONTCARE;
		loadActions.mClearColorValues[0] = pRenderTargetPTDepthCopy->mClearValue;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 1, &pRenderTargetPTDepthCopy, NULL, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(
			pCmd, 0.0f, 0.0f, (float)pRenderTargetPTDepthCopy->mWidth, (float)pRenderTargetPTDepthCopy->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetPTDepthCopy->mWidth, pRenderTargetPTDepthCopy->mHeight);

		// Draw the transparent geometry.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "PT Copy depth buffer");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "PT Copy depth buffer");

		cmdBindPipeline(pCmd, pPipelinePTCopyDepth);
		cmdBindDescriptorSet(pCmd, 0, pDescriptorSetPTCopyDepth);
		cmdDraw(pCmd, 3, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);

		textureBarriers[0].pRenderTarget = pRenderTargetDepth;
		textureBarriers[0].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		textureBarriers[0].mNewState = RESOURCE_STATE_DEPTH_WRITE;
		textureBarriers[1].pRenderTarget = pRenderTargetPTDepthCopy;
		textureBarriers[1].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
		textureBarriers[1].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 2, textureBarriers);
#endif

		for (int i = 0; i < PT_RT_COUNT; ++i)
		{
			textureBarriers[i].pRenderTarget = pRenderTargetPT[i];
			textureBarriers[i].mCurrentState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			textureBarriers[i].mNewState = RESOURCE_STATE_RENDER_TARGET;
		}
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, PT_RT_COUNT, textureBarriers);

		loadActions = {};
		for (int i = 0; i < PT_RT_COUNT; ++i)
		{
			loadActions.mLoadActionsColor[i] = LOAD_ACTION_CLEAR;
			loadActions.mClearColorValues[i] = pRenderTargetPT[i]->mClearValue;
		}
		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, PT_RT_COUNT, pRenderTargetPT, pRenderTargetDepth, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)pRenderTargetPT[0]->mWidth, (float)pRenderTargetPT[0]->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetPT[0]->mWidth, pRenderTargetPT[0]->mHeight);

		// Draw the transparent geometry.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Draw transparent geometry (PT)");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render transparent geometry (PT)");

		cmdBindPipeline(pCmd, pPipelinePTShade);
		cmdBindDescriptorSet(pCmd, SHADE_PT, pDescriptorSetShade);
		cmdBindDescriptorSet(pCmd, UNIFORM_SET(gFrameIndex, VIEW_CAMERA, GEOM_TRANSPARENT), pDescriptorSetUniforms);
		DrawObjects(pCmd, &gTransparentDrawCalls, pRootSignature);

		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);

		// Composite PT buffers
		for (uint32_t i = 0; i < PT_RT_COUNT; ++i)
		{
			textureBarriers[i].pRenderTarget = pRenderTargetPT[i];
			textureBarriers[i].mCurrentState = RESOURCE_STATE_RENDER_TARGET;
			textureBarriers[i].mNewState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		}
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, PT_RT_COUNT, textureBarriers);

		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 1, &pRenderTargetScreen, NULL, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)pRenderTargetScreen->mWidth, (float)pRenderTargetScreen->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetScreen->mWidth, pRenderTargetScreen->mHeight);

		// Draw the transparent geometry.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Composite PT buffers");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Composite PT buffers");

		cmdBindPipeline(pCmd, pPipelinePTComposite);
		cmdBindDescriptorSet(pCmd, 0, pDescriptorSetPTComposite);
		cmdDraw(pCmd, 3, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);
	}

#if AOIT_ENABLE
	void AdaptiveOrderIndependentTransparency(Cmd* pCmd)
	{
		TextureBarrier textureBarrier = { pTextureAOITClearMask, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS };
		BufferBarrier bufferBarriers[2] = {
			{ pBufferAOITColorData, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS },
#if AOIT_NODE_COUNT != 2
			{ pBufferAOITDepthData, RESOURCE_STATE_PIXEL_SHADER_RESOURCE, RESOURCE_STATE_UNORDERED_ACCESS },
#endif
		};
		cmdResourceBarrier(pCmd, sizeof(bufferBarriers) / sizeof(bufferBarriers[0]), bufferBarriers, 1, &textureBarrier, 0, NULL);

		// Clear AOIT buffers
		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionDepth = LOAD_ACTION_DONTCARE;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 0, NULL, pRenderTargetDepth, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)pRenderTargetScreen->mWidth, (float)pRenderTargetScreen->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetScreen->mWidth, pRenderTargetScreen->mHeight);

		// Draw fullscreen quad.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Clear AOIT buffers");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Clear AOIT buffers");

		cmdBindPipeline(pCmd, pPipelineAOITClear);
		cmdBindDescriptorSet(pCmd, 0, pDescriptorSetAOITClear);
		cmdDraw(pCmd, 3, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);

		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 0, NULL, pRenderTargetDepth, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(
			pCmd, 0.0f, 0.0f, (float)pSwapChain->ppRenderTargets[0]->mWidth,
			(float)pSwapChain->ppRenderTargets[0]->mHeight, 0.0f, 1.0f);
		cmdSetScissor(
			pCmd, 0, 0, pSwapChain->ppRenderTargets[0]->mWidth, pSwapChain->ppRenderTargets[0]->mHeight);

		// Draw the transparent geometry.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Draw transparent geometry (AOIT)");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Render transparent geometry (AOIT)");

		cmdBindPipeline(pCmd, pPipelineAOITShade);
		cmdBindDescriptorSet(pCmd, 0, pDescriptorSetAOITShade[0]);
		cmdBindDescriptorSet(pCmd, gFrameIndex, pDescriptorSetAOITShade[1]);
		DrawObjects(pCmd, &gTransparentDrawCalls, pRootSignatureAOITShade);

		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);

		// Composite AOIT buffers
		textureBarrier = { pTextureAOITClearMask, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
		bufferBarriers[0] = { pBufferAOITColorData, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
#if AOIT_NODE_COUNT != 2
		bufferBarriers[1] = { pBufferAOITDepthData, RESOURCE_STATE_UNORDERED_ACCESS, RESOURCE_STATE_PIXEL_SHADER_RESOURCE };
#endif
		cmdResourceBarrier(pCmd, sizeof(bufferBarriers) / sizeof(bufferBarriers[0]), bufferBarriers, 1, &textureBarrier, 0, NULL);

		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		loadActions.mLoadActionDepth = LOAD_ACTION_LOAD;

		// Start render pass and apply load actions
		cmdBindRenderTargets(pCmd, 1, &pRenderTargetScreen, NULL, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(pCmd, 0.0f, 0.0f, (float)pRenderTargetScreen->mWidth, (float)pRenderTargetScreen->mHeight, 0.0f, 1.0f);
		cmdSetScissor(pCmd, 0, 0, pRenderTargetScreen->mWidth, pRenderTargetScreen->mHeight);

		// Draw fullscreen quad.
		cmdBeginDebugMarker(pCmd, 1, 0, 1, "Composite AOIT buffers");
		cmdBeginGpuTimestampQuery(pCmd, gGpuProfileToken, "Composite AOIT buffers");

		cmdBindPipeline(pCmd, pPipelineAOITComposite);
		cmdBindDescriptorSet(pCmd, 0, pDescriptorSetAOITComposite);
		cmdDraw(pCmd, 3, 0);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndGpuTimestampQuery(pCmd, gGpuProfileToken);
		cmdEndDebugMarker(pCmd);
	}
#endif

	void Draw() override
	{
		uint32_t swapchainImageIndex;
		acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

		Semaphore* pRenderCompleteSemaphore = pRenderCompleteSemaphores[gFrameIndex];
		Fence*     pRenderCompleteFence = pRenderCompleteFences[gFrameIndex];

		// Stall if CPU is running "Swap Chain Buffer Count" frames ahead of GPU
		FenceStatus fenceStatus;
		getFenceStatus(pRenderer, pRenderCompleteFence, &fenceStatus);
		if (fenceStatus == FENCE_STATUS_INCOMPLETE)
			waitForFences(pRenderer, 1, &pRenderCompleteFence);

		resetCmdPool(pRenderer, pCmdPools[gFrameIndex]);

		gCpuTimer.GetUSec(true);
		/************************************************************************/
		// Update uniform buffers
		/************************************************************************/
		BufferUpdateDesc materialBufferUpdateDesc = { pBufferMaterials[gFrameIndex] };
		beginUpdateResource(&materialBufferUpdateDesc);
		*(MaterialUniformBlock*)materialBufferUpdateDesc.pMappedData = gMaterialUniformData;
		endUpdateResource(&materialBufferUpdateDesc, NULL);
		BufferUpdateDesc opaqueBufferUpdateDesc = { pBufferOpaqueObjectTransforms[gFrameIndex] };
		beginUpdateResource(&opaqueBufferUpdateDesc);
		*(ObjectInfoUniformBlock*)opaqueBufferUpdateDesc.pMappedData = gObjectInfoUniformData;
		endUpdateResource(&opaqueBufferUpdateDesc, NULL);
		BufferUpdateDesc transparentBufferUpdateDesc = { pBufferTransparentObjectTransforms[gFrameIndex] };
		beginUpdateResource(&transparentBufferUpdateDesc);
		*(ObjectInfoUniformBlock*)transparentBufferUpdateDesc.pMappedData = gTransparentObjectInfoUniformData;
		endUpdateResource(&transparentBufferUpdateDesc, NULL);

		BufferUpdateDesc cameraCbv = { pBufferCameraUniform[gFrameIndex] };
		beginUpdateResource(&cameraCbv);
		*(CameraUniform*)cameraCbv.pMappedData = gCameraUniformData;
		endUpdateResource(&cameraCbv, NULL);

		BufferUpdateDesc cameraLightBufferCbv = { pBufferCameraLightUniform[gFrameIndex] };
		beginUpdateResource(&cameraLightBufferCbv);
		*(CameraUniform*)cameraLightBufferCbv.pMappedData = gCameraLightUniformData;
		endUpdateResource(&cameraLightBufferCbv, NULL);

		BufferUpdateDesc skyboxViewProjCbv = { pBufferSkyboxUniform[gFrameIndex] };
		beginUpdateResource(&skyboxViewProjCbv);
		*(SkyboxUniformBlock*)skyboxViewProjCbv.pMappedData = gSkyboxUniformData;
		endUpdateResource(&skyboxViewProjCbv, NULL);

		BufferUpdateDesc lightBufferCbv = { pBufferLightUniform[gFrameIndex] };
		beginUpdateResource(&lightBufferCbv);
		*(LightUniformBlock*)lightBufferCbv.pMappedData = gLightUniformData;
		endUpdateResource(&lightBufferCbv, NULL);
		/************************************************************************/
		// Update transparency settings
		/************************************************************************/
		if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT)
		{
			BufferUpdateDesc wboitSettingsUpdateDesc = { pBufferWBOITSettings[gFrameIndex] };
			beginUpdateResource(&wboitSettingsUpdateDesc);
			*(WBOITSettings*)wboitSettingsUpdateDesc.pMappedData = gWBOITSettingsData;
			endUpdateResource(&wboitSettingsUpdateDesc, NULL);
		}
		else if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION)
		{
			BufferUpdateDesc wboitSettingsUpdateDesc = { pBufferWBOITSettings[gFrameIndex] };
			beginUpdateResource(&wboitSettingsUpdateDesc);
			*(WBOITVolitionSettings*)wboitSettingsUpdateDesc.pMappedData = gWBOITVolitionSettingsData;
			endUpdateResource(&wboitSettingsUpdateDesc, NULL);
		}
		/************************************************************************/
		// Rendering
		/************************************************************************/
		// Get command list to store rendering commands for this frame
		Cmd* pCmd = pCmds[gFrameIndex];

		pRenderTargetScreen = pSwapChain->ppRenderTargets[swapchainImageIndex];
		beginCmd(pCmd);
		cmdBeginGpuFrameProfile(pCmd, gGpuProfileToken);
		RenderTargetBarrier barriers1[] = {
			{ pRenderTargetScreen, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
		};
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, barriers1);

		DrawSkybox(pCmd);
		ShadowPass(pCmd);
		StochasticShadowPass(pCmd);
		OpaquePass(pCmd);

		if (gTransparencyType == TRANSPARENCY_TYPE_ALPHA_BLEND)
			AlphaBlendTransparentPass(pCmd);
		else if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT)
			WeightedBlendedOrderIndependentTransparencyPass(pCmd, false);
		else if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION)
			WeightedBlendedOrderIndependentTransparencyPass(pCmd, true);
		else if (gTransparencyType == TRANSPARENCY_TYPE_PHENOMENOLOGICAL)
			PhenomenologicalTransparencyPass(pCmd);
#if AOIT_ENABLE
		else if (gTransparencyType == TRANSPARENCY_TYPE_ADAPTIVE_OIT)
			AdaptiveOrderIndependentTransparency(pCmd);
#endif
		else
			ASSERT(false && "Not implemented.");

		////////////////////////////////////////////////////////
		//  Draw UIs
		LoadActionsDesc loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;

		cmdBeginDebugMarker(pCmd, 0, 1, 0, "Draw UI");
		cmdBindRenderTargets(pCmd, 1, &pRenderTargetScreen, NULL, &loadActions, NULL, NULL, -1, -1);


        float2 txtSize = cmdDrawCpuProfile(pCmd, float2(8.0f, 15.0f), &gFrameTimeDraw);
		cmdDrawGpuProfile(pCmd, float2(8.0f, txtSize.y + 30.f), gGpuProfileToken);

		gVirtualJoystick.Draw(pCmd, { 1.0f, 1.0f, 1.0f, 1.0f });

		cmdDrawProfilerUI();

		gAppUI.Gui(pGuiWindow);
		gAppUI.Draw(pCmd);
		cmdBindRenderTargets(pCmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);

		cmdEndDebugMarker(pCmd);
		////////////////////////////////////////////////////////

		barriers1[0] = { pRenderTargetScreen, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
		cmdResourceBarrier(pCmd, 0, NULL, 0, NULL, 1, barriers1);

		cmdEndGpuFrameProfile(pCmd, gGpuProfileToken);
		endCmd(pCmd);

		QueueSubmitDesc submitDesc = {};
		submitDesc.mCmdCount = 1;
		submitDesc.mSignalSemaphoreCount = 1;
		submitDesc.mWaitSemaphoreCount = 1;
		submitDesc.ppCmds = &pCmd;
		submitDesc.ppSignalSemaphores = &pRenderCompleteSemaphore;
		submitDesc.ppWaitSemaphores = &pImageAcquiredSemaphore;
		submitDesc.pSignalFence = pRenderCompleteFence;
		queueSubmit(pGraphicsQueue, &submitDesc);
		QueuePresentDesc presentDesc = {};
		presentDesc.mIndex = swapchainImageIndex;
		presentDesc.mWaitSemaphoreCount = 1;
		presentDesc.ppWaitSemaphores = &pRenderCompleteSemaphore;
		presentDesc.pSwapChain = pSwapChain;
		presentDesc.mSubmitDone = true;
		PresentStatus presentStatus = queuePresent(pGraphicsQueue, &presentDesc);
		flipProfiler();

		if (presentStatus == PRESENT_STATUS_DEVICE_RESET)
		{
			Thread::Sleep(5000);// Wait for a few seconds to allow the driver to come back online before doing a reset.
			mSettings.mResetGraphics = true;
		}

		// Test re-creating graphics resources mid app.
		if (gTestGraphicsReset)
		{
			mSettings.mResetGraphics = true;
			gTestGraphicsReset = false;
		}

		gFrameIndex = (gFrameIndex + 1) % gImageCount;
	}

	const char* GetName() override { return "15_Transparency"; }

	/************************************************************************/
	// Init and Exit functions
	/************************************************************************/
	void CreateSamplers()
	{
		SamplerDesc samplerPointDesc = {};
		addSampler(pRenderer, &samplerPointDesc, &pSamplerPoint);

		SamplerDesc samplerPointClampDesc = {};
		samplerPointClampDesc.mAddressU = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerPointClampDesc.mAddressV = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerPointClampDesc.mAddressW = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerPointClampDesc.mMinFilter = FILTER_NEAREST;
		samplerPointClampDesc.mMagFilter = FILTER_NEAREST;
		samplerPointClampDesc.mMipMapMode = MIPMAP_MODE_NEAREST;
		addSampler(pRenderer, &samplerPointClampDesc, &pSamplerPointClamp);

		SamplerDesc samplerBiliniearDesc = {};
		samplerBiliniearDesc.mAddressU = ADDRESS_MODE_REPEAT;
		samplerBiliniearDesc.mAddressV = ADDRESS_MODE_REPEAT;
		samplerBiliniearDesc.mAddressW = ADDRESS_MODE_REPEAT;
		samplerBiliniearDesc.mMinFilter = FILTER_LINEAR;
		samplerBiliniearDesc.mMagFilter = FILTER_LINEAR;
		samplerBiliniearDesc.mMipMapMode = MIPMAP_MODE_LINEAR;
		addSampler(pRenderer, &samplerBiliniearDesc, &pSamplerBilinear);

		SamplerDesc samplerTrilinearAnisoDesc = {};
		samplerTrilinearAnisoDesc.mAddressU = ADDRESS_MODE_REPEAT;
		samplerTrilinearAnisoDesc.mAddressV = ADDRESS_MODE_REPEAT;
		samplerTrilinearAnisoDesc.mAddressW = ADDRESS_MODE_REPEAT;
		samplerTrilinearAnisoDesc.mMinFilter = FILTER_LINEAR;
		samplerTrilinearAnisoDesc.mMagFilter = FILTER_LINEAR;
		samplerTrilinearAnisoDesc.mMipMapMode = MIPMAP_MODE_LINEAR;
		samplerTrilinearAnisoDesc.mMipLodBias = 0.0f;
		samplerTrilinearAnisoDesc.mMaxAnisotropy = 8.0f;
		addSampler(pRenderer, &samplerTrilinearAnisoDesc, &pSamplerTrilinearAniso);

		SamplerDesc samplerpSamplerSkyboxDesc = {};
		samplerpSamplerSkyboxDesc.mAddressU = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerpSamplerSkyboxDesc.mAddressV = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerpSamplerSkyboxDesc.mAddressW = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerpSamplerSkyboxDesc.mMinFilter = FILTER_LINEAR;
		samplerpSamplerSkyboxDesc.mMagFilter = FILTER_LINEAR;
		samplerpSamplerSkyboxDesc.mMipMapMode = MIPMAP_MODE_NEAREST;
		addSampler(pRenderer, &samplerpSamplerSkyboxDesc, &pSamplerSkybox);

#if USE_SHADOWS != 0
		SamplerDesc samplerShadowDesc = {};
		samplerShadowDesc.mAddressU = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerShadowDesc.mAddressV = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerShadowDesc.mAddressW = ADDRESS_MODE_CLAMP_TO_EDGE;
		samplerShadowDesc.mMinFilter = FILTER_LINEAR;
		samplerShadowDesc.mMagFilter = FILTER_LINEAR;
		samplerShadowDesc.mMipMapMode = MIPMAP_MODE_LINEAR;
		addSampler(pRenderer, &samplerShadowDesc, &pSamplerShadow);
#endif
	}

	void DestroySamplers()
	{
		removeSampler(pRenderer, pSamplerTrilinearAniso);
		removeSampler(pRenderer, pSamplerBilinear);
		removeSampler(pRenderer, pSamplerPointClamp);
		removeSampler(pRenderer, pSamplerSkybox);
		removeSampler(pRenderer, pSamplerPoint);
#if USE_SHADOWS != 0
		removeSampler(pRenderer, pSamplerShadow);
#endif
	}

	void CreateShaders()
	{
		// Define shader macros
		char maxNumObjectsMacroBuffer[5] = {}; sprintf(maxNumObjectsMacroBuffer, "%i", MAX_NUM_OBJECTS);
		char maxNumTexturesMacroBuffer[5] = {}; sprintf(maxNumTexturesMacroBuffer, "%i", TEXTURE_COUNT);
		char aoitNodeCountMacroBuffer[5] = {}; sprintf(aoitNodeCountMacroBuffer, "%i", AOIT_NODE_COUNT);
		char useShadowsMacroBuffer[5] = {}; sprintf(useShadowsMacroBuffer, "%i", USE_SHADOWS);
		char useRefractionMacroBuffer[5] = {}; sprintf(useRefractionMacroBuffer, "%i", PT_USE_REFRACTION);
		char useDiffusionMacroBuffer[5] = {}; sprintf(useDiffusionMacroBuffer, "%i", PT_USE_DIFFUSION);
		char useCausticsMacroBuffer[5] = {}; sprintf(useCausticsMacroBuffer, "%i", PT_USE_CAUSTICS);

		ShaderMacro maxNumObjectsMacro = { "MAX_NUM_OBJECTS", maxNumObjectsMacroBuffer };
		ShaderMacro maxNumTexturesMacro = { "MAX_NUM_TEXTURES", maxNumTexturesMacroBuffer };
		ShaderMacro aoitNodeCountMacro = { "AOIT_NODE_COUNT", aoitNodeCountMacroBuffer };
		ShaderMacro useShadowsMacro = { "USE_SHADOWS", useShadowsMacroBuffer };
		ShaderMacro useRefractionMacro = { "PT_USE_REFRACTION", useRefractionMacroBuffer };
		ShaderMacro useDiffusionMacro = { "PT_USE_DIFFUSION", useDiffusionMacroBuffer };
		ShaderMacro useCausticsMacro = { "PT_USE_CAUSTICS", useCausticsMacroBuffer };

		ShaderMacro shaderMacros[] = { maxNumObjectsMacro, maxNumTexturesMacro, aoitNodeCountMacro, useShadowsMacro,
									   useRefractionMacro, useDiffusionMacro,   useCausticsMacro };
		const uint  numShaderMacros = sizeof(shaderMacros) / sizeof(shaderMacros[0]);

		// Skybox shader
		ShaderLoadDesc skyboxShaderDesc = {};
		skyboxShaderDesc.mStages[0] = { "skybox.vert", shaderMacros, numShaderMacros };
		skyboxShaderDesc.mStages[1] = { "skybox.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &skyboxShaderDesc, &pShaderSkybox);

#if USE_SHADOWS != 0
		// Shadow mapping shader
		ShaderLoadDesc shadowShaderDesc = {};
		shadowShaderDesc.mStages[0] = { "shadow.vert", shaderMacros, numShaderMacros };
		shadowShaderDesc.mStages[1] = { "shadow.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &shadowShaderDesc, &pShaderShadow);

		// Gaussian blur shader
		ShaderLoadDesc blurShaderDesc = {};
		blurShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
		blurShaderDesc.mStages[1] = { "gaussianBlur.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &blurShaderDesc, &pShaderGaussianBlur);

#if PT_USE_CAUSTICS != 0
		// Stochastic shadow mapping shader
		ShaderLoadDesc stochasticShadowShaderDesc = {};
		stochasticShadowShaderDesc.mStages[0] = { "forward.vert", shaderMacros, numShaderMacros };
		stochasticShadowShaderDesc.mStages[1] = { "stochasticShadow.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &stochasticShadowShaderDesc, &pShaderPTShadow);

		// Downsample shader
		ShaderLoadDesc downsampleShaderDesc = {};
		downsampleShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
		downsampleShaderDesc.mStages[1] = { "downsample.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &downsampleShaderDesc, &pShaderPTDownsample);

		// Shadow map copy shader
		ShaderLoadDesc copyShadowDepthShaderDesc = {};
		copyShadowDepthShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
		copyShadowDepthShaderDesc.mStages[1] = { "copy.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &copyShadowDepthShaderDesc, &pShaderPTCopyShadowDepth);
#endif
#endif

		// Forward shading shader
		ShaderLoadDesc forwardShaderDesc = {};
		forwardShaderDesc.mStages[0] = { "forward.vert", shaderMacros, numShaderMacros };
		forwardShaderDesc.mStages[1] = { "forward.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &forwardShaderDesc, &pShaderForward);

		// WBOIT shade shader
		ShaderLoadDesc wboitShadeShaderDesc = {};
		wboitShadeShaderDesc.mStages[0] = { "forward.vert", shaderMacros, numShaderMacros };
		wboitShadeShaderDesc.mStages[1] = { "weightedBlendedOIT.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &wboitShadeShaderDesc, &pShaderWBOITShade);

		// WBOIT composite shader
		ShaderLoadDesc wboitCompositeShaderDesc = {};
		wboitCompositeShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
		wboitCompositeShaderDesc.mStages[1] = { "weightedBlendedOITComposite.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &wboitCompositeShaderDesc, &pShaderWBOITComposite);

		// WBOIT Volition shade shader
		ShaderLoadDesc wboitVolitionShadeShaderDesc = {};
		wboitVolitionShadeShaderDesc.mStages[0] = { "forward.vert", shaderMacros, numShaderMacros };
		wboitVolitionShadeShaderDesc.mStages[1] = { "weightedBlendedOITVolition.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &wboitVolitionShadeShaderDesc, &pShaderWBOITVShade);

		// WBOIT Volition composite shader
		ShaderLoadDesc wboitVolitionCompositeShaderDesc = {};
		wboitVolitionCompositeShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
		wboitVolitionCompositeShaderDesc.mStages[1] = { "weightedBlendedOITVolitionComposite.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &wboitVolitionCompositeShaderDesc, &pShaderWBOITVComposite);

		// PT shade shader
		ShaderMacro ptShaderMacros[numShaderMacros + 1];
		for (int i = 0; i < numShaderMacros; ++i)
			ptShaderMacros[i] = shaderMacros[i];
		ptShaderMacros[numShaderMacros] = { "PHENOMENOLOGICAL_TRANSPARENCY", "" };
		ShaderLoadDesc ptShadeShaderDesc = {};
		ptShadeShaderDesc.mStages[0] = { "forward.vert", ptShaderMacros, numShaderMacros + 1 };
		ptShadeShaderDesc.mStages[1] = { "phenomenologicalTransparency.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &ptShadeShaderDesc, &pShaderPTShade);

		// PT composite shader
		ShaderLoadDesc ptCompositeShaderDesc = {};
		ptCompositeShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
		ptCompositeShaderDesc.mStages[1] = { "phenomenologicalTransparencyComposite.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &ptCompositeShaderDesc, &pShaderPTComposite);

#if PT_USE_DIFFUSION != 0
		// PT copy depth shader
		ShaderLoadDesc ptCopyShaderDesc = {};
		ptCopyShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
		ptCopyShaderDesc.mStages[1] = { "copy.frag", shaderMacros, numShaderMacros };
		addShader(pRenderer, &ptCopyShaderDesc, &pShaderPTCopyDepth);

		// PT generate mips shader
		ShaderLoadDesc ptGenMipsShaderDesc = {};
		ptGenMipsShaderDesc.mStages[0] = { "generateMips.comp", shaderMacros, numShaderMacros };
		addShader(pRenderer, &ptGenMipsShaderDesc, &pShaderPTGenMips);
#endif

#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			// AOIT shade shader
			ShaderLoadDesc aoitShadeShaderDesc = {};
			aoitShadeShaderDesc.mStages[0] = { "forward.vert", shaderMacros, numShaderMacros };
			aoitShadeShaderDesc.mStages[1] = { "adaptiveOIT.frag", shaderMacros, numShaderMacros };
			addShader(pRenderer, &aoitShadeShaderDesc, &pShaderAOITShade);

			// AOIT composite shader
			ShaderLoadDesc aoitCompositeShaderDesc = {};
			aoitCompositeShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
			aoitCompositeShaderDesc.mStages[1] = { "adaptiveOITComposite.frag", shaderMacros, numShaderMacros };
			addShader(pRenderer, &aoitCompositeShaderDesc, &pShaderAOITComposite);

			// AOIT clear shader
			ShaderLoadDesc aoitClearShaderDesc = {};
			aoitClearShaderDesc.mStages[0] = { "fullscreen.vert", shaderMacros, numShaderMacros };
			aoitClearShaderDesc.mStages[1] = { "adaptiveOITClear.frag", shaderMacros, numShaderMacros };
			addShader(pRenderer, &aoitClearShaderDesc, &pShaderAOITClear);
		}
#endif
	}

	void DestroyShaders()
	{
		removeShader(pRenderer, pShaderSkybox);
#if USE_SHADOWS != 0
		removeShader(pRenderer, pShaderShadow);
		removeShader(pRenderer, pShaderGaussianBlur);
#if PT_USE_CAUSTICS != 0
		removeShader(pRenderer, pShaderPTShadow);
		removeShader(pRenderer, pShaderPTDownsample);
		removeShader(pRenderer, pShaderPTCopyShadowDepth);
#endif
#endif
		removeShader(pRenderer, pShaderForward);
		removeShader(pRenderer, pShaderWBOITShade);
		removeShader(pRenderer, pShaderWBOITComposite);
		removeShader(pRenderer, pShaderWBOITVShade);
		removeShader(pRenderer, pShaderWBOITVComposite);
		removeShader(pRenderer, pShaderPTShade);
		removeShader(pRenderer, pShaderPTComposite);
#if PT_USE_DIFFUSION != 0
		removeShader(pRenderer, pShaderPTCopyDepth);
		removeShader(pRenderer, pShaderPTGenMips);
#endif
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			removeShader(pRenderer, pShaderAOITShade);
			removeShader(pRenderer, pShaderAOITComposite);
			removeShader(pRenderer, pShaderAOITClear);
		}
#endif
	}

	void CreateRootSignatures()
	{
		// Define static samplers
		const char* skyboxSamplerName = "SkySampler";
		const char* pointSamplerName = "PointSampler";
		const char* linearSamplerName = "LinearSampler";
		const char* shadowSamplerName = USE_SHADOWS ? "VSMSampler" : 0;

		Sampler*    staticSamplers[] = { pSamplerSkybox, pSamplerPoint, pSamplerBilinear, pSamplerShadow };
		const char* staticSamplerNames[] = { skyboxSamplerName, pointSamplerName, linearSamplerName, shadowSamplerName };
		const uint  numStaticSamplers = sizeof(staticSamplers) / sizeof(staticSamplers[0]);

		// Skybox root signature
		RootSignatureDesc skyboxRootSignatureDesc = {};
		skyboxRootSignatureDesc.ppShaders = &pShaderSkybox;
		skyboxRootSignatureDesc.mShaderCount = 1;
		skyboxRootSignatureDesc.ppStaticSamplers = staticSamplers;
		skyboxRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		skyboxRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		skyboxRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &skyboxRootSignatureDesc, &pRootSignatureSkybox);

#if USE_SHADOWS != 0
		// Shadow mapping root signature
		RootSignatureDesc blurRootSignatureDesc = {};
		blurRootSignatureDesc.ppShaders = &pShaderGaussianBlur;
		blurRootSignatureDesc.mShaderCount = 1;
		blurRootSignatureDesc.ppStaticSamplers = staticSamplers;
		blurRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		blurRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		blurRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &blurRootSignatureDesc, &pRootSignatureGaussianBlur);

#if PT_USE_CAUSTICS != 0
		// Shadow downsample root signature
		RootSignatureDesc downsampleRootSignatureDesc = {};
		downsampleRootSignatureDesc.ppShaders = &pShaderPTDownsample;
		downsampleRootSignatureDesc.mShaderCount = 1;
		downsampleRootSignatureDesc.ppStaticSamplers = staticSamplers;
		downsampleRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		downsampleRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		downsampleRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &downsampleRootSignatureDesc, &pRootSignaturePTDownsample);

		// Copy shadow root signature
		RootSignatureDesc copyShadowDepthRootSignatureDesc = {};
		copyShadowDepthRootSignatureDesc.ppShaders = &pShaderPTCopyShadowDepth;
		copyShadowDepthRootSignatureDesc.mShaderCount = 1;
		copyShadowDepthRootSignatureDesc.ppStaticSamplers = staticSamplers;
		copyShadowDepthRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		copyShadowDepthRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		copyShadowDepthRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &copyShadowDepthRootSignatureDesc, &pRootSignaturePTCopyShadowDepth);
#endif
#endif

		Shader* pShaders[] =
		{
			pShaderShadow, pShaderWBOITShade, pShaderWBOITVShade, pShaderForward, pShaderPTShade,
#if PT_USE_CAUSTICS
			pShaderPTShadow
#endif
		};
		// Forward shading root signature
		RootSignatureDesc forwardRootSignatureDesc = {};
		forwardRootSignatureDesc.ppShaders = pShaders;
		forwardRootSignatureDesc.mShaderCount = sizeof(pShaders) / sizeof(pShaders[0]);
		forwardRootSignatureDesc.ppStaticSamplers = staticSamplers;
		forwardRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		forwardRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		forwardRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &forwardRootSignatureDesc, &pRootSignature);

		// WBOIT composite root signature
		Shader* pShadersWBOITComposite[2] = { pShaderWBOITComposite, pShaderWBOITVComposite };
		RootSignatureDesc wboitCompositeRootSignatureDesc = {};
		wboitCompositeRootSignatureDesc.ppShaders = pShadersWBOITComposite;
		wboitCompositeRootSignatureDesc.mShaderCount = 2;
		wboitCompositeRootSignatureDesc.ppStaticSamplers = staticSamplers;
		wboitCompositeRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		wboitCompositeRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		wboitCompositeRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &wboitCompositeRootSignatureDesc, &pRootSignatureWBOITComposite);

		// PT composite root signature
		RootSignatureDesc ptCompositeRootSignatureDesc = {};
		ptCompositeRootSignatureDesc.ppShaders = &pShaderPTComposite;
		ptCompositeRootSignatureDesc.mShaderCount = 1;
		ptCompositeRootSignatureDesc.ppStaticSamplers = staticSamplers;
		ptCompositeRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		ptCompositeRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		ptCompositeRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &ptCompositeRootSignatureDesc, &pRootSignaturePTComposite);

#if PT_USE_DIFFUSION != 0
		// PT copy depth root signature
		RootSignatureDesc ptCopyDepthRootSignatureDesc = {};
		ptCopyDepthRootSignatureDesc.ppShaders = &pShaderPTCopyDepth;
		ptCopyDepthRootSignatureDesc.mShaderCount = 1;
		ptCopyDepthRootSignatureDesc.ppStaticSamplers = staticSamplers;
		ptCopyDepthRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		ptCopyDepthRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		ptCopyDepthRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &ptCopyDepthRootSignatureDesc, &pRootSignaturePTCopyDepth);

		// PT generate mips root signature
		RootSignatureDesc ptGenMipsRootSignatureDesc = {};
		ptGenMipsRootSignatureDesc.ppShaders = &pShaderPTGenMips;
		ptGenMipsRootSignatureDesc.mShaderCount = 1;
		ptGenMipsRootSignatureDesc.ppStaticSamplers = staticSamplers;
		ptGenMipsRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
		ptGenMipsRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
		ptGenMipsRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
		addRootSignature(pRenderer, &ptGenMipsRootSignatureDesc, &pRootSignaturePTGenMips);
#endif
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			// AOIT shade root signature
			RootSignatureDesc aoitShadeRootSignatureDesc = {};
			aoitShadeRootSignatureDesc.ppShaders = &pShaderAOITShade;
			aoitShadeRootSignatureDesc.mShaderCount = 1;
			aoitShadeRootSignatureDesc.ppStaticSamplers = staticSamplers;
			aoitShadeRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
			aoitShadeRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
			aoitShadeRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
			addRootSignature(pRenderer, &aoitShadeRootSignatureDesc, &pRootSignatureAOITShade);

			// AOIT composite root signature
			RootSignatureDesc aoitCompositeRootSignatureDesc = {};
			aoitCompositeRootSignatureDesc.ppShaders = &pShaderAOITComposite;
			aoitCompositeRootSignatureDesc.mShaderCount = 1;
			aoitCompositeRootSignatureDesc.ppStaticSamplers = staticSamplers;
			aoitCompositeRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
			aoitCompositeRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
			aoitCompositeRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
			addRootSignature(pRenderer, &aoitCompositeRootSignatureDesc, &pRootSignatureAOITComposite);

			// AOIT clear signature
			RootSignatureDesc aoitClearRootSignatureDesc = {};
			aoitClearRootSignatureDesc.ppShaders = &pShaderAOITClear;
			aoitClearRootSignatureDesc.mShaderCount = 1;
			aoitClearRootSignatureDesc.ppStaticSamplers = staticSamplers;
			aoitClearRootSignatureDesc.mStaticSamplerCount = numStaticSamplers;
			aoitClearRootSignatureDesc.ppStaticSamplerNames = staticSamplerNames;
			aoitClearRootSignatureDesc.mMaxBindlessTextures = TEXTURE_COUNT;
			addRootSignature(pRenderer, &aoitClearRootSignatureDesc, &pRootSignatureAOITClear);
		}
#endif
	}

	void DestroyRootSignatures()
	{
		removeRootSignature(pRenderer, pRootSignatureSkybox);
#if USE_SHADOWS != 0
		removeRootSignature(pRenderer, pRootSignature);
		removeRootSignature(pRenderer, pRootSignatureGaussianBlur);
#if PT_USE_CAUSTICS != 0
		removeRootSignature(pRenderer, pRootSignaturePTDownsample);
		removeRootSignature(pRenderer, pRootSignaturePTCopyShadowDepth);
#endif
#endif
		removeRootSignature(pRenderer, pRootSignatureWBOITComposite);
		removeRootSignature(pRenderer, pRootSignaturePTComposite);
#if PT_USE_DIFFUSION != 0
		removeRootSignature(pRenderer, pRootSignaturePTCopyDepth);
		removeRootSignature(pRenderer, pRootSignaturePTGenMips);
#endif
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			removeRootSignature(pRenderer, pRootSignatureAOITShade);
			removeRootSignature(pRenderer, pRootSignatureAOITComposite);
			removeRootSignature(pRenderer, pRootSignatureAOITClear);
		}
#endif
	}

	void CreateDescriptorSets()
	{
		// Skybox
		DescriptorSetDesc setDesc = { pRootSignatureSkybox, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetSkybox[0]);
		setDesc = { pRootSignatureSkybox, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetSkybox[1]);
		// Gaussian blur
		setDesc = { pRootSignatureGaussianBlur, DESCRIPTOR_UPDATE_FREQ_NONE, 2 + (3 * 2) };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetGaussianBlur);
		// Uniforms
		setDesc = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount * 4 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetUniforms);
		// Forward
		setDesc = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_NONE, 3 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetShade);
		// Gen Mips
		setDesc = { pRootSignaturePTGenMips, DESCRIPTOR_UPDATE_FREQ_PER_DRAW, (1 << 5) };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetPTGenMips);
		// WBOIT Composite
		setDesc = { pRootSignatureWBOITComposite, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetWBOITComposite);
		// PT Copy Depth
		setDesc = { pRootSignaturePTCopyDepth, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetPTCopyDepth);
		// PT Composite
		setDesc = { pRootSignaturePTComposite, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetPTComposite);
#if PT_USE_CAUSTICS
		// PT Copy Shadow Depth
		setDesc = { pRootSignaturePTCopyShadowDepth, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetPTCopyShadowDepth);
		// PT Downsample
		setDesc = { pRootSignaturePTDownsample, DESCRIPTOR_UPDATE_FREQ_NONE, 3 };
		addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetPTDownsample);
#endif
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			// AOIT Clear
			setDesc = { pRootSignatureAOITClear, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
			addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetAOITClear);
			// AOIT Shade
			setDesc = { pRootSignatureAOITShade, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
			addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetAOITShade[0]);
			setDesc = { pRootSignatureAOITShade, DESCRIPTOR_UPDATE_FREQ_PER_FRAME, gImageCount };
			addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetAOITShade[1]);
			// AOIT Composite
			setDesc = { pRootSignatureAOITComposite, DESCRIPTOR_UPDATE_FREQ_NONE, 1 };
			addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetAOITComposite);
		}
#endif
	}

	void DestroyDescriptorSets()
	{
		removeDescriptorSet(pRenderer, pDescriptorSetSkybox[0]);
		removeDescriptorSet(pRenderer, pDescriptorSetSkybox[1]);
		removeDescriptorSet(pRenderer, pDescriptorSetGaussianBlur);
		removeDescriptorSet(pRenderer, pDescriptorSetUniforms);
		removeDescriptorSet(pRenderer, pDescriptorSetShade);
		removeDescriptorSet(pRenderer, pDescriptorSetPTGenMips);
		removeDescriptorSet(pRenderer, pDescriptorSetWBOITComposite);
		removeDescriptorSet(pRenderer, pDescriptorSetPTCopyDepth);
		removeDescriptorSet(pRenderer, pDescriptorSetPTComposite);
#if PT_USE_CAUSTICS
		removeDescriptorSet(pRenderer, pDescriptorSetPTCopyShadowDepth);
		removeDescriptorSet(pRenderer, pDescriptorSetPTDownsample);
#endif
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			removeDescriptorSet(pRenderer, pDescriptorSetAOITClear);
			removeDescriptorSet(pRenderer, pDescriptorSetAOITShade[0]);
			removeDescriptorSet(pRenderer, pDescriptorSetAOITShade[1]);
			removeDescriptorSet(pRenderer, pDescriptorSetAOITComposite);
		}
#endif
	}

	void PrepareDescriptorSets()
	{
		// Skybox
		{
			DescriptorData params[6] = {};
			params[0].pName = "RightText";
			params[0].ppTextures = &pTextures[TEXTURE_SKYBOX_RIGHT];
			params[1].pName = "LeftText";
			params[1].ppTextures = &pTextures[TEXTURE_SKYBOX_LEFT];
			params[2].pName = "TopText";
			params[2].ppTextures = &pTextures[TEXTURE_SKYBOX_UP];
			params[3].pName = "BotText";
			params[3].ppTextures = &pTextures[TEXTURE_SKYBOX_DOWN];
			params[4].pName = "FrontText";
			params[4].ppTextures = &pTextures[TEXTURE_SKYBOX_FRONT];
			params[5].pName = "BackText";
			params[5].ppTextures = &pTextures[TEXTURE_SKYBOX_BACK];
			updateDescriptorSet(pRenderer, 0, pDescriptorSetSkybox[0], 6, params);
			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				params[0].pName = "SkyboxUniformBlock";
				params[0].ppBuffers = &pBufferSkyboxUniform[i];
				updateDescriptorSet(pRenderer, i, pDescriptorSetSkybox[1], 1, params);
			}
		}
		// Gaussian blur
		{
			DescriptorData params[2] = {};
			params[0].pName = "Source";
			params[0].ppTextures = &pRenderTargetShadowVariance[0]->pTexture;
			updateDescriptorSet(pRenderer, 0, pDescriptorSetGaussianBlur, 1, params);
			params[0].ppTextures = &pRenderTargetShadowVariance[1]->pTexture;
			updateDescriptorSet(pRenderer, 1, pDescriptorSetGaussianBlur, 1, params);
#if PT_USE_CAUSTICS
			for (uint32_t w = 0; w < 3; ++w)
			{
				params[0].ppTextures = &pRenderTargetPTShadowFinal[0][w]->pTexture;
				updateDescriptorSet(pRenderer, 2 + (w * 2 + 0), pDescriptorSetGaussianBlur, 1, params);
				params[0].ppTextures = &pRenderTargetPTShadowFinal[1][w]->pTexture;
				updateDescriptorSet(pRenderer, 2 + (w * 2 + 1), pDescriptorSetGaussianBlur, 1, params);
			}
#endif
		}
		// Shadow, Forward, WBOIT, PT, AOIT
		{
			uint32_t updateCount = 1;
			DescriptorData params[9] = {};
			params[0].pName = "MaterialTextures";
			params[0].ppTextures = pTextures;
			params[0].mCount = TEXTURE_COUNT;

#if PT_USE_CAUSTICS
			updateDescriptorSet(pRenderer, SHADE_PT_SHADOW, pDescriptorSetShade, updateCount, params);
#endif

#if USE_SHADOWS != 0
			params[1].pName = "VSM";
			params[1].ppTextures = &pRenderTargetShadowVariance[0]->pTexture;
			++updateCount;
#if PT_USE_CAUSTICS != 0
			params[2].pName = "VSMRed";
			params[2].ppTextures = &pRenderTargetPTShadowFinal[0][0]->pTexture;
			params[3].pName = "VSMGreen";
			params[3].ppTextures = &pRenderTargetPTShadowFinal[0][1]->pTexture;
			params[4].pName = "VSMBlue";
			params[4].ppTextures = &pRenderTargetPTShadowFinal[0][2]->pTexture;
			updateCount += 3;
#endif
#endif
			updateDescriptorSet(pRenderer, SHADE_FORWARD, pDescriptorSetShade, updateCount, params);

#if PT_USE_DIFFUSION != 0
			params[updateCount].pName = "DepthTexture";
			params[updateCount].ppTextures = &pRenderTargetPTDepthCopy->pTexture;
#endif
			updateDescriptorSet(pRenderer, SHADE_PT, pDescriptorSetShade, updateCount + 1, params);
#if AOIT_ENABLE
			if (pRenderer->pActiveGpuSettings->mROVsSupported)
			{
				params[updateCount].pName = "AOITClearMaskUAV";
				params[updateCount].ppTextures = &pTextureAOITClearMask;
				params[updateCount + 1].pName = "AOITColorDataUAV";
				params[updateCount + 1].ppBuffers = &pBufferAOITColorData;
				updateCount += 2;
#if AOIT_NODE_COUNT != 2
				params[updateCount].pName = "AOITDepthDataUAV";
				params[updateCount].ppBuffers = &pBufferAOITDepthData;
				++updateCount;
#endif

				updateDescriptorSet(pRenderer, 0, pDescriptorSetAOITShade[0], updateCount, params);
			}
#endif

			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				// Opaque objects
				DescriptorData params[5] = {};
				params[0].pName = "ObjectUniformBlock";
				params[0].ppBuffers = &pBufferOpaqueObjectTransforms[i];
				params[1].pName = "CameraUniform";
				params[1].ppBuffers = &pBufferCameraLightUniform[i];
				params[2].pName = "MaterialUniform";
				params[2].ppBuffers = &pBufferMaterials[i];
				params[3].pName = "LightUniformBlock";
				params[3].ppBuffers = &pBufferLightUniform[i];
				params[4].pName = "WBOITSettings";
				params[4].ppBuffers = &pBufferWBOITSettings[i];

				// View Shadow Geom Opaque
				updateDescriptorSet(pRenderer, UNIFORM_SET(i, VIEW_SHADOW, GEOM_OPAQUE), pDescriptorSetUniforms, 5, params);
				// View Shadow Geom Transparent
				params[0].ppBuffers = &pBufferTransparentObjectTransforms[i];
				updateDescriptorSet(pRenderer, UNIFORM_SET(i, VIEW_SHADOW, GEOM_TRANSPARENT), pDescriptorSetUniforms, 5, params);
				params[0].ppBuffers = &pBufferOpaqueObjectTransforms[i];
				params[1].ppBuffers = &pBufferCameraUniform[i];
				// View Camera Geom Opaque
				updateDescriptorSet(pRenderer, UNIFORM_SET(i, VIEW_CAMERA, GEOM_OPAQUE), pDescriptorSetUniforms, 5, params);
				// View Camera Geom Transparent
				params[0].ppBuffers = &pBufferTransparentObjectTransforms[i];
				updateDescriptorSet(pRenderer, UNIFORM_SET(i, VIEW_CAMERA, GEOM_TRANSPARENT), pDescriptorSetUniforms, 5, params);

#if AOIT_ENABLE
				if (pRenderer->pActiveGpuSettings->mROVsSupported)
					// AOIT
					updateDescriptorSet(pRenderer, i, pDescriptorSetAOITShade[1], 4, params);
#endif
			}
		}
		// Gen Mips
		{
			RenderTarget* rt = pRenderTargetPTBackground;
			for (uint32_t i = 1; i < rt->mMipLevels; ++i)
			{
				DescriptorData params[2] = {};
				params[0].pName = "Source";
				params[0].ppTextures = &rt->pTexture;
				params[0].mUAVMipSlice = i - 1;
				params[1].pName = "Destination";
				params[1].ppTextures = &rt->pTexture;
				params[1].mUAVMipSlice = i;
				updateDescriptorSet(pRenderer, i - 1, pDescriptorSetPTGenMips, 2, params);
			}
		}
		// WBOIT Composite
		{
			DescriptorData compositeParams[2] = {};
			compositeParams[0].pName = "AccumulationTexture";
			compositeParams[0].ppTextures = &pRenderTargetWBOIT[WBOIT_RT_ACCUMULATION]->pTexture;
			compositeParams[1].pName = "RevealageTexture";
			compositeParams[1].ppTextures = &pRenderTargetWBOIT[WBOIT_RT_REVEALAGE]->pTexture;
			updateDescriptorSet(pRenderer, 0, pDescriptorSetWBOITComposite, 2, compositeParams);
		}
		// PT Copy Depth
		{
			DescriptorData copyParams[1] = {};
			copyParams[0].pName = "Source";
			copyParams[0].ppTextures = &pRenderTargetDepth->pTexture;
			updateDescriptorSet(pRenderer, 0, pDescriptorSetPTCopyDepth, 1, copyParams);
		}
		// PT Composite
		{
			uint32_t compositeParamCount = 3;
			DescriptorData compositeParams[4] = {};
			compositeParams[0].pName = "AccumulationTexture";
			compositeParams[0].ppTextures = &pRenderTargetPT[PT_RT_ACCUMULATION]->pTexture;
			compositeParams[1].pName = "ModulationTexture";
			compositeParams[1].ppTextures = &pRenderTargetPT[PT_RT_MODULATION]->pTexture;
			compositeParams[2].pName = "BackgroundTexture";
			compositeParams[2].ppTextures = &pRenderTargetPTBackground->pTexture;
#if PT_USE_REFRACTION != 0
			compositeParams[3].pName = "RefractionTexture";
			compositeParams[3].ppTextures = &pRenderTargetPT[PT_RT_REFRACTION]->pTexture;
			++compositeParamCount;
#endif
			updateDescriptorSet(pRenderer, 0, pDescriptorSetPTComposite, compositeParamCount, compositeParams);
		}
		// PT Shadows
#if PT_USE_CAUSTICS
		{
			DescriptorData params[1] = {};
			params[0].pName = "Source";
			params[0].ppTextures = &pRenderTargetShadowVariance[0]->pTexture;
			updateDescriptorSet(pRenderer, 0, pDescriptorSetPTCopyShadowDepth, 1, params);

			for (uint32_t w = 0; w < 3; ++w)
			{
				DescriptorData params[1] = {};
				params[0].pName = "Source";
				params[0].ppTextures = &pRenderTargetPTShadowVariance[w]->pTexture;
				updateDescriptorSet(pRenderer, w, pDescriptorSetPTDownsample, 1, params);
			}
		}
#endif
		// AOIT
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			DescriptorData clearParams[1] = {};
			clearParams[0].pName = "AOITClearMaskUAV";
			clearParams[0].ppTextures = &pTextureAOITClearMask;
			updateDescriptorSet(pRenderer, 0, pDescriptorSetAOITClear, 1, clearParams);

			DescriptorData compositeParams[2] = {};
			compositeParams[0].pName = "AOITClearMaskSRV";
			compositeParams[0].ppTextures = &pTextureAOITClearMask;
			compositeParams[1].pName = "AOITColorDataSRV";
			compositeParams[1].ppBuffers = &pBufferAOITColorData;
			updateDescriptorSet(pRenderer, 0, pDescriptorSetAOITComposite, 2, compositeParams);
		}
#endif
	}

	void CreateResources()
	{
		LoadTextures();

		const float gSkyboxPointArray[] = {
			10.0f,  -10.0f, -10.0f, 6.0f,    // -z
			-10.0f, -10.0f, -10.0f, 6.0f,   -10.0f, 10.0f,  -10.0f, 6.0f,   -10.0f, 10.0f,
			-10.0f, 6.0f,   10.0f,  10.0f,  -10.0f, 6.0f,   10.0f,  -10.0f, -10.0f, 6.0f,

			-10.0f, -10.0f, 10.0f,  2.0f,    //-x
			-10.0f, -10.0f, -10.0f, 2.0f,   -10.0f, 10.0f,  -10.0f, 2.0f,   -10.0f, 10.0f,
			-10.0f, 2.0f,   -10.0f, 10.0f,  10.0f,  2.0f,   -10.0f, -10.0f, 10.0f,  2.0f,

			10.0f,  -10.0f, -10.0f, 1.0f,    //+x
			10.0f,  -10.0f, 10.0f,  1.0f,   10.0f,  10.0f,  10.0f,  1.0f,   10.0f,  10.0f,
			10.0f,  1.0f,   10.0f,  10.0f,  -10.0f, 1.0f,   10.0f,  -10.0f, -10.0f, 1.0f,

			-10.0f, -10.0f, 10.0f,  5.0f,    // +z
			-10.0f, 10.0f,  10.0f,  5.0f,   10.0f,  10.0f,  10.0f,  5.0f,   10.0f,  10.0f,
			10.0f,  5.0f,   10.0f,  -10.0f, 10.0f,  5.0f,   -10.0f, -10.0f, 10.0f,  5.0f,

			-10.0f, 10.0f,  -10.0f, 3.0f,    //+y
			10.0f,  10.0f,  -10.0f, 3.0f,   10.0f,  10.0f,  10.0f,  3.0f,   10.0f,  10.0f,
			10.0f,  3.0f,   -10.0f, 10.0f,  10.0f,  3.0f,   -10.0f, 10.0f,  -10.0f, 3.0f,

			10.0f,  -10.0f, 10.0f,  4.0f,    //-y
			10.0f,  -10.0f, -10.0f, 4.0f,   -10.0f, -10.0f, -10.0f, 4.0f,   -10.0f, -10.0f,
			-10.0f, 4.0f,   -10.0f, -10.0f, 10.0f,  4.0f,   10.0f,  -10.0f, 10.0f,  4.0f,
		};

		uint64_t       skyBoxDataSize = 4 * 6 * 6 * sizeof(float);
		BufferLoadDesc skyboxVbDesc = {};
		skyboxVbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
		skyboxVbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
		skyboxVbDesc.mDesc.mSize = skyBoxDataSize;
		skyboxVbDesc.pData = gSkyboxPointArray;
		skyboxVbDesc.ppBuffer = &pBufferSkyboxVertex;
		addResource(&skyboxVbDesc, NULL);

#if USE_SHADOWS != 0
		const uint shadowMapResolution = 1024;

		RenderTargetDesc renderTargetDesc = {};
		renderTargetDesc.mArraySize = 1;
		renderTargetDesc.mClearValue = { {1.0f, 1.0f, 1.0f, 1.0f} };
		renderTargetDesc.mDepth = 1;
		renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
		renderTargetDesc.mFormat = TinyImageFormat_R16G16_SFLOAT;
		renderTargetDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		renderTargetDesc.mWidth = shadowMapResolution;
		renderTargetDesc.mHeight = shadowMapResolution;
		renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
		renderTargetDesc.mSampleQuality = 0;
		renderTargetDesc.pName = "Shadow variance RT";
		for (uint32_t i = 0; i < 2; ++i)
		{
			addRenderTarget(pRenderer, &renderTargetDesc, &pRenderTargetShadowVariance[i]);
		}

		RenderTargetDesc shadowRT = {};
		shadowRT.mArraySize = 1;
		shadowRT.mClearValue = { {1.0f, 0.0f} };
		shadowRT.mDepth = 1;
		shadowRT.mFormat = TinyImageFormat_D16_UNORM;
		shadowRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
		shadowRT.mWidth = shadowMapResolution;
		shadowRT.mHeight = shadowMapResolution;
		shadowRT.mSampleCount = SAMPLE_COUNT_1;
		shadowRT.mSampleQuality = 0;
		shadowRT.pName = "Shadow depth RT";
		addRenderTarget(pRenderer, &shadowRT, &pRenderTargetShadowDepth);

#if PT_USE_CAUSTICS != 0
		const uint ptShadowMapResolution = 4096;
		renderTargetDesc = {};
		renderTargetDesc.mArraySize = 1;
		renderTargetDesc.mClearValue = { { 1.0f, 1.0f, 1.0f, 1.0f } };
		renderTargetDesc.mDepth = 1;
		renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
		renderTargetDesc.mFormat = TinyImageFormat_R16G16_UNORM;
		renderTargetDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		renderTargetDesc.mWidth = ptShadowMapResolution;
		renderTargetDesc.mHeight = ptShadowMapResolution;
		renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
		renderTargetDesc.mSampleQuality = 0;
		renderTargetDesc.pName = "PT shadow variance RT";
		for (uint32_t w = 0; w < 3; ++w)
		{
			addRenderTarget(pRenderer, &renderTargetDesc, &pRenderTargetPTShadowVariance[w]);
		}

		renderTargetDesc = {};
		renderTargetDesc.mArraySize = 1;
		renderTargetDesc.mClearValue = { { 1.0f, 1.0f, 1.0f, 1.0f } };
		renderTargetDesc.mDepth = 1;
		renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
		renderTargetDesc.mFormat = TinyImageFormat_R16G16_UNORM;
		renderTargetDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		renderTargetDesc.mWidth = ptShadowMapResolution / 4;
		renderTargetDesc.mHeight = ptShadowMapResolution / 4;
		renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
		renderTargetDesc.mSampleQuality = 0;
		renderTargetDesc.pName = "PT shadow final RT";
		for (uint32_t w = 0; w < 3; ++w)
		{
			for (uint32_t i = 0; i < 2; ++i)
			{
				addRenderTarget(pRenderer, &renderTargetDesc, &pRenderTargetPTShadowFinal[i][w]);
			}
		}
#endif
#endif
	}

	void DestroyResources()
	{
		removeResource(pBufferSkyboxVertex);
#if USE_SHADOWS != 0
		for (uint32_t i = 0; i < 2; ++i)
		{
			removeRenderTarget(pRenderer, pRenderTargetShadowVariance[i]);
		}
		removeRenderTarget(pRenderer, pRenderTargetShadowDepth);
#if PT_USE_CAUSTICS != 0
		for (uint32_t w = 0; w < 3; ++w)
		{
			removeRenderTarget(pRenderer, pRenderTargetPTShadowVariance[w]);
			for (uint32_t i = 0; i < 2; ++i)
			{
				removeRenderTarget(pRenderer, pRenderTargetPTShadowFinal[i][w]);
			}
		}
#endif
#endif

		DestroyTextures();
		DestroyModels();
	}

	void LoadModel(size_t m)
	{
		static const char* modelNames[MESH_COUNT] = { "cube.gltf", "sphere.gltf", "plane.gltf", "lion.gltf" };

		GeometryLoadDesc loadDesc = {};
		loadDesc.pFileName = modelNames[m];
		loadDesc.ppGeometry = &pMeshes[m];
		loadDesc.pVertexLayout = &vertexLayoutDefault;
		addResource(&loadDesc, NULL);
	}

	void LoadModels()
	{
		vertexLayoutDefault.mAttribCount = 3;
		vertexLayoutDefault.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		vertexLayoutDefault.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
		vertexLayoutDefault.mAttribs[0].mBinding = 0;
		vertexLayoutDefault.mAttribs[0].mLocation = 0;
		vertexLayoutDefault.mAttribs[0].mOffset = 0;
		vertexLayoutDefault.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
		vertexLayoutDefault.mAttribs[1].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
		vertexLayoutDefault.mAttribs[1].mBinding = 0;
		vertexLayoutDefault.mAttribs[1].mLocation = 1;
		vertexLayoutDefault.mAttribs[1].mOffset = 3 * sizeof(float);
		vertexLayoutDefault.mAttribs[2].mSemantic = SEMANTIC_TEXCOORD0;
		vertexLayoutDefault.mAttribs[2].mFormat = TinyImageFormat_R32G32_SFLOAT;
		vertexLayoutDefault.mAttribs[2].mBinding = 0;
		vertexLayoutDefault.mAttribs[2].mLocation = 2;
		vertexLayoutDefault.mAttribs[2].mOffset = 6 * sizeof(float);

		for (size_t i = 0; i < MESH_COUNT; i += 1)
		{
			LoadModel(i);
		}
	}

	void DestroyModels()
	{
		for (int i = 0; i < MESH_COUNT; ++i)
			removeResource(pMeshes[i]);
	}

	void LoadTextures()
	{
		const char* textureNames[TEXTURE_COUNT] = {
			"skybox/hw_sahara/sahara_rt",
			"skybox/hw_sahara/sahara_lf",
			"skybox/hw_sahara/sahara_up",
			"skybox/hw_sahara/sahara_dn",
			"skybox/hw_sahara/sahara_ft",
			"skybox/hw_sahara/sahara_bk",
			"grid",
		};

		for (int i = 0; i < TEXTURE_COUNT; ++i)
		{
			TextureLoadDesc textureDesc = {};
			textureDesc.pFileName = textureNames[i];
			textureDesc.ppTexture = &pTextures[i];
			addResource(&textureDesc, NULL);
		}
	}

	void DestroyTextures()
	{
		for (uint i = 0; i < TEXTURE_COUNT; ++i)
			removeResource(pTextures[i]);
	}

	void CreateUniformBuffers()
	{
		BufferLoadDesc materialUBDesc = {};
		materialUBDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		materialUBDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		materialUBDesc.mDesc.mSize = sizeof(MaterialUniformBlock);
		materialUBDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		materialUBDesc.pData = NULL;
		for (int i = 0; i < gImageCount; ++i)
		{
			materialUBDesc.ppBuffer = &pBufferMaterials[i];
			addResource(&materialUBDesc, NULL);
		}

		BufferLoadDesc ubDesc = {};
		ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		ubDesc.mDesc.mSize = sizeof(ObjectInfoUniformBlock);
		ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		ubDesc.pData = NULL;
		for (int i = 0; i < gImageCount; ++i)
		{
			ubDesc.ppBuffer = &pBufferOpaqueObjectTransforms[i];
			addResource(&ubDesc, NULL);
		}
		for (int i = 0; i < gImageCount; ++i)
		{
			ubDesc.ppBuffer = &pBufferTransparentObjectTransforms[i];
			addResource(&ubDesc, NULL);
		}

		BufferLoadDesc skyboxDesc = {};
		skyboxDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		skyboxDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		skyboxDesc.mDesc.mSize = sizeof(SkyboxUniformBlock);
		skyboxDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		skyboxDesc.pData = NULL;
		for (int i = 0; i < gImageCount; ++i)
		{
			skyboxDesc.ppBuffer = &pBufferSkyboxUniform[i];
			addResource(&skyboxDesc, NULL);
		}

		BufferLoadDesc camUniDesc = {};
		camUniDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		camUniDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		camUniDesc.mDesc.mSize = sizeof(CameraUniform);
		camUniDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		camUniDesc.pData = &gCameraUniformData;
		for (int i = 0; i < gImageCount; ++i)
		{
			camUniDesc.ppBuffer = &pBufferCameraUniform[i];
			addResource(&camUniDesc, NULL);
		}

		BufferLoadDesc camLightUniDesc = {};
		camLightUniDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		camLightUniDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		camLightUniDesc.mDesc.mSize = sizeof(CameraUniform);
		camLightUniDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		camLightUniDesc.pData = &gCameraLightUniformData;
		for (int i = 0; i < gImageCount; ++i)
		{
			camLightUniDesc.ppBuffer = &pBufferCameraLightUniform[i];
			addResource(&camLightUniDesc, NULL);
		}

		BufferLoadDesc lightUniformDesc = {};
		lightUniformDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		lightUniformDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		lightUniformDesc.mDesc.mSize = sizeof(LightUniformBlock);
		lightUniformDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		lightUniformDesc.pData = NULL;
		for (int i = 0; i < gImageCount; ++i)
		{
			lightUniformDesc.ppBuffer = &pBufferLightUniform[i];
			addResource(&lightUniformDesc, NULL);
		}
		BufferLoadDesc wboitSettingsDesc = {};
		wboitSettingsDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		wboitSettingsDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		wboitSettingsDesc.mDesc.mSize = max(sizeof(WBOITSettings), sizeof(WBOITVolitionSettings));
		wboitSettingsDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		wboitSettingsDesc.pData = NULL;
		for (int i = 0; i < gImageCount; ++i)
		{
			wboitSettingsDesc.ppBuffer = &pBufferWBOITSettings[i];
			addResource(&wboitSettingsDesc, NULL);
		}
	}

	void DestroyUniformBuffers()
	{
		for (int i = 0; i < gImageCount; ++i)
		{
			removeResource(pBufferMaterials[i]);
			removeResource(pBufferOpaqueObjectTransforms[i]);
			removeResource(pBufferTransparentObjectTransforms[i]);
			removeResource(pBufferLightUniform[i]);
			removeResource(pBufferSkyboxUniform[i]);
			removeResource(pBufferCameraUniform[i]);
			removeResource(pBufferCameraLightUniform[i]);
			removeResource(pBufferWBOITSettings[i]);
		}
	}

	/************************************************************************/
	// Load and Unload functions
	/************************************************************************/
	bool CreateRenderTargetsAndSwapChain() const
	{
		const uint32_t width = mSettings.mWidth;
		const uint32_t height = mSettings.mHeight;

		const ClearValue depthClear = {{1.0f, 0}};
		const ClearValue colorClearBlack = { {0.0f, 0.0f, 0.0f, 0.0f} };
		const ClearValue colorClearWhite = { {1.0f, 1.0f, 1.0f, 1.0f} };
		const ClearValue colorClearTransparentWhite = {{1.0f, 1.0f, 1.0f, 0.0f}};
		/************************************************************************/
		// Main depth buffer
		/************************************************************************/
		RenderTargetDesc depthRT = {};
		depthRT.mArraySize = 1;
		depthRT.mClearValue = depthClear;
		depthRT.mDepth = 1;
		depthRT.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
		depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
		depthRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
		depthRT.mWidth = width;
		depthRT.mHeight = height;
		depthRT.mSampleCount = SAMPLE_COUNT_1;
		depthRT.mSampleQuality = 0;
		depthRT.pName = "Depth RT";
		addRenderTarget(pRenderer, &depthRT, &pRenderTargetDepth);
#if PT_USE_DIFFUSION != 0
		depthRT.mFormat = TinyImageFormat_R32_SFLOAT;
		depthRT.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		depthRT.pName = "Depth RT PT";
		addRenderTarget(pRenderer, &depthRT, &pRenderTargetPTDepthCopy);
#endif
		/************************************************************************/
		// Swapchain
		/************************************************************************/
		{
			const uint32_t width = mSettings.mWidth;
			const uint32_t height = mSettings.mHeight;
			SwapChainDesc  swapChainDesc = {};
			swapChainDesc.mWindowHandle = pWindow->handle;
			swapChainDesc.mPresentQueueCount = 1;
			swapChainDesc.ppPresentQueues = &pGraphicsQueue;
			swapChainDesc.mWidth = width;
			swapChainDesc.mHeight = height;
			swapChainDesc.mImageCount = gImageCount;
			swapChainDesc.mColorFormat = getRecommendedSwapchainFormat(true);
			swapChainDesc.mColorClearValue = {{1, 0, 1, 1}};

			swapChainDesc.mEnableVsync = false;
			::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

			if (pSwapChain == NULL)
				return false;
		}
		/************************************************************************/
		// WBOIT render targets
		/************************************************************************/
		ClearValue     wboitClearValues[] = { colorClearBlack, colorClearWhite };
		const char* wboitNames[] = { "Accumulation RT", "Revealage RT" };
		for (int i = 0; i < WBOIT_RT_COUNT; ++i)
		{
			RenderTargetDesc renderTargetDesc = {};
			renderTargetDesc.mArraySize = 1;
			renderTargetDesc.mClearValue = wboitClearValues[i];
			renderTargetDesc.mDepth = 1;
			renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
			renderTargetDesc.mFormat = gWBOITRenderTargetFormats[i];
			renderTargetDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			renderTargetDesc.mWidth = width;
			renderTargetDesc.mHeight = height;
			renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
			renderTargetDesc.mSampleQuality = 0;
			renderTargetDesc.pName = wboitNames[i];
			addRenderTarget(pRenderer, &renderTargetDesc, &pRenderTargetWBOIT[i]);
		}

		/************************************************************************/
		// PT render targets
		/************************************************************************/
		ClearValue     ptClearValues[] = { colorClearBlack, colorClearTransparentWhite, colorClearBlack };
		const char* ptNames[] = { "Accumulation RT", "Modulation RT", "Refraction RT" };
		for (int i = 0; i < PT_RT_COUNT; ++i)
		{
			if (i == PT_RT_ACCUMULATION)
			{
				// PT shares the accumulation buffer with WBOIT
				pRenderTargetPT[PT_RT_ACCUMULATION] = pRenderTargetWBOIT[WBOIT_RT_ACCUMULATION];
				continue;
			}
			RenderTargetDesc renderTargetDesc = {};
			renderTargetDesc.mArraySize = 1;
			renderTargetDesc.mClearValue = ptClearValues[i];
			renderTargetDesc.mDepth = 1;
			renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_TEXTURE;
			renderTargetDesc.mFormat = gPTRenderTargetFormats[i];
			renderTargetDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			renderTargetDesc.mWidth = width;
			renderTargetDesc.mHeight = height;
			renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
			renderTargetDesc.mSampleQuality = 0;
			renderTargetDesc.pName = ptNames[i];
			addRenderTarget(pRenderer, &renderTargetDesc, &pRenderTargetPT[i]);
		}

		{
			RenderTargetDesc renderTargetDesc = {};
			renderTargetDesc.mArraySize = 1;
			renderTargetDesc.mClearValue = pSwapChain->ppRenderTargets[0]->mClearValue;
			renderTargetDesc.mDepth = 1;
			renderTargetDesc.mFormat = pSwapChain->ppRenderTargets[0]->mFormat;
			renderTargetDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			renderTargetDesc.mWidth = width;
			renderTargetDesc.mHeight = height;
			renderTargetDesc.mSampleCount = SAMPLE_COUNT_1;
			renderTargetDesc.mSampleQuality = 0;
			renderTargetDesc.mDescriptors = DESCRIPTOR_TYPE_RW_TEXTURE | DESCRIPTOR_TYPE_TEXTURE;
			renderTargetDesc.mMipLevels = (uint)log2(width);
			renderTargetDesc.pName = "PT Background RT";
			renderTargetDesc.mFlags = TEXTURE_CREATION_FLAG_OWN_MEMORY_BIT;
			addRenderTarget(pRenderer, &renderTargetDesc, &pRenderTargetPTBackground);
		}

#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			// Create AOIT resources
			TextureDesc aoitClearMaskTextureDesc = {};
			aoitClearMaskTextureDesc.mFormat = TinyImageFormat_R32_UINT;
			aoitClearMaskTextureDesc.mWidth = mSettings.mWidth;
			aoitClearMaskTextureDesc.mHeight = mSettings.mHeight;
			aoitClearMaskTextureDesc.mDepth = 1;
			aoitClearMaskTextureDesc.mArraySize = 1;
			aoitClearMaskTextureDesc.mSampleCount = SAMPLE_COUNT_1;
			aoitClearMaskTextureDesc.mSampleQuality = 0;
			aoitClearMaskTextureDesc.mMipLevels = 1;
			aoitClearMaskTextureDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			aoitClearMaskTextureDesc.mDescriptors = DESCRIPTOR_TYPE_RW_TEXTURE | DESCRIPTOR_TYPE_TEXTURE;
			aoitClearMaskTextureDesc.pName = "AOIT Clear Mask";

			TextureLoadDesc aoitClearMaskTextureLoadDesc = {};
			aoitClearMaskTextureLoadDesc.pDesc = &aoitClearMaskTextureDesc;
			aoitClearMaskTextureLoadDesc.ppTexture = &pTextureAOITClearMask;
			addResource(&aoitClearMaskTextureLoadDesc, NULL);

#if AOIT_NODE_COUNT != 2
			BufferLoadDesc aoitDepthDataLoadDesc = {};
			aoitDepthDataLoadDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
			aoitDepthDataLoadDesc.mDesc.mFormat = TinyImageFormat_UNDEFINED;
			aoitDepthDataLoadDesc.mDesc.mElementCount = mSettings.mWidth * mSettings.mHeight;
			aoitDepthDataLoadDesc.mDesc.mStructStride = sizeof(uint32_t) * 4 * AOIT_RT_COUNT;
			aoitDepthDataLoadDesc.mDesc.mSize = aoitDepthDataLoadDesc.mDesc.mElementCount * aoitDepthDataLoadDesc.mDesc.mStructStride;
			aoitDepthDataLoadDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER | DESCRIPTOR_TYPE_BUFFER;
			aoitDepthDataLoadDesc.mDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			aoitDepthDataLoadDesc.mDesc.pName = "AOIT Depth Data";
			aoitDepthDataLoadDesc.ppBuffer = &pBufferAOITDepthData;
			addResource(&aoitDepthDataLoadDesc, NULL);
#endif

			BufferLoadDesc aoitColorDataLoadDesc = {};
			aoitColorDataLoadDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
			aoitColorDataLoadDesc.mDesc.mFormat = TinyImageFormat_UNDEFINED;
			aoitColorDataLoadDesc.mDesc.mElementCount = mSettings.mWidth * mSettings.mHeight;
			aoitColorDataLoadDesc.mDesc.mStructStride = sizeof(uint32_t) * 4 * AOIT_RT_COUNT;
			aoitColorDataLoadDesc.mDesc.mSize = aoitColorDataLoadDesc.mDesc.mElementCount * aoitColorDataLoadDesc.mDesc.mStructStride;
			aoitColorDataLoadDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_RW_BUFFER | DESCRIPTOR_TYPE_BUFFER;
			aoitColorDataLoadDesc.mDesc.mStartState = RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			aoitColorDataLoadDesc.mDesc.pName = "AOIT Color Data";
			aoitColorDataLoadDesc.ppBuffer = &pBufferAOITColorData;
			addResource(&aoitColorDataLoadDesc, NULL);
		}
#endif

		return true;
	}

	void DestroyRenderTargetsAndSwapChian()
	{
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			removeResource(pTextureAOITClearMask);
#if AOIT_NODE_COUNT != 2
			removeResource(pBufferAOITDepthData);
#endif
			removeResource(pBufferAOITColorData);
		}
#endif

		removeRenderTarget(pRenderer, pRenderTargetDepth);
#if PT_USE_DIFFUSION != 0
		removeRenderTarget(pRenderer, pRenderTargetPTDepthCopy);
#endif
		for (uint32_t i = 0; i < WBOIT_RT_COUNT; ++i)
		{
			removeRenderTarget(pRenderer, pRenderTargetWBOIT[i]);
		}
		for (uint32_t i = 0; i < PT_RT_COUNT; ++i)
		{
			if (i == PT_RT_ACCUMULATION)
			{
				continue;    // Acculuation RT is shared with WBOIT and has already been removed
			}
			removeRenderTarget(pRenderer, pRenderTargetPT[i]);
		}
		removeRenderTarget(pRenderer, pRenderTargetPTBackground);
		removeSwapChain(pRenderer, pSwapChain);
	}

	void CreatePipelines()
	{
		// Define vertex layouts
		VertexLayout vertexLayoutSkybox = {};
		vertexLayoutSkybox.mAttribCount = 1;
		vertexLayoutSkybox.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		vertexLayoutSkybox.mAttribs[0].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		vertexLayoutSkybox.mAttribs[0].mBinding = 0;
		vertexLayoutSkybox.mAttribs[0].mLocation = 0;
		vertexLayoutSkybox.mAttribs[0].mOffset = 0;

		RasterizerStateDesc rasterStateBackDesc = {};
		rasterStateBackDesc.mCullMode = CULL_MODE_BACK;

		RasterizerStateDesc rasterStateFrontDesc = {};
		rasterStateFrontDesc.mCullMode = CULL_MODE_FRONT;

		RasterizerStateDesc rasterStateNoneDesc = {};
		rasterStateNoneDesc.mCullMode = CULL_MODE_NONE;

		DepthStateDesc depthStateEnabledDesc = {};
		depthStateEnabledDesc.mDepthFunc = CMP_LEQUAL;
		depthStateEnabledDesc.mDepthWrite = true;
		depthStateEnabledDesc.mDepthTest = true;

		DepthStateDesc depthStateDisabledDesc = {};
		depthStateDisabledDesc.mDepthWrite = false;
		depthStateDisabledDesc.mDepthTest = false;

		DepthStateDesc depthStateNoWriteDesc = {};
		depthStateNoWriteDesc.mDepthFunc = CMP_LEQUAL;
		depthStateNoWriteDesc.mDepthWrite = false;
		depthStateNoWriteDesc.mDepthTest = true;

		BlendStateDesc blendStateAlphaDesc = {};
		blendStateAlphaDesc.mSrcFactors[0] = BC_SRC_ALPHA;
		blendStateAlphaDesc.mDstFactors[0] = BC_ONE_MINUS_SRC_ALPHA;
		blendStateAlphaDesc.mBlendModes[0] = BM_ADD;
		blendStateAlphaDesc.mSrcAlphaFactors[0] = BC_ONE;
		blendStateAlphaDesc.mDstAlphaFactors[0] = BC_ZERO;
		blendStateAlphaDesc.mBlendAlphaModes[0] = BM_ADD;
		blendStateAlphaDesc.mMasks[0] = ALL;
		blendStateAlphaDesc.mRenderTargetMask = BLEND_STATE_TARGET_0;
		blendStateAlphaDesc.mIndependentBlend = false;

		BlendStateDesc blendStateWBOITShadeDesc = {};
		blendStateWBOITShadeDesc.mSrcFactors[0] = BC_ONE;
		blendStateWBOITShadeDesc.mDstFactors[0] = BC_ONE;
		blendStateWBOITShadeDesc.mBlendModes[0] = BM_ADD;
		blendStateWBOITShadeDesc.mSrcAlphaFactors[0] = BC_ONE;
		blendStateWBOITShadeDesc.mDstAlphaFactors[0] = BC_ONE;
		blendStateWBOITShadeDesc.mBlendAlphaModes[0] = BM_ADD;
		blendStateWBOITShadeDesc.mMasks[0] = ALL;
		blendStateWBOITShadeDesc.mSrcFactors[1] = BC_ZERO;
		blendStateWBOITShadeDesc.mDstFactors[1] = BC_ONE_MINUS_SRC_COLOR;
		blendStateWBOITShadeDesc.mBlendModes[1] = BM_ADD;
		blendStateWBOITShadeDesc.mSrcAlphaFactors[1] = BC_ZERO;
		blendStateWBOITShadeDesc.mDstAlphaFactors[1] = BC_ONE_MINUS_SRC_ALPHA;
		blendStateWBOITShadeDesc.mBlendAlphaModes[1] = BM_ADD;
		blendStateWBOITShadeDesc.mMasks[1] = RED;
		blendStateWBOITShadeDesc.mRenderTargetMask = BLEND_STATE_TARGET_0 | BLEND_STATE_TARGET_1;
		blendStateWBOITShadeDesc.mIndependentBlend = true;

		BlendStateDesc blendStateWBOITVolitionShadeDesc = {};
		blendStateWBOITVolitionShadeDesc.mSrcFactors[0] = BC_ONE;
		blendStateWBOITVolitionShadeDesc.mDstFactors[0] = BC_ONE;
		blendStateWBOITVolitionShadeDesc.mBlendModes[0] = BM_ADD;
		blendStateWBOITVolitionShadeDesc.mSrcAlphaFactors[0] = BC_ONE;
		blendStateWBOITVolitionShadeDesc.mDstAlphaFactors[0] = BC_ONE;
		blendStateWBOITVolitionShadeDesc.mBlendAlphaModes[0] = BM_ADD;
		blendStateWBOITVolitionShadeDesc.mMasks[0] = ALL;
		blendStateWBOITVolitionShadeDesc.mSrcFactors[1] = BC_ZERO;
		blendStateWBOITVolitionShadeDesc.mDstFactors[1] = BC_ONE_MINUS_SRC_COLOR;
		blendStateWBOITVolitionShadeDesc.mBlendModes[1] = BM_ADD;
		blendStateWBOITVolitionShadeDesc.mSrcAlphaFactors[1] = BC_ONE;
		blendStateWBOITVolitionShadeDesc.mDstAlphaFactors[1] = BC_ONE;
		blendStateWBOITVolitionShadeDesc.mBlendAlphaModes[1] = BM_ADD;
		blendStateWBOITVolitionShadeDesc.mMasks[1] = RED | ALPHA;
		blendStateWBOITVolitionShadeDesc.mRenderTargetMask = BLEND_STATE_TARGET_0 | BLEND_STATE_TARGET_1;
		blendStateWBOITVolitionShadeDesc.mIndependentBlend = true;

		BlendStateDesc blendStatePTShadeDesc = {};
		blendStatePTShadeDesc.mSrcFactors[0] = BC_ONE;
		blendStatePTShadeDesc.mDstFactors[0] = BC_ONE;
		blendStatePTShadeDesc.mBlendModes[0] = BM_ADD;
		blendStatePTShadeDesc.mSrcAlphaFactors[0] = BC_ONE;
		blendStatePTShadeDesc.mDstAlphaFactors[0] = BC_ONE;
		blendStatePTShadeDesc.mBlendAlphaModes[0] = BM_ADD;
		blendStatePTShadeDesc.mMasks[0] = ALL;
		blendStatePTShadeDesc.mSrcFactors[1] = BC_ZERO;
		blendStatePTShadeDesc.mDstFactors[1] = BC_ONE_MINUS_SRC_COLOR;
		blendStatePTShadeDesc.mBlendModes[1] = BM_ADD;
		blendStatePTShadeDesc.mSrcAlphaFactors[1] = BC_ONE;
		blendStatePTShadeDesc.mDstAlphaFactors[1] = BC_ONE;
		blendStatePTShadeDesc.mBlendAlphaModes[1] = BM_ADD;
		blendStatePTShadeDesc.mMasks[1] = ALL;
#if PT_USE_REFRACTION != 0
		blendStatePTShadeDesc.mSrcFactors[2] = BC_ONE;
		blendStatePTShadeDesc.mDstFactors[2] = BC_ONE;
		blendStatePTShadeDesc.mBlendModes[2] = BM_ADD;
		blendStatePTShadeDesc.mSrcAlphaFactors[2] = BC_ONE;
		blendStatePTShadeDesc.mDstAlphaFactors[2] = BC_ONE;
		blendStatePTShadeDesc.mBlendAlphaModes[2] = BM_ADD;
		blendStatePTShadeDesc.mMasks[2] = RED | GREEN;
		blendStatePTShadeDesc.mRenderTargetMask = BLEND_STATE_TARGET_2;
#endif
		blendStatePTShadeDesc.mRenderTargetMask |= BLEND_STATE_TARGET_0 | BLEND_STATE_TARGET_1;
		blendStatePTShadeDesc.mIndependentBlend = true;

		BlendStateDesc blendStatePTMinDesc = {};
		blendStatePTMinDesc.mSrcFactors[0] = BC_ONE;
		blendStatePTMinDesc.mDstFactors[0] = BC_ONE;
		blendStatePTMinDesc.mBlendModes[0] = BM_MIN;
		blendStatePTMinDesc.mSrcAlphaFactors[0] = BC_ONE;
		blendStatePTMinDesc.mDstAlphaFactors[0] = BC_ONE;
		blendStatePTMinDesc.mBlendAlphaModes[0] = BM_MIN;
		blendStatePTMinDesc.mMasks[0] = RED | GREEN;
		blendStatePTMinDesc.mRenderTargetMask = BLEND_STATE_TARGET_0 | BLEND_STATE_TARGET_1 | BLEND_STATE_TARGET_2;
		blendStatePTMinDesc.mIndependentBlend = false;

#if AOIT_ENABLE
		BlendStateDesc blendStateAOITShadeaDesc = {};
		blendStateAOITShadeaDesc.mSrcFactors[0] = BC_ONE;
		blendStateAOITShadeaDesc.mDstFactors[0] = BC_SRC_ALPHA;
		blendStateAOITShadeaDesc.mBlendModes[0] = BM_ADD;
		blendStateAOITShadeaDesc.mSrcAlphaFactors[0] = BC_ONE;
		blendStateAOITShadeaDesc.mDstAlphaFactors[0] = BC_SRC_ALPHA;
		blendStateAOITShadeaDesc.mBlendAlphaModes[0] = BM_ADD;
		blendStateAOITShadeaDesc.mMasks[0] = ALL;
		blendStateAOITShadeaDesc.mRenderTargetMask = BLEND_STATE_TARGET_0;
		blendStateAOITShadeaDesc.mIndependentBlend = false;
#endif

		// Skybox pipeline
		PipelineDesc desc = {};
		desc.mType = PIPELINE_TYPE_GRAPHICS;
		GraphicsPipelineDesc& skyboxPipelineDesc = desc.mGraphicsDesc;
		skyboxPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		skyboxPipelineDesc.pShaderProgram = pShaderSkybox;
		skyboxPipelineDesc.pRootSignature = pRootSignatureSkybox;
		skyboxPipelineDesc.mRenderTargetCount = 1;
		skyboxPipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		skyboxPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		skyboxPipelineDesc.mSampleQuality = 0;
		skyboxPipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		skyboxPipelineDesc.pVertexLayout = &vertexLayoutSkybox;
		skyboxPipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		skyboxPipelineDesc.pDepthState = &depthStateDisabledDesc;
		skyboxPipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelineSkybox);

#if USE_SHADOWS != 0
		// Shadow pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& shadowPipelineDesc = desc.mGraphicsDesc;
		shadowPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		shadowPipelineDesc.pShaderProgram = pShaderShadow;
		shadowPipelineDesc.pRootSignature = pRootSignature;
		shadowPipelineDesc.mRenderTargetCount = 1;
		shadowPipelineDesc.pColorFormats = &pRenderTargetShadowVariance[0]->mFormat;
		shadowPipelineDesc.mSampleCount = pRenderTargetShadowVariance[0]->mSampleCount;
		shadowPipelineDesc.mSampleQuality = pRenderTargetShadowVariance[0]->mSampleQuality;
		shadowPipelineDesc.mDepthStencilFormat = TinyImageFormat_D16_UNORM;
		shadowPipelineDesc.pVertexLayout = &vertexLayoutDefault;
		shadowPipelineDesc.pRasterizerState = &rasterStateFrontDesc;
		shadowPipelineDesc.pDepthState = &depthStateEnabledDesc;
		shadowPipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelineShadow);

		// Gaussian blur pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& blurPipelineDesc = desc.mGraphicsDesc;
		blurPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		blurPipelineDesc.pShaderProgram = pShaderGaussianBlur;
		blurPipelineDesc.pRootSignature = pRootSignatureGaussianBlur;
		blurPipelineDesc.mRenderTargetCount = 1;
		blurPipelineDesc.pColorFormats = &pRenderTargetShadowVariance[0]->mFormat;
		blurPipelineDesc.mSampleCount = pRenderTargetShadowVariance[0]->mSampleCount;
		blurPipelineDesc.mSampleQuality = pRenderTargetShadowVariance[0]->mSampleQuality;
		blurPipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		blurPipelineDesc.pVertexLayout = NULL;
		blurPipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		blurPipelineDesc.pDepthState = &depthStateDisabledDesc;
		blurPipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelineGaussianBlur);

#if PT_USE_CAUSTICS != 0
		blurPipelineDesc.pColorFormats = &pRenderTargetPTShadowFinal[0][0]->mFormat;
		addPipeline(pRenderer, &desc, &pPipelinePTGaussianBlur);
		TinyImageFormat stochasticShadowColorFormats[] = { pRenderTargetPTShadowVariance[0]->mFormat, pRenderTargetPTShadowVariance[1]->mFormat,
            pRenderTargetPTShadowVariance[2]->mFormat };

		// Stochastic shadow pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& stochasticShadowPipelineDesc = desc.mGraphicsDesc;
		stochasticShadowPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		stochasticShadowPipelineDesc.pShaderProgram = pShaderPTShadow;
		stochasticShadowPipelineDesc.pRootSignature = pRootSignature;
		stochasticShadowPipelineDesc.mRenderTargetCount = 3;
		stochasticShadowPipelineDesc.pColorFormats = stochasticShadowColorFormats;
		stochasticShadowPipelineDesc.mSampleCount = pRenderTargetPTShadowVariance[0]->mSampleCount;
		stochasticShadowPipelineDesc.mSampleQuality = pRenderTargetPTShadowVariance[0]->mSampleQuality;
		stochasticShadowPipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		stochasticShadowPipelineDesc.pVertexLayout = &vertexLayoutDefault;
		stochasticShadowPipelineDesc.pRasterizerState = &rasterStateFrontDesc;
		stochasticShadowPipelineDesc.pDepthState = &depthStateDisabledDesc;
		stochasticShadowPipelineDesc.pBlendState = &blendStatePTMinDesc;
		addPipeline(pRenderer, &desc, &pPipelinePTShadow);

		// Downsample shadow pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& downsampleShadowPipelineDesc = desc.mGraphicsDesc;
		downsampleShadowPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		downsampleShadowPipelineDesc.pShaderProgram = pShaderPTDownsample;
		downsampleShadowPipelineDesc.pRootSignature = pRootSignaturePTDownsample;
		downsampleShadowPipelineDesc.mRenderTargetCount = 1;
		downsampleShadowPipelineDesc.pColorFormats = &pRenderTargetPTShadowFinal[0][0]->mFormat;
		downsampleShadowPipelineDesc.mSampleCount = pRenderTargetPTShadowFinal[0][0]->mSampleCount;
		downsampleShadowPipelineDesc.mSampleQuality = pRenderTargetPTShadowFinal[0][0]->mSampleQuality;
		downsampleShadowPipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		downsampleShadowPipelineDesc.pVertexLayout = NULL;
		downsampleShadowPipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		downsampleShadowPipelineDesc.pDepthState = &depthStateDisabledDesc;
		downsampleShadowPipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelinePTDownsample);

		// Copy shadow map pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& copyShadowDepthPipelineDesc = desc.mGraphicsDesc;
		copyShadowDepthPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		copyShadowDepthPipelineDesc.pShaderProgram = pShaderPTCopyShadowDepth;
		copyShadowDepthPipelineDesc.pRootSignature = pRootSignaturePTCopyShadowDepth;
		copyShadowDepthPipelineDesc.mRenderTargetCount = 1;
		copyShadowDepthPipelineDesc.pColorFormats = &pRenderTargetPTShadowVariance[0]->mFormat;
		copyShadowDepthPipelineDesc.mSampleCount = pRenderTargetPTShadowVariance[0]->mSampleCount;
		copyShadowDepthPipelineDesc.mSampleQuality = pRenderTargetPTShadowVariance[0]->mSampleQuality;
		copyShadowDepthPipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		copyShadowDepthPipelineDesc.pVertexLayout = NULL;
		copyShadowDepthPipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		copyShadowDepthPipelineDesc.pDepthState = &depthStateDisabledDesc;
		copyShadowDepthPipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelinePTCopyShadowDepth);

#endif
#endif

		// Forward shading pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& forwardPipelineDesc = desc.mGraphicsDesc;
		forwardPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		forwardPipelineDesc.pShaderProgram = pShaderForward;
		forwardPipelineDesc.pRootSignature = pRootSignature;
		forwardPipelineDesc.mRenderTargetCount = 1;
		forwardPipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		forwardPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		forwardPipelineDesc.mSampleQuality = 0;
		forwardPipelineDesc.mDepthStencilFormat = TinyImageFormat_D32_SFLOAT;
		forwardPipelineDesc.pVertexLayout = &vertexLayoutDefault;
		forwardPipelineDesc.pRasterizerState = &rasterStateFrontDesc;
		forwardPipelineDesc.pDepthState = &depthStateEnabledDesc;
		forwardPipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelineForward);

		// Transparent forward shading pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& transparentForwardPipelineDesc = desc.mGraphicsDesc;
		transparentForwardPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		transparentForwardPipelineDesc.pShaderProgram = pShaderForward;
		transparentForwardPipelineDesc.pRootSignature = pRootSignature;
		transparentForwardPipelineDesc.mRenderTargetCount = 1;
		transparentForwardPipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		transparentForwardPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		transparentForwardPipelineDesc.mSampleQuality = 0;
		transparentForwardPipelineDesc.mDepthStencilFormat = TinyImageFormat_D32_SFLOAT;
		transparentForwardPipelineDesc.pVertexLayout = &vertexLayoutDefault;
		transparentForwardPipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		transparentForwardPipelineDesc.pDepthState = &depthStateNoWriteDesc;
		transparentForwardPipelineDesc.pBlendState = &blendStateAlphaDesc;
		addPipeline(pRenderer, &desc, &pPipelineTransparentForward);

		// WBOIT shading pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& wboitShadePipelineDesc = desc.mGraphicsDesc;
		wboitShadePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		wboitShadePipelineDesc.pShaderProgram = pShaderWBOITShade;
		wboitShadePipelineDesc.pRootSignature = pRootSignature;
		wboitShadePipelineDesc.mRenderTargetCount = WBOIT_RT_COUNT;
		wboitShadePipelineDesc.pColorFormats = gWBOITRenderTargetFormats;
		wboitShadePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		wboitShadePipelineDesc.mSampleQuality = 0;
		wboitShadePipelineDesc.mDepthStencilFormat = TinyImageFormat_D32_SFLOAT;
		wboitShadePipelineDesc.pVertexLayout = &vertexLayoutDefault;
		wboitShadePipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		wboitShadePipelineDesc.pDepthState = &depthStateNoWriteDesc;
		wboitShadePipelineDesc.pBlendState = &blendStateWBOITShadeDesc;
		addPipeline(pRenderer, &desc, &pPipelineWBOITShade);

		// WBOIT composite pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& wboitCompositePipelineDesc = desc.mGraphicsDesc;
		wboitCompositePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		wboitCompositePipelineDesc.pShaderProgram = pShaderWBOITComposite;
		wboitCompositePipelineDesc.pRootSignature = pRootSignatureWBOITComposite;
		wboitCompositePipelineDesc.mRenderTargetCount = 1;
		wboitCompositePipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		wboitCompositePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		wboitCompositePipelineDesc.mSampleQuality = 0;
		wboitCompositePipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		wboitCompositePipelineDesc.pVertexLayout = NULL;
		wboitCompositePipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		wboitCompositePipelineDesc.pDepthState = &depthStateDisabledDesc;
		wboitCompositePipelineDesc.pBlendState = &blendStateAlphaDesc;
		addPipeline(pRenderer, &desc, &pPipelineWBOITComposite);

		// WBOIT Volition shading pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& wboitVolitionShadePipelineDesc = desc.mGraphicsDesc;
		wboitVolitionShadePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		wboitVolitionShadePipelineDesc.pShaderProgram = pShaderWBOITVShade;
		wboitVolitionShadePipelineDesc.pRootSignature = pRootSignature;
		wboitVolitionShadePipelineDesc.mRenderTargetCount = WBOIT_RT_COUNT;
		wboitVolitionShadePipelineDesc.pColorFormats = gWBOITRenderTargetFormats;
		wboitVolitionShadePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		wboitVolitionShadePipelineDesc.mSampleQuality = 0;
		wboitVolitionShadePipelineDesc.mDepthStencilFormat = TinyImageFormat_D32_SFLOAT;
		wboitVolitionShadePipelineDesc.pVertexLayout = &vertexLayoutDefault;
		wboitVolitionShadePipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		wboitVolitionShadePipelineDesc.pDepthState = &depthStateNoWriteDesc;
		wboitVolitionShadePipelineDesc.pBlendState = &blendStateWBOITVolitionShadeDesc;
		addPipeline(pRenderer, &desc, &pPipelineWBOITVShade);

		// WBOIT Volition composite pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& wboitVolitionCompositePipelineDesc = desc.mGraphicsDesc;
		wboitVolitionCompositePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		wboitVolitionCompositePipelineDesc.pShaderProgram = pShaderWBOITVComposite;
		wboitVolitionCompositePipelineDesc.pRootSignature = pRootSignatureWBOITComposite;
		wboitVolitionCompositePipelineDesc.mRenderTargetCount = 1;
		wboitVolitionCompositePipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		wboitVolitionCompositePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		wboitVolitionCompositePipelineDesc.mSampleQuality = 0;
		wboitVolitionCompositePipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		wboitVolitionCompositePipelineDesc.pVertexLayout = NULL;
		wboitVolitionCompositePipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		wboitVolitionCompositePipelineDesc.pDepthState = &depthStateDisabledDesc;
		wboitVolitionCompositePipelineDesc.pBlendState = &blendStateAlphaDesc;
		addPipeline(pRenderer, &desc, &pPipelineWBOITVComposite);

		// PT shading pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& ptShadePipelineDesc = desc.mGraphicsDesc;
		ptShadePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		ptShadePipelineDesc.pShaderProgram = pShaderPTShade;
		ptShadePipelineDesc.pRootSignature = pRootSignature;
		ptShadePipelineDesc.mRenderTargetCount = PT_RT_COUNT;
		ptShadePipelineDesc.pColorFormats = gPTRenderTargetFormats;
		ptShadePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		ptShadePipelineDesc.mSampleQuality = 0;
		ptShadePipelineDesc.mDepthStencilFormat = TinyImageFormat_D32_SFLOAT;
		ptShadePipelineDesc.pVertexLayout = &vertexLayoutDefault;
		ptShadePipelineDesc.pRasterizerState = &rasterStateFrontDesc;
		ptShadePipelineDesc.pDepthState = &depthStateNoWriteDesc;
		ptShadePipelineDesc.pBlendState = &blendStatePTShadeDesc;
		addPipeline(pRenderer, &desc, &pPipelinePTShade);

		// PT composite pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& ptCompositePipelineDesc = desc.mGraphicsDesc;
		ptCompositePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		ptCompositePipelineDesc.pShaderProgram = pShaderPTComposite;
		ptCompositePipelineDesc.pRootSignature = pRootSignaturePTComposite;
		ptCompositePipelineDesc.mRenderTargetCount = 1;
		ptCompositePipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		ptCompositePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		ptCompositePipelineDesc.mSampleQuality = 0;
		ptCompositePipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		ptCompositePipelineDesc.pVertexLayout = NULL;
		ptCompositePipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		ptCompositePipelineDesc.pDepthState = &depthStateDisabledDesc;
		ptCompositePipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelinePTComposite);

#if PT_USE_DIFFUSION != 0
		TinyImageFormat ptCopyDepthFormat = pRenderTargetPTDepthCopy->mFormat;

		// PT copy depth pipeline
		desc.mGraphicsDesc = {};
		GraphicsPipelineDesc& ptCopyDepthPipelineDesc = desc.mGraphicsDesc;
		ptCopyDepthPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		ptCopyDepthPipelineDesc.pShaderProgram = pShaderPTCopyDepth;
		ptCopyDepthPipelineDesc.pRootSignature = pRootSignaturePTCopyDepth;
		ptCopyDepthPipelineDesc.mRenderTargetCount = 1;
		ptCopyDepthPipelineDesc.pColorFormats = &ptCopyDepthFormat;
		ptCopyDepthPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		ptCopyDepthPipelineDesc.mSampleQuality = 0;
		ptCopyDepthPipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
		ptCopyDepthPipelineDesc.pVertexLayout = NULL;
		ptCopyDepthPipelineDesc.pRasterizerState = &rasterStateNoneDesc;
		ptCopyDepthPipelineDesc.pDepthState = &depthStateDisabledDesc;
		ptCopyDepthPipelineDesc.pBlendState = NULL;
		addPipeline(pRenderer, &desc, &pPipelinePTCopyDepth);

		// PT generate mips pipeline
		desc.mType = PIPELINE_TYPE_COMPUTE;
		desc.mComputeDesc = {};
		ComputePipelineDesc& ptGenMipsPipelineDesc = desc.mComputeDesc;
		ptGenMipsPipelineDesc.pShaderProgram = pShaderPTGenMips;
		ptGenMipsPipelineDesc.pRootSignature = pRootSignaturePTGenMips;
		addPipeline(pRenderer, &desc, &pPipelinePTGenMips);
#endif
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			// AOIT shading pipeline
			desc.mType = PIPELINE_TYPE_GRAPHICS;
			desc.mGraphicsDesc = {};
			GraphicsPipelineDesc& aoitShadePipelineDesc = desc.mGraphicsDesc;
			aoitShadePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
			aoitShadePipelineDesc.pShaderProgram = pShaderAOITShade;
			aoitShadePipelineDesc.pRootSignature = pRootSignatureAOITShade;
			aoitShadePipelineDesc.mRenderTargetCount = 0;
			aoitShadePipelineDesc.pColorFormats = NULL;
			aoitShadePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
			aoitShadePipelineDesc.mSampleQuality = 0;
			aoitShadePipelineDesc.mDepthStencilFormat = TinyImageFormat_D32_SFLOAT;
			aoitShadePipelineDesc.pVertexLayout = &vertexLayoutDefault;
			aoitShadePipelineDesc.pRasterizerState = &rasterStateNoneDesc;
			aoitShadePipelineDesc.pDepthState = &depthStateNoWriteDesc;
			aoitShadePipelineDesc.pBlendState = NULL;
			addPipeline(pRenderer, &desc, &pPipelineAOITShade);

			// AOIT composite pipeline
			desc.mGraphicsDesc = {};
			GraphicsPipelineDesc& aoitCompositePipelineDesc = desc.mGraphicsDesc;
			aoitCompositePipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
			aoitCompositePipelineDesc.pShaderProgram = pShaderAOITComposite;
			aoitCompositePipelineDesc.pRootSignature = pRootSignatureAOITComposite;
			aoitCompositePipelineDesc.mRenderTargetCount = 1;
			aoitCompositePipelineDesc.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
			aoitCompositePipelineDesc.mSampleCount = SAMPLE_COUNT_1;
			aoitCompositePipelineDesc.mSampleQuality = 0;
			aoitCompositePipelineDesc.mDepthStencilFormat = TinyImageFormat_UNDEFINED;
			aoitCompositePipelineDesc.pVertexLayout = NULL;
			aoitCompositePipelineDesc.pRasterizerState = &rasterStateNoneDesc;
			aoitCompositePipelineDesc.pDepthState = &depthStateDisabledDesc;
			aoitCompositePipelineDesc.pBlendState = &blendStateAOITShadeaDesc;
			addPipeline(pRenderer, &desc, &pPipelineAOITComposite);

			// AOIT clear pipeline
			desc.mGraphicsDesc = {};
			GraphicsPipelineDesc& aoitClearPipelineDesc = desc.mGraphicsDesc;
			aoitClearPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
			aoitClearPipelineDesc.pShaderProgram = pShaderAOITClear;
			aoitClearPipelineDesc.pRootSignature = pRootSignatureAOITClear;
			aoitClearPipelineDesc.mRenderTargetCount = 0;
			aoitClearPipelineDesc.pColorFormats = NULL;
			aoitClearPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
			aoitClearPipelineDesc.mSampleQuality = 0;
			aoitClearPipelineDesc.mDepthStencilFormat = pRenderTargetDepth->mFormat;
			aoitClearPipelineDesc.pVertexLayout = NULL;
			aoitClearPipelineDesc.pRasterizerState = &rasterStateNoneDesc;
			aoitClearPipelineDesc.pDepthState = &depthStateDisabledDesc;
			aoitClearPipelineDesc.pBlendState = NULL;
			addPipeline(pRenderer, &desc, &pPipelineAOITClear);
		}
#endif
	}

	void DestroyPipelines()
	{
		removePipeline(pRenderer, pPipelineSkybox);
#if USE_SHADOWS != 0
		removePipeline(pRenderer, pPipelineShadow);
		removePipeline(pRenderer, pPipelineGaussianBlur);
#if PT_USE_CAUSTICS != 0
		removePipeline(pRenderer, pPipelinePTGaussianBlur);
		removePipeline(pRenderer, pPipelinePTShadow);
		removePipeline(pRenderer, pPipelinePTDownsample);
		removePipeline(pRenderer, pPipelinePTCopyShadowDepth);
#endif
#endif
		removePipeline(pRenderer, pPipelineForward);
		removePipeline(pRenderer, pPipelineTransparentForward);
		removePipeline(pRenderer, pPipelineWBOITShade);
		removePipeline(pRenderer, pPipelineWBOITComposite);
		removePipeline(pRenderer, pPipelineWBOITVShade);
		removePipeline(pRenderer, pPipelineWBOITVComposite);
		removePipeline(pRenderer, pPipelinePTShade);
		removePipeline(pRenderer, pPipelinePTComposite);
#if PT_USE_DIFFUSION != 0
		removePipeline(pRenderer, pPipelinePTCopyDepth);
		removePipeline(pRenderer, pPipelinePTGenMips);
#endif
#if AOIT_ENABLE
		if (pRenderer->pActiveGpuSettings->mROVsSupported)
		{
			removePipeline(pRenderer, pPipelineAOITShade);
			removePipeline(pRenderer, pPipelineAOITComposite);
			removePipeline(pRenderer, pPipelineAOITClear);
		}
#endif
	}
};

void GuiController::UpdateDynamicUI()
{
	if (gTransparencyType != GuiController::currentTransparencyType)
	{
		if (GuiController::currentTransparencyType == TRANSPARENCY_TYPE_ALPHA_BLEND)
			GuiController::alphaBlendDynamicWidgets.HideWidgets(pGuiWindow);
		else if (GuiController::currentTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT)
			GuiController::weightedBlendedOitDynamicWidgets.HideWidgets(pGuiWindow);
		else if (GuiController::currentTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION)
			GuiController::weightedBlendedOitVolitionDynamicWidgets.HideWidgets(pGuiWindow);

		if (gTransparencyType == TRANSPARENCY_TYPE_ALPHA_BLEND)
			GuiController::alphaBlendDynamicWidgets.ShowWidgets(pGuiWindow);
		else if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT)
			GuiController::weightedBlendedOitDynamicWidgets.ShowWidgets(pGuiWindow);
		else if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION)
			GuiController::weightedBlendedOitVolitionDynamicWidgets.ShowWidgets(pGuiWindow);

		GuiController::currentTransparencyType = (TransparencyType)gTransparencyType;
	}
}

void GuiController::AddGui()
{
	static const char* transparencyTypeNames[] = {
		"Alpha blended",
		"(WBOIT) Weighted blended order independent transparency",
		"(WBOIT) Weighted blended order independent transparency - Volition",
		"(PT) Phenomenological transparency",
#if AOIT_ENABLE
		"(AOIT) Adaptive order independent transparency",
#endif
		NULL    //needed for unix
	};

	static const uint32_t transparencyTypeValues[] = {
		TRANSPARENCY_TYPE_ALPHA_BLEND,
		TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT,
		TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION,
		TRANSPARENCY_TYPE_PHENOMENOLOGICAL,
#if AOIT_ENABLE
		TRANSPARENCY_TYPE_ADAPTIVE_OIT,
#endif
		0    //needed for unix
	};

	uint32_t dropDownCount = 4;
#if AOIT_ENABLE
	if (pRenderer->pActiveGpuSettings->mROVsSupported)
		dropDownCount = 5;
#endif

	DropdownWidget ddTestScripts("Test Scripts", &gCurrentScriptIndex, gTestScripts, gScriptIndexes, sizeof(gTestScripts) / sizeof(gTestScripts[0]));
	ButtonWidget bRunScript("Run");
	bRunScript.pOnEdited = RunScript;
	pGuiWindow->AddWidget(ddTestScripts);
	pGuiWindow->AddWidget(bRunScript);

	pGuiWindow->AddWidget(
		DropdownWidget("Transparency Type", &gTransparencyType, transparencyTypeNames, transparencyTypeValues, dropDownCount));

	// TRANSPARENCY_TYPE_ALPHA_BLEND Widgets
	{
		GuiController::alphaBlendDynamicWidgets.AddWidget(
			LabelWidget("Blend Settings"));

		GuiController::alphaBlendDynamicWidgets.AddWidget(
			CheckboxWidget("Sort Objects", &gAlphaBlendSettings.mSortObjects));

		GuiController::alphaBlendDynamicWidgets.AddWidget(
			CheckboxWidget("Sort Particles", &gAlphaBlendSettings.mSortParticles));
	}
	// TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT Widgets
	{
		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(
			LabelWidget("Blend Settings"));

		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(
			SliderFloatWidget("Color Resistance", &gWBOITSettingsData.mColorResistance, 1.0f, 25.0f));

		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(
			SliderFloatWidget("Range Adjustment", &gWBOITSettingsData.mRangeAdjustment, 0.0f, 1.0f));

		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(
			SliderFloatWidget("Depth Range", &gWBOITSettingsData.mDepthRange, 0.1f, 500.0f));

		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(
			SliderFloatWidget("Ordering Strength", &gWBOITSettingsData.mOrderingStrength, 0.1f, 25.0f));

		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(
			SliderFloatWidget("Underflow Limit", &gWBOITSettingsData.mUnderflowLimit, 1e-4f, 1e-1f, 1e-4f));

		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(
			SliderFloatWidget("Overflow Limit", &gWBOITSettingsData.mOverflowLimit, 3e1f, 3e4f));

		ButtonWidget resetButton("Reset");
		resetButton.pOnDeactivatedAfterEdit = ([]() { gWBOITSettingsData = WBOITSettings(); });
		GuiController::weightedBlendedOitDynamicWidgets.AddWidget(resetButton);
	}
	// TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION Widgets
	{
		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			LabelWidget("Blend Settings"));

		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			SliderFloatWidget("Opacity Sensitivity", &gWBOITVolitionSettingsData.mOpacitySensitivity, 1.0f, 25.0f));

		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			SliderFloatWidget("Weight Bias", &gWBOITVolitionSettingsData.mWeightBias, 0.0f, 25.0f));

		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			SliderFloatWidget("Precision Scalar", &gWBOITVolitionSettingsData.mPrecisionScalar, 100.0f, 100000.0f));

		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			SliderFloatWidget("Maximum Weight", &gWBOITVolitionSettingsData.mMaximumWeight, 0.1f, 100.0f));

		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			SliderFloatWidget("Maximum Color Value", &gWBOITVolitionSettingsData.mMaximumColorValue, 100.0f, 10000.0f));

		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			SliderFloatWidget("Additive Sensitivity", &gWBOITVolitionSettingsData.mAdditiveSensitivity, 0.1f, 25.0f));

		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(
			SliderFloatWidget("Emissive Sensitivity", &gWBOITVolitionSettingsData.mEmissiveSensitivity, 0.01f, 1.0f));

		ButtonWidget resetButton("Reset");
		resetButton.pOnDeactivatedAfterEdit = ([]() { gWBOITVolitionSettingsData = WBOITVolitionSettings(); });
		GuiController::weightedBlendedOitVolitionDynamicWidgets.AddWidget(resetButton);
	}

	pGuiWindow->AddWidget(LabelWidget("Light Settings"));

	const float3 lightPosBound(10.0f);
	pGuiWindow->AddWidget(SliderFloat3Widget("Light Position", &gLightCpuSettings.mLightPosition, -lightPosBound, lightPosBound, float3(0.1f)));

	if (gTransparencyType == TRANSPARENCY_TYPE_ALPHA_BLEND)
	{
		GuiController::currentTransparencyType = TRANSPARENCY_TYPE_ALPHA_BLEND;
		GuiController::alphaBlendDynamicWidgets.ShowWidgets(pGuiWindow);
	}
	else if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT)
	{
		GuiController::currentTransparencyType = TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT;
		GuiController::weightedBlendedOitDynamicWidgets.ShowWidgets(pGuiWindow);
	}
	else if (gTransparencyType == TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION)
	{
		GuiController::currentTransparencyType = TRANSPARENCY_TYPE_WEIGHTED_BLENDED_OIT_VOLITION;
		GuiController::weightedBlendedOitVolitionDynamicWidgets.ShowWidgets(pGuiWindow);
	}
	else if (gTransparencyType == TRANSPARENCY_TYPE_PHENOMENOLOGICAL)
	{
		GuiController::currentTransparencyType = TRANSPARENCY_TYPE_PHENOMENOLOGICAL;
	}
#if AOIT_ENABLE
	else if (gTransparencyType == TRANSPARENCY_TYPE_ADAPTIVE_OIT && pRenderer->pActiveGpuSettings->mROVsSupported)
	{
		GuiController::currentTransparencyType = TRANSPARENCY_TYPE_ADAPTIVE_OIT;
	}
#endif
}

void GuiController::RemoveGui()
{
	alphaBlendDynamicWidgets.Destroy();
	weightedBlendedOitDynamicWidgets.Destroy();
	weightedBlendedOitVolitionDynamicWidgets.Destroy();
}

DEFINE_APPLICATION_MAIN(Transparency)
