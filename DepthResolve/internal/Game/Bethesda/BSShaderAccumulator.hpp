#pragma once

#include "BSBatchRenderer.hpp"
#include "Gamebryo/NiAlphaAccumulator.hpp"
#include "Gamebryo/NiCriticalSection.hpp"

NiSmartPointer(BSShaderAccumulator)

class BSShaderProperty;
class BSInstanceRendering;
class BSOcclusionQuery;
class ShadowSceneNode;

class BSShaderAccumulator : public NiAlphaAccumulator {
public:

	virtual void FinishAccumulatingPreResolveDepth();
	virtual void FinishAccumulatingPostResolveDepth();
	virtual void RenderPassList(uint32_t, bool); // Oblivion leftover

	enum BATCHRENDERER_CREATION_MODE {
		BRCM_NORMAL			= 99,
		BRCM_NO_GROUPS		= 100,
		BRCM_ALPHA_GROUP	= 101,
	};

	BSShaderAccumulator();
	virtual ~BSShaderAccumulator();

	struct ActorOcclusionTest {
		NiBound				kBound;
		uint32_t			uiRefID;
		BSOcclusionQuery*	pOcclusionQuery;
		bool				bIsVisible;
		bool				bQuerySent;
		bool				bEnabled;
		uint32_t			uiPixelCount;
	};

	struct SunOcclusionTest {
		BSOcclusionQuery*	pOcclusionQuery;
		bool				bWaiting;
		uint8_t				ucSampleCount; // Added by me
		float				fPercentOccluded;
		uint32_t			uiPixelCount;
		uint32_t			uiFrameCount;
	};

	uint32_t								unk034;
	bool									bWorldGeometry;
	bool									unk039;
	bool									bRenderDOF;
	uint32_t								uiSunPixelCount;
	bool									bWaitingForSunQuery;
	float									fPercentSunOccludedStored;
	SunOcclusionTest						kSunTests[3];
	bool									bProcessLights;
	bool									bIs1stPerson;
	bool									unk086;
	uint16_t								unk088;
	uint16_t								usQueryCount;
	BSOcclusionQuery**						ppOcclusionQueries;
	NiTPointerList<NiGeometry*>				kVolumetricFogGeoList;
	uint32_t								pad0[40];
	NiGeometryPtr							spOcclusionTestShape;
	NiTPointerList<ActorOcclusionTest>		kActorOcclusionTestList;
	uint32_t								uiLastRefID;
	bool									bLastCullResult;
	NiColorA								kAmbientColor;
	bool									bWaterReflection;
	bool									bWaterRefraction;
	bool									bUseUnderwaterGroup;
	bool									bIsUnderwater;
	int32_t									iWaterHeight;
	bool									bRenderDecals;
	BSInstanceRendering*					pInstanceRendering;
	BSBatchRenderer*						pBatchRenderers[2];
	BSShaderManager::RenderPassType*		pPasses;
	BSBatchRenderer::TransparencyType*		pTransparencies;
	bool*									pPassStates;
	bool*									bPassFinishStates;
	uint32_t								uiBatchRendererCount;
	uint32_t								uiMaxPassCount;
	ShadowSceneNode*						pActiveShadowSceneNode;
	NiObjectPtr								spObject198;
	BSShaderManager::RenderMode				eRenderMode;
	uint32_t								spShadowLight;
	BSShaderProperty::RenderPass			kTileRenderPass;
	uint32_t								pad[4];
	uint32_t								unk1C4;
	uint32_t								unk1C8;
	uint32_t								unk1CC;
	uint32_t								unk1D0;
	uint32_t								unk1D4;
	uint32_t								unk1D8;
	uint32_t								unk1DC;
	uint32_t								unk1E0;
	uint32_t								unk1E4;
	uint32_t								unk1E8;
	uint32_t								unk1EC;
	uint32_t								unk1F0;
	uint32_t								unk1F4;
	uint32_t								unk1F8;
	uint32_t								unk1FC;
	NiCriticalSection						kCriticalSection;
	uint32_t								unk220;
	uint32_t								unk224;
	uint32_t								unk228;
	uint32_t								unk22C;
	uint32_t								unk230;
	uint32_t								unk234;
	uint32_t								unk238;
	uint32_t								unk23C;
	uint32_t								unk240;
	uint32_t								unk244;
	uint32_t								unk248;
	uint32_t								unk24C;
	uint32_t								unk250;
	uint32_t								unk254;
	uint32_t								unk258;
	uint32_t								unk25C;
	uint32_t								unk260;
	uint32_t								unk264;
	uint32_t								unk268;
	uint32_t								unk26C;
	uint32_t								unk270;
	uint32_t								unk274;
	uint32_t								unk278;
	uint32_t								unk27C;

	NIRTTI_ADDRESS(0x11FA000)

	bool GetWaterPassesWithinRange(BSShaderManager::RenderPassType aeStartRange, BSShaderManager::RenderPassType aeEndRange);

	void RenderBatches(BSShaderManager::RenderPassType aeStart, BSShaderManager::RenderPassType aeEnd, bool abAlphaPass = false, BSBatchRenderer::GroupType aeGeometryGroup = BSBatchRenderer::GROUP_NONE) __restrict;
	void RenderGeometryGroup(BSBatchRenderer::GroupType aeGeometryGroup, bool abAlphaPass);
	void RenderAlphaGeometry(BSBatchRenderer::AlphaGroupType aeGeometryGroup);
};

ASSERT_SIZE(BSShaderAccumulator, 0x280);