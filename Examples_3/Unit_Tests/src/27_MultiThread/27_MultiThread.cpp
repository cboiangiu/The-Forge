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

/********************************************************************************************************
*
* The Forge - ANIMATION - MULTI THREADED UNIT TEST
*
* The purpose of this demo is to show how to playback a clip using the
* animnation middleware on multiple rigs in a multi threaded fashion
*
*********************************************************************************************************/

// Interfaces
#include "../../../../Common_3/OS/Interfaces/ICameraController.h"
#include "../../../../Common_3/OS/Interfaces/IApp.h"
#include "../../../../Common_3/OS/Interfaces/ILog.h"
#include "../../../../Common_3/OS/Interfaces/IFileSystem.h"
#include "../../../../Common_3/OS/Interfaces/ITime.h"
#include "../../../../Common_3/OS/Interfaces/IThread.h"
#include "../../../../Common_3/OS/Interfaces/IProfiler.h"
#include "../../../../Common_3/OS/Interfaces/IInput.h"

// Rendering
#include "../../../../Common_3/Renderer/IRenderer.h"
#include "../../../../Common_3/Renderer/IResourceLoader.h"

// Middleware packages
#include "../../../../Middleware_3/Animation/SkeletonBatcher.h"
#include "../../../../Middleware_3/Animation/AnimatedObject.h"
#include "../../../../Middleware_3/Animation/Animation.h"
#include "../../../../Middleware_3/Animation/Clip.h"
#include "../../../../Middleware_3/Animation/ClipController.h"
#include "../../../../Middleware_3/Animation/Rig.h"

#include "../../../../Middleware_3/UI/AppUI.h"
// tiny stl
#include "../../../../Common_3/ThirdParty/OpenSource/EASTL/vector.h"
#include "../../../../Common_3/ThirdParty/OpenSource/EASTL/string.h"

// Math
#include "../../../../Common_3/OS/Math/MathTypes.h"

#include "../../../../Common_3/OS/Core/ThreadSystem.h"

// Memory
#include "../../../../Common_3/OS/Interfaces/IMemory.h"

//--------------------------------------------------------------------------------------------
// RENDERING PIPELINE DATA
//--------------------------------------------------------------------------------------------
const uint32_t gImageCount = 3;
ProfileToken   gGpuProfileToken;

uint32_t       gFrameIndex = 0;
Renderer*      pRenderer = NULL;

Queue*   pGraphicsQueue = NULL;
CmdPool* pCmdPools[gImageCount];
Cmd*     pCmds[gImageCount];

SwapChain*    pSwapChain = NULL;
RenderTarget* pDepthBuffer = NULL;
Fence*        pRenderCompleteFences[gImageCount] = { NULL };
Semaphore*    pImageAcquiredSemaphore = NULL;
Semaphore*    pRenderCompleteSemaphores[gImageCount] = { NULL };

VirtualJoystickUI gVirtualJoystick;

Shader*   pSkeletonShader = NULL;
Buffer*   pJointVertexBuffer = NULL;
Buffer*   pBoneVertexBuffer = NULL;
Pipeline* pSkeletonPipeline = NULL;
int       gNumberOfJointPoints;
int       gNumberOfBonePoints;

Shader*        pPlaneDrawShader = NULL;
Buffer*        pPlaneVertexBuffer = NULL;
Pipeline*      pPlaneDrawPipeline = NULL;
RootSignature* pRootSignature = NULL;
DescriptorSet* pDescriptorSet = NULL;

struct UniformBlockPlane
{
	mat4 mProjectView;
	mat4 mToWorldMat;
};
UniformBlockPlane gUniformDataPlane;

Buffer* pPlaneUniformBuffer[gImageCount] = { NULL };

//--------------------------------------------------------------------------------------------
// CAMERA CONTROLLER & SYSTEMS (File/Log/UI)
//--------------------------------------------------------------------------------------------

ICameraController* pCameraController = NULL;
UIApp         gAppUI;
GuiComponent* pStandaloneControlsGUIWindow = NULL;

TextDrawDesc gFrameTimeDraw = TextDrawDesc(0, 0xff00ffff, 18);

//--------------------------------------------------------------------------------------------
// ANIMATION DATA
//--------------------------------------------------------------------------------------------

unsigned int       gNumRigs = 50;    // Determines the number of rigs to update and draw
const unsigned int kMaxNumRigs = 4096;

// AnimatedObjects
AnimatedObject gStickFigureAnimObjects[kMaxNumRigs];

// Animations
Animation gWalkAnimations[kMaxNumRigs];

// ClipControllers
ClipController gWalkClipControllers[kMaxNumRigs];

// Clips
Clip gWalkClip;

// Rigs
Rig gStickFigureRigs[kMaxNumRigs];

// SkeletonBatcher
SkeletonBatcher gSkeletonBatcher;

// Filenames
const char* gStickFigureName = "stickFigure/skeleton.ozz";
const char* gWalkClipName = "stickFigure/animations/walk.ozz";

