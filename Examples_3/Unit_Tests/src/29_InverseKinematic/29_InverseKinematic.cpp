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
* The Forge - ANIMATION - AIM IK UNIT TEST
*
* The purpose of this demo is to show how to do aim inverse kinematics using the
* animnation middleware
*
*********************************************************************************************************/

// Interfaces
#include "../../../../Common_3/OS/Interfaces/ICameraController.h"
#include "../../../../Common_3/OS/Interfaces/IApp.h"
#include "../../../../Common_3/OS/Interfaces/ILog.h"
#include "../../../../Common_3/OS/Interfaces/IFileSystem.h"
#include "../../../../Common_3/OS/Interfaces/ITime.h"
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

Shader*           pPlaneDrawShader = NULL;
Buffer*           pPlaneVertexBuffer = NULL;
Pipeline*         pPlaneDrawPipeline = NULL;
RootSignature*    pRootSignature = NULL;
DescriptorSet*    pDescriptorSetPlane = NULL;
DescriptorSet*    pDescriptorSetTarget = NULL;

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

// AnimatedObjects
AnimatedObject gStickFigureAnimObject;

// Animations
Animation gStandAnimation;

// ClipControllers
ClipController gStandClipController;

// Clips
Clip gStandClip;

// Rigs
Rig gStickFigureRig;

// SkeletonBatcher
SkeletonBatcher gSkeletonBatcher;

// parameters for aim IK
AimIKDesc     gAimIKDesc;
TwoBonesIKDesc gTwoBonesIKDesc;
int           gJointChain[4];
const Vector3 gJointUpVectors[4] = { Vector3::xAxis(), Vector3::xAxis(), Vector3::xAxis(), Vector3::xAxis() };
//--------------------------------------------------------------------------------------------
// TARGET
//--------------------------------------------------------------------------------------------

UniformSkeletonBlock gUniformDataTarget;

Buffer* pTargetUniformBuffer[gImageCount] = { NULL };

// Filenames
const char* gStickFigureName = "stickFigure/skeleton.ozz";
const char* gStandClipName = "stickFigure/animations/stand.ozz";

float* pJointPoints = 0;
float* pBonePoints = 0;

const int   gSphereResolution = 30;                   // Increase for higher resolution joint spheres
const float gBoneWidthRatio = 0.2f;                   // Determines how far along the bone to put the max width [0,1]
const float gJointRadius = gBoneWidthRatio * 0.5f;    // set to replicate Ozz skeleton

// Timer to get animationsystem update time
static HiresTimer gAnimationUpdateTimer;

//--------------------------------------------------------------------------------------------
// UI DATA
//--------------------------------------------------------------------------------------------
struct UIData
{
	struct IKParamsData
	{
		bool mAim = true;
		float mFoot;
	};
	IKParamsData mIKParams;

	struct BlendParamsData
	{
		bool*  mAutoSetBlendParams;
		float* mStandClipWeight;
		float* mThreshold;
	};
	BlendParamsData mBlendParams;

	struct ClipData
	{
		bool*  mPlay;
		bool*  mLoop;
		float  mAnimationTime;    // will get set by clip controller
		float* mPlaybackSpeed;
	};
	ClipData mStandClip;

	struct GeneralSettingsData
	{
		bool mShowBindPose = false;    //false;
		bool mDrawPlane = true;
	};
	GeneralSettingsData mGeneralSettings;
};
UIData gUIData;

// Hard set the controller's time ratio via callback when it is set in the UI
void StandClipTimeChangeCallback() { gStandClipController.SetTimeRatioHard(gUIData.mStandClip.mAnimationTime); }

bool gTestGraphicsReset = false;
void testGraphicsReset()
{
	gTestGraphicsReset = !gTestGraphicsReset;
}