float* pJointPoints;
float* pBonePoints;

const int   gSphereResolution = 3;                    // Increase for higher resolution joint spheres
const float gBoneWidthRatio = 0.2f;                   // Determines how far along the bone to put the max width [0,1]
const float gJointRadius = gBoneWidthRatio * 0.5f;    // set to replicate Ozz skeleton

// Timer to get animationsystem update time
static HiresTimer gAnimationUpdateTimer;

//--------------------------------------------------------------------------------------------
// MULTI THREADING DATA
//--------------------------------------------------------------------------------------------

// Toggle for enabling/disabling threading through UI
bool gEnableThreading = true;
bool gAutomateThreading = false;

// Maximum number of tasks to be threaded
const unsigned int kMaxTaskCount = kMaxNumRigs;

// Number of rigs per task that will be adjusted by the UI
unsigned int gGrainSize = 32;

struct ThreadData
{
	AnimatedObject* mAnimatedObject;
	float           mDeltaTime;
	unsigned int    mNumberSystems;
};
ThreadData gThreadData[kMaxTaskCount];

struct ThreadSkeletonData
{
	unsigned int	mFrameNumber;
	unsigned int    mNumberRigs;
	uint32_t        mOffset;
};
ThreadSkeletonData gThreadSkeletonData[kMaxTaskCount];

ThreadSystem* pThreadSystem = NULL;

//--------------------------------------------------------------------------------------------
// UI DATA
//--------------------------------------------------------------------------------------------
struct UIData
{
	struct ThreadingControlData
	{
		bool*         mEnableThreading = &gEnableThreading;
		bool* mAutomateThreading = &gAutomateThreading;
		unsigned int* mGrainSize = &gGrainSize;
	};
	ThreadingControlData mThreadingControl;

	struct SampleControlData
	{
		unsigned int* mNumberOfRigs = &gNumRigs;
	};
	SampleControlData mSampleControl;

	struct GeneralSettingsData
	{
		bool mDrawPlane = true;
	};
	GeneralSettingsData mGeneralSettings;
};
UIData gUIData;

const char* gTestScripts[] = { "Test.lua", "Test_Reset.lua" };
uint32_t gScriptIndexes[] = { 0 };
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

//--------------------------------------------------------------------------------------------
// APP CODE
//--------------------------------------------------------------------------------------------
class MultiThread: public IApp
{
	public:
	bool Init()
	{
        // FILE PATHS
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_SOURCES,  "Shaders");
		fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG,   RD_SHADER_BINARIES, "CompiledShaders");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_GPU_CONFIG,      "GPUCfg");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_TEXTURES,        "Textures");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_MESHES,          "Meshes");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_FONTS,           "Fonts");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_ANIMATIONS,      "Animation");
		fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SCRIPTS,		   "Scripts");

		// GENERATE VERTEX BUFFERS
		//

		// Generate joint vertex buffer

		generateSpherePoints(&pJointPoints, &gNumberOfJointPoints, gSphereResolution, gJointRadius);

		// Generate bone vertex buffer
		generateBonePoints(&pBonePoints, &gNumberOfBonePoints, gBoneWidthRatio);

		
        
        // RIGS
        //
		// Initialize the rig with the path to its ozz file and its rendering details
		for (unsigned int i = 0; i < kMaxNumRigs; i++)
		{
			gStickFigureRigs[i].Initialize(RD_ANIMATIONS, gStickFigureName);

			// Add the rig to the list of skeletons to render
			gSkeletonBatcher.AddRig(&gStickFigureRigs[i]);

			// alternate the rig colors
			if (i % 2 == 1)
			{
				gStickFigureRigs[i].SetJointColor(vec4(.1f, .9f, .1f, 1.f));
				gStickFigureRigs[i].SetBoneColor(vec4(.1f, .2f, .9f, 1.f));
			}
		}

		// CLIPS
		//
		// Since all the skeletons are the same we can just initialize with the first one
		gWalkClip.Initialize(RD_ANIMATIONS, gWalkClipName, &gStickFigureRigs[0]);

		// CLIP CONTROLLERS
		//
		// Initialize with the length of the clip they are controlling
		for (unsigned int i = 0; i < kMaxNumRigs; i++)
		{
			gWalkClipControllers[i].Initialize(gWalkClip.GetDuration());
		}

		// ANIMATIONS
		//
		for (unsigned int i = 0; i < kMaxNumRigs; i++)
		{
			AnimationDesc animationDesc{};
			animationDesc.mRig = &gStickFigureRigs[i];
			animationDesc.mNumLayers = 1;
			animationDesc.mLayerProperties[0].mClip = &gWalkClip;
			animationDesc.mLayerProperties[0].mClipController = &gWalkClipControllers[i];

			gWalkAnimations[i].Initialize(animationDesc);
		}

		// ANIMATED OBJECTS
		//
		const unsigned int gridWidth = 25;
		const unsigned int gridDepth = 10;
		for (unsigned int i = 0; i < kMaxNumRigs; i++)
		{
			gStickFigureAnimObjects[i].Initialize(&gStickFigureRigs[i], &gWalkAnimations[i]);

			// Calculate and set offset for each rig
			vec3 offset = vec3(-8.75f + 0.75f * (i % gridWidth), ((i / gridWidth) / gridDepth) * 2.0f, 8.0f - 2 * ((i / gridWidth) % gridDepth));
			gStickFigureAnimObjects[i].SetRootTransform(mat4::translation(offset));
		}

		/************************************************************************/
		// SETUP THE MAIN CAMERA
		//
		CameraMotionParameters cmp{ 50.0f, 75.0f, 150.0f };
		vec3                   camPos{ -10.0f, 5.0f, 13.0f };
		vec3                   lookAt{ 0.0f, 0.0f, -1.5f };

		pCameraController = createFpsCameraController(camPos, lookAt);
		pCameraController->setMotionParameters(cmp);
		
		// INITIALIZE THREAD SYSTEM
		//
		initThreadSystem(&pThreadSystem);

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
		actionDesc = { InputBindings::FLOAT_RIGHTSTICK, [](InputActionContext* ctx) { return onCameraInput(ctx, 1); }, NULL, 20.0f, 200.0f, 0.5f };
		addInputAction(&actionDesc);
		actionDesc = { InputBindings::FLOAT_LEFTSTICK, [](InputActionContext* ctx) { return onCameraInput(ctx, 0); }, NULL, 20.0f, 200.0f, 1.0f };
		addInputAction(&actionDesc);
		actionDesc = { InputBindings::BUTTON_NORTH, [](InputActionContext* ctx) { pCameraController->resetView(); return true; } };
		addInputAction(&actionDesc);

		return true;
	}

	void Exit()
	{
		exitInputSystem();
		shutdownThreadSystem(pThreadSystem);

		// Rigs
		for (unsigned int i = 0; i < kMaxNumRigs; i++)
		{
			gStickFigureRigs[i].Destroy();
		}

		// Clips
		gWalkClip.Destroy();

		// Animations
		for (unsigned int i = 0; i < kMaxNumRigs; i++)
		{
			gWalkAnimations[i].Destroy();
		}

		// AnimatedObjects
		for (unsigned int i = 0; i < kMaxNumRigs; i++)
		{
			gStickFigureAnimObjects[i].Destroy();
		}

		destroyCameraController(pCameraController);

		// Need to free memory;
		tf_free(pJointPoints);
		tf_free(pBonePoints);
	}

	bool Load()
	{
		if (mSettings.mResetGraphics || !pRenderer) 
		{
			// WINDOW AND RENDERER SETUP
		//
			RendererDesc settings = { 0 };
			initRenderer(GetName(), &settings, &pRenderer);
			if (!pRenderer)    //check for init success
				return false;

			// CREATE COMMAND LIST AND GRAPHICS/COMPUTE QUEUES
			//
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

			// INITIALIZE RESOURCE/DEBUG SYSTEMS
			//
			initResourceLoaderInterface(pRenderer);

			if (!gVirtualJoystick.Init(pRenderer, "circlepad"))
				return false;

			// INITIALIZE THE USER INTERFACE
			//
			if (!gAppUI.Init(pRenderer))
				return false;
			gAppUI.AddTestScripts(gTestScripts, sizeof(gTestScripts) / sizeof(gTestScripts[0]));

			gAppUI.LoadFont("TitilliumText/TitilliumText-Bold.otf");

			initProfiler();
			initProfilerUI(&gAppUI, mSettings.mWidth, mSettings.mHeight);

			gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

			// INITIALIZE PIPILINE STATES
			//
			ShaderLoadDesc planeShader = {};
			planeShader.mStages[0] = { "plane.vert", NULL, 0 };
			planeShader.mStages[1] = { "plane.frag", NULL, 0 };
			ShaderLoadDesc basicShader = {};
			basicShader.mStages[0] = { "basic.vert", NULL, 0 };
			basicShader.mStages[1] = { "basic.frag", NULL, 0 };

			addShader(pRenderer, &planeShader, &pPlaneDrawShader);
			addShader(pRenderer, &basicShader, &pSkeletonShader);

			Shader*           shaders[] = { pSkeletonShader, pPlaneDrawShader };
			RootSignatureDesc rootDesc = {};
			rootDesc.mShaderCount = 2;
			rootDesc.ppShaders = shaders;
			addRootSignature(pRenderer, &rootDesc, &pRootSignature);

			DescriptorSetDesc setDesc = { pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_DRAW, gImageCount };
			addDescriptorSet(pRenderer, &setDesc, &pDescriptorSet);

			uint64_t       jointDataSize = gNumberOfJointPoints * sizeof(float);
			BufferLoadDesc jointVbDesc = {};
			jointVbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
			jointVbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
			jointVbDesc.mDesc.mSize = jointDataSize;
			jointVbDesc.pData = pJointPoints;
			jointVbDesc.ppBuffer = &pJointVertexBuffer;
			addResource(&jointVbDesc, NULL);

			uint64_t       boneDataSize = gNumberOfBonePoints * sizeof(float);
			BufferLoadDesc boneVbDesc = {};
			boneVbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
			boneVbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
			boneVbDesc.mDesc.mSize = boneDataSize;
			boneVbDesc.pData = pBonePoints;
			boneVbDesc.ppBuffer = &pBoneVertexBuffer;
			addResource(&boneVbDesc, NULL);

			//Generate plane vertex buffer
			float planePoints[] = { -10.0f, 0.0f, -10.0f, 1.0f, 0.0f, 0.0f, -10.0f, 0.0f, 10.0f,  1.0f, 1.0f, 0.0f,
									10.0f,  0.0f, 10.0f,  1.0f, 1.0f, 1.0f, 10.0f,  0.0f, 10.0f,  1.0f, 1.0f, 1.0f,
									10.0f,  0.0f, -10.0f, 1.0f, 0.0f, 1.0f, -10.0f, 0.0f, -10.0f, 1.0f, 0.0f, 0.0f };

			uint64_t       planeDataSize = 6 * 6 * sizeof(float);
			BufferLoadDesc planeVbDesc = {};
			planeVbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
			planeVbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
			planeVbDesc.mDesc.mSize = planeDataSize;
			planeVbDesc.pData = planePoints;
			planeVbDesc.ppBuffer = &pPlaneVertexBuffer;
			addResource(&planeVbDesc, NULL);

			BufferLoadDesc ubDesc = {};
			ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
			ubDesc.mDesc.mSize = sizeof(UniformBlockPlane);
			ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
			ubDesc.pData = NULL;
			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				ubDesc.ppBuffer = &pPlaneUniformBuffer[i];
				addResource(&ubDesc, NULL);
			}
			/************************************************************************/
			// SETUP ANIMATION STRUCTURES
			/************************************************************************/
			// SKELETON RENDERER
			//

			// Set up details for rendering the skeletons
			SkeletonRenderDesc skeletonRenderDesc = {};
			skeletonRenderDesc.mRenderer = pRenderer;
			skeletonRenderDesc.mSkeletonPipeline = pSkeletonPipeline;
			skeletonRenderDesc.mRootSignature = pRootSignature;
			skeletonRenderDesc.mJointVertexBuffer = pJointVertexBuffer;
			skeletonRenderDesc.mNumJointPoints = gNumberOfJointPoints;
			skeletonRenderDesc.mDrawBones = true;
			skeletonRenderDesc.mBoneVertexBuffer = pBoneVertexBuffer;
			skeletonRenderDesc.mNumBonePoints = gNumberOfBonePoints;
			skeletonRenderDesc.mBoneVertexStride = sizeof(float) * 6;
			skeletonRenderDesc.mJointVertexStride = sizeof(float) * 6;
			gSkeletonBatcher.Initialize(skeletonRenderDesc);

			// Add the GUI Panels/Windows
			const TextDrawDesc UIPanelWindowTitleTextDesc = { 0, 0xffff00ff, 16 };

			vec2    UIPosition = { mSettings.mWidth * 0.01f, mSettings.mHeight * 0.15f };
			vec2    UIPanelSize = { 650, 1000 };
			GuiDesc guiDesc(UIPosition, UIPanelSize, UIPanelWindowTitleTextDesc);
			pStandaloneControlsGUIWindow = gAppUI.AddGuiComponent("Multiple Rigs", &guiDesc);

			// SET gUIData MEMBERS THAT NEED POINTERS TO ANIMATION DATA
			//

			// SET UP GUI BASED ON gUIData STRUCT
			//
			{
				// THREADING CONTROL
				//
				CollapsingHeaderWidget CollapsingThreadingControlWidgets("Threading Control");

				// EnableThreading - Checkbox
				CollapsingThreadingControlWidgets.AddSubWidget(SeparatorWidget());
				CollapsingThreadingControlWidgets.AddSubWidget(CheckboxWidget("Enable Threading", gUIData.mThreadingControl.mEnableThreading));

				// AutomateThreading - Checkbox
				CollapsingThreadingControlWidgets.AddSubWidget(CheckboxWidget("Automate Threading", gUIData.mThreadingControl.mAutomateThreading));

				// GrainSize - Slider
				unsigned uintValMin = 1;
				unsigned uintValMax = kMaxNumRigs;
				unsigned sliderStepSizeUint = 1;

				CollapsingThreadingControlWidgets.AddSubWidget(SeparatorWidget());
				CollapsingThreadingControlWidgets.AddSubWidget(
					SliderUintWidget("Grain Size", gUIData.mThreadingControl.mGrainSize, uintValMin, uintValMax, sliderStepSizeUint));
				CollapsingThreadingControlWidgets.AddSubWidget(SeparatorWidget());

				// SAMPLE CONTROL
				//
				CollapsingHeaderWidget CollapsingSampleControlWidgets("Sample Control");

				// NumRigs - Slider
				uintValMin = 1;
				uintValMax = kMaxNumRigs;
				sliderStepSizeUint = 1;

				CollapsingSampleControlWidgets.AddSubWidget(SeparatorWidget());
				CollapsingSampleControlWidgets.AddSubWidget(
					SliderUintWidget("Number of Rigs", gUIData.mSampleControl.mNumberOfRigs, uintValMin, uintValMax, sliderStepSizeUint));
				CollapsingSampleControlWidgets.AddSubWidget(SeparatorWidget());

				// GENERAL SETTINGS
				//
				CollapsingHeaderWidget CollapsingGeneralSettingsWidgets("General Settings");

				// DrawPlane - Checkbox
				CollapsingGeneralSettingsWidgets.AddSubWidget(SeparatorWidget());
				CollapsingGeneralSettingsWidgets.AddSubWidget(CheckboxWidget("Draw Plane", &gUIData.mGeneralSettings.mDrawPlane));
				CollapsingGeneralSettingsWidgets.AddSubWidget(SeparatorWidget());

				// Add all widgets to the window

				// Reset graphics with a button.
				ButtonWidget testGPUReset("ResetGraphicsDevice");
				testGPUReset.pOnEdited = testGraphicsReset;
				pStandaloneControlsGUIWindow->AddWidget(testGPUReset);

				pStandaloneControlsGUIWindow->AddWidget(CollapsingThreadingControlWidgets);
				pStandaloneControlsGUIWindow->AddWidget(CollapsingSampleControlWidgets);
				pStandaloneControlsGUIWindow->AddWidget(CollapsingGeneralSettingsWidgets);

				DropdownWidget ddTestScripts("Test Scripts", &gCurrentScriptIndex, gTestScripts, gScriptIndexes, sizeof(gTestScripts) / sizeof(gTestScripts[0]));
				ButtonWidget bRunScript("Run");
				bRunScript.pOnEdited = RunScript;
				pStandaloneControlsGUIWindow->AddWidget(ddTestScripts);
				pStandaloneControlsGUIWindow->AddWidget(bRunScript);
			}

			waitForAllResourceLoads();

			// Prepare descriptor sets
			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				DescriptorData params[1] = {};
				params[0].pName = "uniformBlock";
				params[0].ppBuffers = &pPlaneUniformBuffer[i];
				updateDescriptorSet(pRenderer, i, pDescriptorSet, 1, params);
			}
		}

		// INITIALIZE SWAP-CHAIN AND DEPTH BUFFER
		//
		if (!addSwapChain())
			return false;
		if (!addDepthBuffer())
			return false;

		// LOAD USER INTERFACE
		//
		if (!gAppUI.Load(pSwapChain->ppRenderTargets))
			return false;

		if (!gVirtualJoystick.Load(pSwapChain->ppRenderTargets[0]))
			return false;

		//layout and pipeline for skeleton draw
		VertexLayout vertexLayout = {};
		vertexLayout.mAttribCount = 2;
		vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
		vertexLayout.mAttribs[0].mBinding = 0;
		vertexLayout.mAttribs[0].mLocation = 0;
		vertexLayout.mAttribs[0].mOffset = 0;
		vertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
		vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
		vertexLayout.mAttribs[1].mBinding = 0;
		vertexLayout.mAttribs[1].mLocation = 1;
		vertexLayout.mAttribs[1].mOffset = 3 * sizeof(float);

		RasterizerStateDesc rasterizerStateDesc = {};
		rasterizerStateDesc.mCullMode = CULL_MODE_NONE;

		RasterizerStateDesc skeletonRasterizerStateDesc = {};
		skeletonRasterizerStateDesc.mCullMode = CULL_MODE_FRONT;

		DepthStateDesc depthStateDesc = {};
		depthStateDesc.mDepthTest = true;
		depthStateDesc.mDepthWrite = true;
		depthStateDesc.mDepthFunc = CMP_LEQUAL;

		PipelineDesc desc = {};
		desc.mType = PIPELINE_TYPE_GRAPHICS;
		GraphicsPipelineDesc& pipelineSettings = desc.mGraphicsDesc;
		pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		pipelineSettings.mRenderTargetCount = 1;
		pipelineSettings.pDepthState = &depthStateDesc;
		pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
		pipelineSettings.mSampleCount = pSwapChain->ppRenderTargets[0]->mSampleCount;
		pipelineSettings.mSampleQuality = pSwapChain->ppRenderTargets[0]->mSampleQuality;
		pipelineSettings.mDepthStencilFormat = pDepthBuffer->mFormat;
		pipelineSettings.pRootSignature = pRootSignature;
		pipelineSettings.pShaderProgram = pSkeletonShader;
		pipelineSettings.pVertexLayout = &vertexLayout;
		pipelineSettings.pRasterizerState = &skeletonRasterizerStateDesc;
		addPipeline(pRenderer, &desc, &pSkeletonPipeline);

		// Update the mSkeletonPipeline pointer now that the pipeline has been loaded
		gSkeletonBatcher.LoadPipeline(pSkeletonPipeline);

		//layout and pipeline for plane draw
		vertexLayout = {};
		vertexLayout.mAttribCount = 2;
		vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32A32_SFLOAT;
		vertexLayout.mAttribs[0].mBinding = 0;
		vertexLayout.mAttribs[0].mLocation = 0;
		vertexLayout.mAttribs[0].mOffset = 0;
		vertexLayout.mAttribs[1].mSemantic = SEMANTIC_TEXCOORD0;
		vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32_SFLOAT;
		vertexLayout.mAttribs[1].mBinding = 0;
		vertexLayout.mAttribs[1].mLocation = 1;
		vertexLayout.mAttribs[1].mOffset = 4 * sizeof(float);

		pipelineSettings.pDepthState = NULL;
		pipelineSettings.pRasterizerState = &rasterizerStateDesc;
		pipelineSettings.pShaderProgram = pPlaneDrawShader;
		addPipeline(pRenderer, &desc, &pPlaneDrawPipeline);

		return true;
	}

	void Unload()
	{
		waitQueueIdle(pGraphicsQueue);

		gAppUI.Unload();

		gVirtualJoystick.Unload();

		removePipeline(pRenderer, pPlaneDrawPipeline);
		removePipeline(pRenderer, pSkeletonPipeline);

		removeSwapChain(pRenderer, pSwapChain);
		removeRenderTarget(pRenderer, pDepthBuffer);

		if (mSettings.mQuit || mSettings.mResetGraphics) 
		{
			waitThreadSystemIdle(pThreadSystem);
			exitProfilerUI();

			exitProfiler();

			// Skeleton Renderer
			gSkeletonBatcher.Destroy();

			gVirtualJoystick.Exit();

			gAppUI.Exit();

			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				removeResource(pPlaneUniformBuffer[i]);
			}

			removeResource(pJointVertexBuffer);
			removeResource(pBoneVertexBuffer);
			removeResource(pPlaneVertexBuffer);

			removeShader(pRenderer, pSkeletonShader);
			removeShader(pRenderer, pPlaneDrawShader);
			removeDescriptorSet(pRenderer, pDescriptorSet);
			removeRootSignature(pRenderer, pRootSignature);

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
		}
	}

	void Update(float deltaTime)
	{
		updateInputSystem(mSettings.mWidth, mSettings.mHeight);

		pCameraController->update(deltaTime);

		/************************************************************************/
		// Scene Update
		/************************************************************************/

		// update camera with time
		mat4 viewMat = pCameraController->getViewMatrix();

		const float aspectInverse = (float)mSettings.mHeight / (float)mSettings.mWidth;
		const float horizontal_fov = PI / 2.0f;
		mat4        projMat = mat4::perspective(horizontal_fov, aspectInverse, 0.1f, 1000.0f);
		mat4        projViewMat = projMat * viewMat;

		vec3 lightPos = vec3(0.0f, 10.0f, 2.0f);
		vec3 lightColor = vec3(1.0f, 1.0f, 1.0f);

		/************************************************************************/
		// GUI
		/************************************************************************/
		gAppUI.Update(deltaTime);

		/************************************************************************/
		// Animation
		/************************************************************************/
		gAnimationUpdateTimer.Reset();

		// Update the animated objects amd pose the rigs based on the animated object's updated values for this frame
		gSkeletonBatcher.SetActiveRigs(gNumRigs);
		// Threading
		if (gEnableThreading)
		{
			if (gAutomateThreading)
			{
				uint32_t threadCount = getThreadSystemThreadCount(pThreadSystem);
				gGrainSize = max(1U, gNumRigs / threadCount);
			}

			gGrainSize = min(gGrainSize, gNumRigs);
			unsigned int taskCount = max(1U, gNumRigs / gGrainSize);

			// Submit taskCount number of jobs
			for (unsigned int i = 0; i < taskCount; i++)
			{
				gThreadData[i].mAnimatedObject = &gStickFigureAnimObjects[gGrainSize * i];
				gThreadData[i].mDeltaTime = deltaTime;
				gThreadData[i].mNumberSystems = gGrainSize;
			}
			addThreadSystemRangeTask(pThreadSystem, &MultiThread::AnimatedObjectThreadedUpdate, gThreadData, taskCount);

			// If there is a remainder, submit another job to finish it
			unsigned int remainder = (uint32_t)max(0, (int32_t)gNumRigs - (int32_t)(taskCount * gGrainSize));
			if (remainder != 0)
			{
				gThreadData[taskCount].mAnimatedObject = &gStickFigureAnimObjects[gGrainSize * taskCount];
				gThreadData[taskCount].mDeltaTime = deltaTime;
				gThreadData[taskCount].mNumberSystems = remainder;

				addThreadSystemTask(pThreadSystem, &MultiThread::AnimatedObjectThreadedUpdate, &gThreadData[taskCount]);
			}
		}
		// Naive
		else
		{
			for (unsigned int i = 0; i < gNumRigs; ++i)
			{
				if (!gStickFigureAnimObjects[i].Update(deltaTime))
					LOGF(eERROR, "Animation NOT Updating!");

				gStickFigureAnimObjects[i].PoseRig();
			}

			// Record animation update time
			gAnimationUpdateTimer.GetUSec(true);
		}

		// Update uniforms that will be shared between all skeletons
		gSkeletonBatcher.SetSharedUniforms(projViewMat, lightPos, lightColor);

		/************************************************************************/
		// Plane
		/************************************************************************/
		gUniformDataPlane.mProjectView = projViewMat;
		gUniformDataPlane.mToWorldMat = mat4::identity();

		if (gEnableThreading)
		{
			// Ensure all jobs are finished before proceeding
			while (assistThreadSystem(pThreadSystem)) {};
			waitThreadSystemIdle(pThreadSystem);

			// Record animation update time
			gAnimationUpdateTimer.GetUSec(true);
		}
	}

	void Draw()
	{
		uint32_t swapchainImageIndex;
		acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

		// UPDATE UNIFORM BUFFERS
		//

		// Update all the instanced uniform data for each batch of joints and bones
		// Threading
		if (gEnableThreading)
		{
			unsigned int taskCount = max(1U, gNumRigs / gGrainSize);

			// Submit taskCount number of jobs
			for (unsigned int i = 0; i < taskCount; ++i)
			{
				gThreadSkeletonData[i].mFrameNumber = gFrameIndex;
				gThreadSkeletonData[i].mNumberRigs = gGrainSize;
				gThreadSkeletonData[i].mOffset = i * gGrainSize;
			}
			addThreadSystemRangeTask(pThreadSystem, &MultiThread::SkeletonBatchUniformsThreaded, gThreadSkeletonData, taskCount);

			// If there is a remainder, submit another job to finish it
			unsigned int remainder = (uint32_t)max(0, (int32_t)gNumRigs - (int32_t)(taskCount * gGrainSize));
			if (remainder != 0)
			{
				gThreadSkeletonData[taskCount].mFrameNumber = gFrameIndex;
				gThreadSkeletonData[taskCount].mNumberRigs = remainder;
				gThreadSkeletonData[taskCount].mOffset = taskCount * gGrainSize;

				addThreadSystemTask(pThreadSystem, &MultiThread::SkeletonBatchUniformsThreaded, &gThreadSkeletonData[taskCount]);
			}

			// Ensure all jobs are finished before proceeding
			while (assistThreadSystem(pThreadSystem)) {};
			waitThreadSystemIdle(pThreadSystem);
		}
		else
		{
			gSkeletonBatcher.SetPerInstanceUniforms(gFrameIndex, gNumRigs);
		}

		BufferUpdateDesc planeViewProjCbv = { pPlaneUniformBuffer[gFrameIndex] };
		beginUpdateResource(&planeViewProjCbv);
		*(UniformBlockPlane*)planeViewProjCbv.pMappedData = gUniformDataPlane;
		endUpdateResource(&planeViewProjCbv, NULL);

		// FRAME SYNC & ACQUIRE SWAPCHAIN RENDER TARGET
		//
		// Stall if CPU is running "Swap Chain Buffer Count" frames ahead of GPU
		Fence*      pNextFence = pRenderCompleteFences[gFrameIndex];
		FenceStatus fenceStatus;
		getFenceStatus(pRenderer, pNextFence, &fenceStatus);
		if (fenceStatus == FENCE_STATUS_INCOMPLETE)
			waitForFences(pRenderer, 1, &pNextFence);

		resetCmdPool(pRenderer, pCmdPools[gFrameIndex]);

		// Acquire the main render target from the swapchain
		RenderTarget* pRenderTarget = pSwapChain->ppRenderTargets[swapchainImageIndex];
		Semaphore*    pRenderCompleteSemaphore = pRenderCompleteSemaphores[gFrameIndex];
		Fence*        pRenderCompleteFence = pRenderCompleteFences[gFrameIndex];
		Cmd*          cmd = pCmds[gFrameIndex];
		beginCmd(cmd);    // start recording commands

		// start gpu frame profiler
		cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);

		RenderTargetBarrier barriers[] =    // wait for resource transition
		{
			{ pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET },
		};
		cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

		// bind and clear the render target
		LoadActionsDesc loadActions = {};    // render target clean command
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
		loadActions.mClearColorValues[0] = pRenderTarget->mClearValue;
		loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
        loadActions.mClearDepth.depth = 1.0f;
        loadActions.mClearDepth.stencil = 0;
		cmdBindRenderTargets(cmd, 1, &pRenderTarget, pDepthBuffer, &loadActions, NULL, NULL, -1, -1);
		cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth, (float)pRenderTarget->mHeight, 0.0f, 1.0f);
		cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

		//// draw plane
		if (gUIData.mGeneralSettings.mDrawPlane)
		{
			const uint32_t stride = sizeof(float) * 6;
			cmdBeginDebugMarker(cmd, 1, 0, 1, "Draw Plane");
			cmdBindPipeline(cmd, pPlaneDrawPipeline);
			cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSet);
			cmdBindVertexBuffer(cmd, 1, &pPlaneVertexBuffer, &stride, NULL);
			cmdDraw(cmd, 6, 0);
			cmdEndDebugMarker(cmd);
		}

		//// draw the skeleton of the rigs
		cmdBeginDebugMarker(cmd, 1, 0, 1, "Draw Skeletons");
		gSkeletonBatcher.Draw(cmd, gFrameIndex);
		cmdEndDebugMarker(cmd);

		//// draw the UI
		cmdBeginDebugMarker(cmd, 0, 1, 0, "Draw UI");
		loadActions = {};
		loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
		cmdBindRenderTargets(cmd, 1, &pRenderTarget, NULL, &loadActions, NULL, NULL, -1, -1);

		gVirtualJoystick.Draw(cmd, { 1.0f, 1.0f, 1.0f, 1.0f });

		gAppUI.Gui(pStandaloneControlsGUIWindow);    // adds the gui element to AppUI::ComponentsToUpdate list
        float2 txtSize = cmdDrawCpuProfile(cmd, float2(8.0f, 15.0f), &gFrameTimeDraw);
		gAppUI.DrawText(
			cmd, float2(8.f, txtSize.y + 30.f), eastl::string().sprintf("Animation Update %f ms", gAnimationUpdateTimer.GetUSecAverage() / 1000.0f).c_str(),
			&gFrameTimeDraw);