//--------------------------------------------------------------------------------------------
// APP CODE
//--------------------------------------------------------------------------------------------
class AimIK: public IApp
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

		// GENERATE VERTEX BUFFERS
		//

		// Generate joint vertex buffer
		generateSpherePoints(&pJointPoints, &gNumberOfJointPoints, gSphereResolution, gJointRadius);

		// Generate bone vertex buffer
		generateBonePoints(&pBonePoints, &gNumberOfBonePoints, gBoneWidthRatio);

		
		// RIGS
		//
		// Initialize the rig with the path to its ozz file
		gStickFigureRig.Initialize(RD_ANIMATIONS, gStickFigureName);

		// Add the rig to the list of skeletons to render
		gSkeletonBatcher.AddRig(&gStickFigureRig);

		// CLIPS
		//
		gStandClip.Initialize(RD_ANIMATIONS, gStandClipName, &gStickFigureRig);

		// CLIP CONTROLLERS
		//
		// Initialize with the length of the clip they are controlling and an
		// optional external time to set based on their updating
		gStandClipController.Initialize(gStandClip.GetDuration(), &gUIData.mStandClip.mAnimationTime);

		// ANIMATIONS
		//
		AnimationDesc animationDesc{};
		animationDesc.mRig = &gStickFigureRig;
		animationDesc.mNumLayers = 1;
		animationDesc.mLayerProperties[0].mClip = &gStandClip;
		animationDesc.mLayerProperties[0].mClipController = &gStandClipController;

		gStandAnimation.Initialize(animationDesc);

		// ANIMATED OBJECTS
		//
		gStickFigureAnimObject.Initialize(&gStickFigureRig, &gStandAnimation);

		const char* aimJointNames[4] = { "Head", "Spine3", "Spine2", "Spine1" };
		gAimIKDesc.mForward = Vector3::yAxis();
		gAimIKDesc.mOffset = Vector3(.07f, .1f, 0.f);
		gAimIKDesc.mPoleVector = Vector3::yAxis();
		gAimIKDesc.mTwistAngle = 0.0f;
		gAimIKDesc.mJointWeight = 0.5f;
		gAimIKDesc.mJointChainLength = 4;
		gAimIKDesc.mJointChain = gJointChain;
		gAimIKDesc.mJointUpVectors = gJointUpVectors;
		gStickFigureRig.FindJointChain(aimJointNames, gAimIKDesc.mJointChainLength, gJointChain);

		const char* twoBonesJointNames[] = { "RightUpLeg", "RightLeg", "RightFoot" };
		gTwoBonesIKDesc.mSoften = 1.0f;
		gTwoBonesIKDesc.mWeight = 1.0f;
		gTwoBonesIKDesc.mTwistAngle = 0.0f;
		gTwoBonesIKDesc.mPoleVector = Vector3::zAxis();
		gTwoBonesIKDesc.mMidAxis = Vector3::zAxis();
		gStickFigureRig.FindJointChain(twoBonesJointNames, 3, gTwoBonesIKDesc.mJointChain);

		/************************************************************************/
		// SETUP THE MAIN CAMERA
		//
		CameraMotionParameters cmp{ 50.0f, 75.0f, 150.0f };
		vec3                   camPos{ -3.0f, 3.0f, 3.0f };
		vec3                   lookAt{ 0.0f, 1.0f, 0.0f };

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
		gStickFigureRig.Destroy();
		gStandClip.Destroy();
		gStandAnimation.Destroy();
		gStickFigureAnimObject.Destroy();

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
			addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetPlane);
			addDescriptorSet(pRenderer, &setDesc, &pDescriptorSetTarget);

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
			ubDesc.mDesc.mSize = sizeof(UniformSkeletonBlock);
			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				ubDesc.ppBuffer = &pTargetUniformBuffer[i];
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
			pStandaloneControlsGUIWindow = gAppUI.AddGuiComponent("Stand Animation", &guiDesc);

			// SET gUIData MEMBERS THAT NEED POINTERS TO ANIMATION DATA
			//

			// Blend Params
			gUIData.mBlendParams.mAutoSetBlendParams = gStandAnimation.GetAutoSetBlendParamsPtr();

			gUIData.mBlendParams.mStandClipWeight = gStandClipController.GetWeightPtr();
			gUIData.mBlendParams.mThreshold = gStandAnimation.GetThresholdPtr();

			// Stand Clip
			gUIData.mStandClip.mPlay = gStandClipController.GetPlayPtr();
			gUIData.mStandClip.mLoop = gStandClipController.GetLoopPtr();
			gUIData.mStandClip.mPlaybackSpeed = gStandClipController.GetPlaybackSpeedPtr();

			// SET UP GUI BASED ON gUIData STRUCT
			//
			{
				// INVERSE KINEMATICS
				//
				CollapsingHeaderWidget CollapsingIKWidgets("Inverse Kinematics");
				CollapsingIKWidgets.SetCollapsed(false);
				CollapsingIKWidgets.AddSubWidget(CheckboxWidget("Aim IK", &gUIData.mIKParams.mAim));
				float             sliderStepSize = 0.01f;
				SliderFloatWidget SliderFootIK("Foot two bone IK", &gUIData.mIKParams.mFoot, 0.0f, 0.5f, sliderStepSize);
				CollapsingIKWidgets.AddSubWidget(SliderFootIK);

				// BLEND PARAMETERS
				//
				CollapsingHeaderWidget CollapsingBlendParamsWidgets("Blend Parameters");

				// AutoSetBlendParams - Checkbox
				CollapsingBlendParamsWidgets.AddSubWidget(SeparatorWidget());
				CollapsingBlendParamsWidgets.AddSubWidget(CheckboxWidget("Auto Set Blend Params", gUIData.mBlendParams.mAutoSetBlendParams));

				// Stand Clip Weight - Slider
				float fValMin = 0.0f;
				float fValMax = 1.0f;

				CollapsingBlendParamsWidgets.AddSubWidget(SeparatorWidget());
				CollapsingBlendParamsWidgets.AddSubWidget(
					SliderFloatWidget("Clip Weight [Stand]", gUIData.mBlendParams.mStandClipWeight, fValMin, fValMax, sliderStepSize));

				// Threshold - Slider
				fValMin = 0.01f;
				fValMax = 1.0f;
				sliderStepSize = 0.01f;

				CollapsingBlendParamsWidgets.AddSubWidget(SeparatorWidget());
				CollapsingBlendParamsWidgets.AddSubWidget(
					SliderFloatWidget("Threshold", gUIData.mBlendParams.mThreshold, fValMin, fValMax, sliderStepSize));
				CollapsingBlendParamsWidgets.AddSubWidget(SeparatorWidget());

				// STAND CLIP
				//
				CollapsingHeaderWidget CollapsingStandClipWidgets("Stand Clip");

				// Play/Pause - Checkbox
				CollapsingStandClipWidgets.AddSubWidget(SeparatorWidget());
				CollapsingStandClipWidgets.AddSubWidget(CheckboxWidget("Play", gUIData.mStandClip.mPlay));

				// Loop - Checkbox
				CollapsingStandClipWidgets.AddSubWidget(SeparatorWidget());
				CollapsingStandClipWidgets.AddSubWidget(CheckboxWidget("Loop", gUIData.mStandClip.mLoop));

				// Animation Time - Slider
				fValMin = 0.0f;
				fValMax = gStandClipController.GetDuration();
				sliderStepSize = 0.01f;
				SliderFloatWidget SliderAnimationTime("Animation Time", &gUIData.mStandClip.mAnimationTime, fValMin, fValMax, sliderStepSize);
				SliderAnimationTime.pOnActive = StandClipTimeChangeCallback;

				CollapsingStandClipWidgets.AddSubWidget(SeparatorWidget());
				CollapsingStandClipWidgets.AddSubWidget(SliderAnimationTime);

				// Playback Speed - Slider
				fValMin = -5.0f;
				fValMax = 5.0f;
				sliderStepSize = 0.1f;

				CollapsingStandClipWidgets.AddSubWidget(SeparatorWidget());
				CollapsingStandClipWidgets.AddSubWidget(
					SliderFloatWidget("Playback Speed", gUIData.mStandClip.mPlaybackSpeed, fValMin, fValMax, sliderStepSize));
				CollapsingStandClipWidgets.AddSubWidget(SeparatorWidget());

				// GENERAL SETTINGS
				//
				CollapsingHeaderWidget CollapsingGeneralSettingsWidgets("General Settings");

				// ShowBindPose - Checkbox
				CollapsingGeneralSettingsWidgets.AddSubWidget(SeparatorWidget());
				CollapsingGeneralSettingsWidgets.AddSubWidget(CheckboxWidget("Show Bind Pose", &gUIData.mGeneralSettings.mShowBindPose));

				// DrawPlane - Checkbox
				CollapsingGeneralSettingsWidgets.AddSubWidget(SeparatorWidget());
				CollapsingGeneralSettingsWidgets.AddSubWidget(CheckboxWidget("Draw Plane", &gUIData.mGeneralSettings.mDrawPlane));
				CollapsingGeneralSettingsWidgets.AddSubWidget(SeparatorWidget());

				// Add all widgets to the window
				
				// Reset graphics with a button.
				ButtonWidget testGPUReset("ResetGraphicsDevice");
				testGPUReset.pOnEdited = testGraphicsReset;
				pStandaloneControlsGUIWindow->AddWidget(testGPUReset);

				pStandaloneControlsGUIWindow->AddWidget(CollapsingIKWidgets);
				pStandaloneControlsGUIWindow->AddWidget(CollapsingBlendParamsWidgets);
				pStandaloneControlsGUIWindow->AddWidget(CollapsingStandClipWidgets);
				pStandaloneControlsGUIWindow->AddWidget(CollapsingGeneralSettingsWidgets);
			}

			waitForAllResourceLoads();

			// Prepare descriptor sets
			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				DescriptorData params[1] = {};
				params[0].pName = "uniformBlock";
				params[0].ppBuffers = &pPlaneUniformBuffer[i];
				updateDescriptorSet(pRenderer, i, pDescriptorSetPlane, 1, params);
				params[0].ppBuffers = &pTargetUniformBuffer[gFrameIndex];
				updateDescriptorSet(pRenderer, i, pDescriptorSetTarget, 1, params);
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

		if (mSettings.mResetGraphics || mSettings.mQuit) 
		{
			exitProfilerUI();
			exitProfiler();

			gSkeletonBatcher.Destroy();
			gVirtualJoystick.Exit();

			gAppUI.Exit();

			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				removeResource(pPlaneUniformBuffer[i]);
			}
			for (uint32_t i = 0; i < gImageCount; ++i)
			{
				removeResource(pTargetUniformBuffer[i]);
			}

			removeResource(pJointVertexBuffer);
			removeResource(pBoneVertexBuffer);
			removeResource(pPlaneVertexBuffer);

			removeShader(pRenderer, pSkeletonShader);
			removeShader(pRenderer, pPlaneDrawShader);
			removeDescriptorSet(pRenderer, pDescriptorSetPlane);
			removeDescriptorSet(pRenderer, pDescriptorSetTarget);
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

		vec3 lightPos = vec3(0.0f, 1000.0f, 0.0f);
		vec3 lightColor = vec3(1.0f, 1.0f, 1.0f);

		/************************************************************************/
		// Animation
		/************************************************************************/
		gAnimationUpdateTimer.Reset();

		// Update the animated object for this frame
		if (!gStickFigureAnimObject.Update(deltaTime))
			LOGF(eINFO, "Animation NOT Updating!");

		static float time = 0.0f;
		time += 2.0f * deltaTime;

		const Point3 aimTarget = Vector3(.2f, 1.5f, -.3f) + Point3(sin(time * .5f), cos(time * .25f), cos(time) * .5f + .5f);

		gUniformDataTarget.mProjectView = projViewMat;
		gUniformDataTarget.mLightPosition = Vector4(lightPos);
		gUniformDataTarget.mLightColor = Vector4(lightColor);
		gUniformDataTarget.mToWorldMat[0] = Matrix4(Matrix3::identity() * 0.25f, Vector3(aimTarget));
		gUniformDataTarget.mColor[0] = Vector4(1.0f, 0.0f, 0.0f, 1.0f);

		if (gUIData.mIKParams.mAim)
		{
			if (!gStickFigureAnimObject.AimIK(&gAimIKDesc, aimTarget))
				LOGF(eINFO, "Aim IK failed!");
		}

		Matrix4 mat = gStickFigureRig.GetJointModelMats()[gTwoBonesIKDesc.mJointChain[2]];
		Point3  twoBoneTarget = Point3(mat.getCol3().get128()) + Vector3(0.0f, gUIData.mIKParams.mFoot, 0.0f);

		if (!gStickFigureAnimObject.TwoBonesIK(&gTwoBonesIKDesc, twoBoneTarget))
			LOGF(eINFO, "Two bone IK failed!");

		if (!gUIData.mGeneralSettings.mShowBindPose)
		{
			// Pose the rig based on the animated object's updated values
			gStickFigureAnimObject.PoseRig();
		}
		else
		{
			// Ignore the updated values and pose in bind
			gStickFigureAnimObject.PoseRigInBind();
		}

		// Record animation update time
		gAnimationUpdateTimer.GetUSec(true);

		// Update uniforms that will be shared between all skeletons
		gSkeletonBatcher.SetSharedUniforms(projViewMat, lightPos, lightColor);

		/************************************************************************/
		// Plane
		/************************************************************************/
		gUniformDataPlane.mProjectView = projViewMat;
		gUniformDataPlane.mToWorldMat = mat4::identity();

		/************************************************************************/
		// GUI
		/************************************************************************/
		gAppUI.Update(deltaTime);
	}

	void Draw()
	{
		uint32_t swapchainImageIndex;
		acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL, &swapchainImageIndex);

		// UPDATE UNIFORM BUFFERS
		//

		// Update all the instanced uniform data for each batch of joints and bones
		gSkeletonBatcher.SetPerInstanceUniforms(gFrameIndex);

		BufferUpdateDesc planeViewProjCbv = { pPlaneUniformBuffer[gFrameIndex] };
		beginUpdateResource(&planeViewProjCbv);
		*(UniformBlockPlane*)planeViewProjCbv.pMappedData = gUniformDataPlane;
		endUpdateResource(&planeViewProjCbv, NULL);
		BufferUpdateDesc targetViewProjCbv = { pTargetUniformBuffer[gFrameIndex] };
		beginUpdateResource(&targetViewProjCbv);
		*(UniformSkeletonBlock*)targetViewProjCbv.pMappedData = gUniformDataTarget;
		endUpdateResource(&targetViewProjCbv, NULL);

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

		const uint32_t stride = sizeof(float) * 6;
		//// draw plane
		if (gUIData.mGeneralSettings.mDrawPlane)
		{
			cmdBeginDebugMarker(cmd, 1, 0, 1, "Draw Plane");
			cmdBindPipeline(cmd, pPlaneDrawPipeline);
			cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetPlane);
			cmdBindVertexBuffer(cmd, 1, &pPlaneVertexBuffer, &stride, NULL);
			cmdDraw(cmd, 6, 0);
			cmdEndDebugMarker(cmd);
		}

		//// draw the skeleton of the rig
		cmdBeginDebugMarker(cmd, 1, 0, 1, "Draw Skeletons");
		gSkeletonBatcher.Draw(cmd, gFrameIndex);
		cmdEndDebugMarker(cmd);

		cmdBindVertexBuffer(cmd, 1, &pJointVertexBuffer, &stride, NULL);
		cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetTarget);
		cmdDrawInstanced(cmd, gNumberOfJointPoints / 6, 0, 1, 0);

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

	const char* GetName() { return "29_InverseKinematic"; }

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
		addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);

		return pDepthBuffer != NULL;
	}
};

DEFINE_APPLICATION_MAIN(AimIK)