#if !defined(__ANDROID__)
        cmdDrawGpuProfile(cmd, float2(8.f, txtSize.y * 2.f + 45.f), gGpuProfileToken, &gFrameTimeDraw);
#endif

		cmdDrawProfilerUI();

		gAppUI.Draw(cmd);

		cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
		cmdEndDebugMarker(cmd);

		// PRESENT THE GRPAHICS QUEUE
		//
		barriers[0] = { pRenderTarget, RESOURCE_STATE_RENDER_TARGET, RESOURCE_STATE_PRESENT };
		cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);
		cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
		endCmd(cmd);

		QueueSubmitDesc submitDesc = {};
		submitDesc.mCmdCount = 1;
		submitDesc.mSignalSemaphoreCount = 1;
		submitDesc.mWaitSemaphoreCount = 1;
		submitDesc.ppCmds = &cmd;
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

	const char* GetName() { return "27_MultiThread"; }

	bool addSwapChain()
	{
		SwapChainDesc swapChainDesc = {};
		swapChainDesc.mWindowHandle = pWindow->handle;
		swapChainDesc.mPresentQueueCount = 1;
		swapChainDesc.ppPresentQueues = &pGraphicsQueue;
		swapChainDesc.mWidth = mSettings.mWidth;
		swapChainDesc.mHeight = mSettings.mHeight;
		swapChainDesc.mImageCount = gImageCount;
		swapChainDesc.mColorFormat = getRecommendedSwapchainFormat(true);
		swapChainDesc.mColorClearValue = { { 0.39f, 0.41f, 0.37f, 1.0f } };
		swapChainDesc.mEnableVsync = mSettings.mDefaultVSyncEnabled;
		::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

		return pSwapChain != NULL;
	}

	bool addDepthBuffer()
	{
		// Add depth buffer
		RenderTargetDesc depthRT = {};
		depthRT.mArraySize = 1;
        depthRT.mClearValue.depth = 1.0f;
        depthRT.mClearValue.stencil = 0;
		depthRT.mDepth = 1;
		depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
		depthRT.mStartState = RESOURCE_STATE_DEPTH_WRITE;
		depthRT.mHeight = mSettings.mHeight;
		depthRT.mSampleCount = SAMPLE_COUNT_1;
		depthRT.mSampleQuality = 0;
		depthRT.mWidth = mSettings.mWidth;
		depthRT.mFlags = TEXTURE_CREATION_FLAG_ON_TILE;
		addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);

		return pDepthBuffer != NULL;
	}

	static void SkeletonBatchUniformsThreaded(void* pData, uintptr_t i)
	{
		ThreadSkeletonData* data = ((ThreadSkeletonData*)pData) + i;
		gSkeletonBatcher.SetPerInstanceUniforms(data->mFrameNumber, data->mNumberRigs, data->mOffset);
	}

	// Threaded animated object update call
	static void AnimatedObjectThreadedUpdate(void* pData, uintptr_t i)
	{
		// Unpack data
		ThreadData*     data = ((ThreadData*)pData)+i;
		AnimatedObject* animSystem = data->mAnimatedObject;
		float           deltaTime = data->mDeltaTime;
		unsigned int    numberSystems = data->mNumberSystems;

		// Update the systems
		for (unsigned int i = 0; i < numberSystems; ++i)
		{
			if (!(animSystem[i].Update(deltaTime)))
				LOGF(eERROR, "Animation NOT Updating!");

			animSystem[i].PoseRig();
		}
	}
};

DEFINE_APPLICATION_MAIN(MultiThread)
