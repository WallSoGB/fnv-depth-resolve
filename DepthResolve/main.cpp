#include "Bethesda/BSMemory.hpp"
#include "Bethesda/BSRenderedTexture.hpp"
#include "Bethesda/BSRenderState.hpp"
#include "Bethesda/BSShaderAccumulator.hpp"
#include "Bethesda/BSShaderManager.hpp"
#include "Bethesda/ImageSpaceEffect.hpp"
#include "Bethesda/ImageSpaceEffectDepthOfField.hpp"
#include "Bethesda/ImageSpaceManager.hpp"
#include "Bethesda/ImageSpaceTexture.hpp"
#include "Bethesda/TESMain.hpp"
#include "Gamebryo/NiDX9Renderer.hpp"
#include "Misc/BSD3DTexture.hpp"

#include "nvse/PluginAPI.h"
#include "nvapi/nvapi.h"

#include <vector>
#include <algorithm>

BS_ALLOCATORS

IDebugLog gLog("logs\\DepthResolve.log");

// Constants.
static NVSEMessagingInterface*	pMsgInterface = nullptr;
static uint32_t					uiPluginHandle = 0;
static constexpr uint32_t		uiShaderLoaderVersion = 131;

// Statics.
static bool bRESZ;
static bool bNVAPI;

static std::vector<ImageSpaceEffect*> kPostDepthEffects;

class BSShaderManagerEx {
public:
	static NiTexturePtr spINTZDepthTexture;

	static NiTexture* GetINTZDepthTexture(uint32_t auiWidth, uint32_t auiHeight) {
		if (!spINTZDepthTexture) [[unlikely]] {
			IDirect3DDevice9* pDevice = NiDX9Renderer::GetSingleton()->GetD3DDevice();
			IDirect3DTexture9* pD3DTexture = nullptr;
			pDevice->CreateTexture(auiWidth, auiHeight, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &pD3DTexture, nullptr);

			spINTZDepthTexture = BSD3DTexture::CreateObject(pD3DTexture);

			_MESSAGE("INTZ texture created (%d, %d)", auiWidth, auiHeight);

			if (bNVAPI) {
				_MESSAGE("Registering INTZ to NVAPI %p", pD3DTexture);
				NvAPI_D3D9_RegisterResource(pD3DTexture);
			}
		}

		return spINTZDepthTexture;
	}
};

class ImageSpaceManagerEx {
public:
	static NiTexture* GetDepthTexture() {
		return BSShaderManagerEx::GetINTZDepthTexture(0, 0);
	}
};

CallDetour kSortAlphaDetours[2];
CallDetour kRenderGeometryGroupDetour;
class BSShaderAccumulatorEx {
private:
	static IDirect3DSurface9* pLastRTDepth;
public:
	static void __fastcall ResolveDepth(BSRenderedTexture* apCurrentRenderTarget) {
		IDirect3DDevice9* pDevice = NiDX9Renderer::GetSingleton()->GetD3DDevice();
		if (bNVAPI) {
			NiRenderTargetGroup* pRTGroup;
			if (apCurrentRenderTarget)
				pRTGroup = apCurrentRenderTarget->GetRenderTargetGroup();
			else
				pRTGroup = NiDX9Renderer::GetSingleton()->GetCurrentRenderTargetGroup();

			if (pRTGroup) [[likely]] {
				IDirect3DSurface9* pRTDepth = pRTGroup->GetDepthStencilBuffer()->GetDX9RendererData()->m_pkD3DSurface;

				if (pLastRTDepth && pRTDepth != pLastRTDepth) {
					NvAPI_D3D9_UnregisterResource(pLastRTDepth);
				}
				pLastRTDepth = pRTDepth;

				IDirect3DBaseTexture9* pDepthBuffer = BSShaderManagerEx::GetINTZDepthTexture(0, 0)->GetDX9RendererData()->GetD3DTexture();
				if (NvAPI_D3D9_StretchRectEx(pDevice, pRTDepth, NULL, pDepthBuffer, NULL, D3DTEXF_NONE) == NVAPI_UNREGISTERED_RESOURCE) {
					NvAPI_D3D9_RegisterResource(pRTDepth);
					NvAPI_D3D9_RegisterResource(pDepthBuffer);
					NvAPI_D3D9_StretchRectEx(pDevice, pRTDepth, NULL, pDepthBuffer, NULL, D3DTEXF_NONE);
				}
			}
		}
		else if (bRESZ) {
			pDevice->SetTexture(0, BSShaderManagerEx::GetINTZDepthTexture(0, 0)->GetDX9RendererData()->GetD3DTexture());

			BSRenderState::SetZEnable(D3DZB_FALSE, BSRSL_NONE);
			BSRenderState::SetZWriteEnable(false, BSRSL_NONE);
			BSRenderState::SetColorWriteEnable(0, BSRSL_NONE);

			DWORD uiOrigPointSize;
			pDevice->GetRenderState(D3DRS_POINTSIZE, &uiOrigPointSize);

			D3DXVECTOR3 vDummyPoint(0.0f, 0.0f, 0.0f);
			pDevice->DrawPrimitiveUP(D3DPT_POINTLIST, 1, vDummyPoint, sizeof(D3DXVECTOR3));

			BSRenderState::RestoreColorWriteEnable(BSRSL_NONE);
			BSRenderState::RestoreZWriteEnable(BSRSL_NONE);
			BSRenderState::RestoreZEnable(BSRSL_NONE);

			pDevice->SetRenderState(D3DRS_POINTSIZE, 0x7FA05000);
			pDevice->SetRenderState(D3DRS_POINTSIZE, uiOrigPointSize);
		}
	}

	static void __fastcall RenderImageSpaceEffects(BSShaderAccumulator* apAccumulator, BSRenderedTexture* apTexture) {
		if (!kPostDepthEffects.empty()) {
			NiDX9Renderer* pRenderer = NiDX9Renderer::GetSingleton();
			BSRenderedTexture::StopOffscreen();
			for (const auto& pEffect : kPostDepthEffects) {
				if (!pEffect)
					continue;
				if (!pEffect->IsActive())
					continue;
				ImageSpaceManager::GetSingleton()->RenderEffect(pEffect, pRenderer, apTexture, apTexture, nullptr, true);
			}

			BSRenderedTexture::StartOffscreen(NiRenderer::CLEAR_NONE, apTexture->GetRenderTargetGroup());
			pRenderer->SetCameraData(apAccumulator->m_pkCamera);
		}
	}

	void SetupDepth(NiCamera* apWorldCamera, NiCamera* ap1stPersonCamera, BSRenderedTexture* apRenderedTexture) {
		ResolveDepth(apRenderedTexture);
	}

	// Pre-water alpha
	template<uint32_t auiIndex>
	void SortAlphaPasses() {
		ResolveDepth(nullptr);

		if constexpr (auiIndex == 0) {
			BSShaderAccumulator* pThis = reinterpret_cast<BSShaderAccumulator*>(this);
			BSRenderedTexture* pISTexture = BSShaderManager::GetCurrentRenderTarget();
			// Pre-water depth.
			if (pThis->bWorldGeometry && pISTexture)
				RenderImageSpaceEffects(pThis, pISTexture);
		}

		ThisCall(kSortAlphaDetours[auiIndex].GetOverwrittenAddr(), this);
	}

	// Post-water alpha
	void RenderGeometryGroup(BSBatchRenderer::GroupType auiGeometryGroup, bool abAlphaPass) {
		ResolveDepth(nullptr);

		ThisCall(kRenderGeometryGroupDetour.GetOverwrittenAddr(), this, auiGeometryGroup, abAlphaPass);
	}
};

IDirect3DSurface9* BSShaderAccumulatorEx::pLastRTDepth = nullptr;

class TESMainEx {
public:
	void RenderDepthOfField(BSShaderAccumulator* apAccumulator, BSRenderedTexture* apRenderedTexture) {
		// Depth is set up already, clear the render targets though to make the ISE render correctly.
		BSRenderedTexture::StopOffscreen();
		return;
	}
};

class ImageSpaceEffectDepthOfFieldEx : public ImageSpaceEffectDepthOfField {
public:
	bool UpdateParamsEx(int a2) {
		bool bResult = ThisCall<bool>(0xBD66C0, this, a2);

		SceneGraph* pSceneGraph = TESMain::GetWorldSceneGraph();
		NiCamera* pSceneGraphCamera = pSceneGraph->GetCamera();

		ImageSpaceShaderParam* pParameters = kShaderParams.GetAt(2);

		float fNear = pSceneGraphCamera->m_kViewFrustum.m_fNear;
		float fFmN = pSceneGraphCamera->m_kViewFrustum.m_fFar - pSceneGraphCamera->m_kViewFrustum.m_fNear;
		float fNtF = pSceneGraphCamera->m_kViewFrustum.m_fNear * pSceneGraphCamera->m_kViewFrustum.m_fFar;

		pParameters->SetPixelConstants(2, -100000000.0, fNear, fFmN, fNtF);

		return bResult;
	}

	void ReturnTexturesEx() {
		if (kTextures.GetAt(4))
			kTextures.GetAt(4)->ClearTexture();

		ThisCall(0xBD6A30, this);
	}
};

bool CheckDXVK() {
	HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
	if (!d3d9Module) return false;

	char modulePath[MAX_PATH];
	if (!GetModuleFileNameA(d3d9Module, modulePath, MAX_PATH)) {
		return false;
	}

	DWORD versionSize = GetFileVersionInfoSizeA(modulePath, nullptr);
	if (versionSize == 0) return false;

	std::vector<BYTE> versionData(versionSize);
	if (!GetFileVersionInfoA(modulePath, 0, versionSize, versionData.data())) {
		return false;
	}

	struct LANGANDCODEPAGE {
		WORD wLanguage;
		WORD wCodePage;
	} *lpTranslate;

	UINT cbTranslate;
	if (VerQueryValueA(versionData.data(), "\\VarFileInfo\\Translation",
		(LPVOID*)&lpTranslate, &cbTranslate)) {

		char subBlock[256];
		sprintf_s(subBlock, "\\StringFileInfo\\%04x%04x\\ProductName", lpTranslate[0].wLanguage, lpTranslate[0].wCodePage);

		char* fileDesc;
		UINT fileDescLen;
		if (VerQueryValueA(versionData.data(), subBlock, (LPVOID*)&fileDesc, &fileDescLen)) {
			if (!memcmp(fileDesc, "DXVK", 4)) {
				return true;
			}
		}
	}

	return false;
}

bool INTZTextureResetCallback(bool abBeforeReset, void* pvData) {
	if (abBeforeReset) {
		_MESSAGE("Releasing INTZ texture before reset");

		if (bNVAPI && BSShaderManagerEx::spINTZDepthTexture) {
			IDirect3DBaseTexture9* pD3DTexture = BSShaderManagerEx::spINTZDepthTexture->GetDX9RendererData()->GetD3DTexture();
			_MESSAGE("Unregistering INTZ from NVAPI %p", pD3DTexture);
			NvAPI_D3D9_UnregisterResource(pD3DTexture);
		}

		BSShaderManagerEx::spINTZDepthTexture = nullptr;
	}
	else {
		NiDX9Renderer* pRenderer = NiDX9Renderer::GetSingleton();
		if (!pRenderer || !pRenderer->GetD3DDevice()) {
			_MESSAGE("Device not available during reset callback");
			return false;
		}

		uint32_t uiWidth, uiHeight;
		if (BSShaderManager::bLetterBox) {
			uiWidth = BSShaderManager::iLetterboxWidth;
			uiHeight = BSShaderManager::iLetterboxHeight;
		}
		else {
			uiWidth = pRenderer->GetScreenWidth();
			uiHeight = pRenderer->GetScreenHeight();
		}
		BSShaderManagerEx::GetINTZDepthTexture(uiWidth, uiHeight);
	}

	return true;
}

NiTexturePtr BSShaderManagerEx::spINTZDepthTexture = nullptr;

void InitHooks() {
	// To allow manual resolve if someone needs one
	WriteRelJumpEx(0xB65550, &BSShaderAccumulatorEx::SetupDepth);
	
	WriteRelJumpEx(0x875E40, &TESMainEx::RenderDepthOfField);
	WriteRelJump(0xB54090, &ImageSpaceManagerEx::GetDepthTexture);

	// BSShaderAccumulator::RegisterObject_Standard
	// Skip accumulating geometry to depth groups, as we don't render them anymore
	SafeWrite8(0xB64057, 0xEB);

	// World
	kSortAlphaDetours[0].ReplaceCallEx(0xB65C32, &BSShaderAccumulatorEx::SortAlphaPasses<0>); // Pre-Water
	kRenderGeometryGroupDetour.ReplaceCallEx(0xB65D62, &BSShaderAccumulatorEx::RenderGeometryGroup); // Post-Water

	// First person
	kSortAlphaDetours[1].ReplaceCallEx(0xB65E1F, &BSShaderAccumulatorEx::SortAlphaPasses<1>);

	// TESMain::DrawWorldStandard
	// Skip the not working motion blur while aiming rendering.
	SafeWrite16(0x870EB3, 0x33EB);  // JMP 0x870EE8

	ReplaceVirtualFuncEx(0x10BC42C, &ImageSpaceEffectDepthOfFieldEx::UpdateParamsEx);
	ReplaceVirtualFuncEx(0x10BC424, &ImageSpaceEffectDepthOfFieldEx::ReturnTexturesEx);
}

void MessageHandler(NVSEMessagingInterface::Message* msg) {
	switch (msg->type) {
	case NVSEMessagingInterface::kMessage_DeferredInit:
	{
		bool bDXVK = CheckDXVK();

		_MESSAGE("DXVK status: %u", bDXVK);

		NiDX9Renderer* pRenderer = NiDX9Renderer::GetSingleton();
		IDirect3D9* pD3D9 = pRenderer->GetD3D9();
		D3DDISPLAYMODE kDisplayMode;
		pD3D9->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &kDisplayMode);
		bRESZ = pD3D9->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, kDisplayMode.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('R', 'E', 'S', 'Z')) == D3D_OK;

		_MESSAGE("RESZ status: %u", bRESZ);

		if (!bRESZ && bDXVK) {
			MessageBox(NULL, "Incompatible DXVK version.\nYour version of DXVK is incompatible with Depth Resolve. Use version <= 2.6.1, or >= 2.7.1.", "Depth Resolve", MB_OK | MB_ICONERROR);
			ExitProcess(0);
		}

		bNVAPI = !bRESZ && NvAPI_Initialize() == NVAPI_OK;

		_MESSAGE("NVAPI status: %u", bNVAPI);

		if (!bRESZ && !bNVAPI) {
			MessageBox(NULL, "Depth Resolve not compatible with your device, because it does not support RESZ nor Nvidia API.", "Depth Resolve", MB_OK | MB_ICONERROR);
			ExitProcess(0);
		}

		uint32_t uiWidth, uiHeight;
		if (BSShaderManager::bLetterBox) {
			uiWidth = BSShaderManager::iLetterboxWidth;
			uiHeight = BSShaderManager::iLetterboxHeight;
		}
		else {
			uiWidth = pRenderer->GetScreenWidth();
			uiHeight = pRenderer->GetScreenHeight();
		}
		BSShaderManagerEx::GetINTZDepthTexture(uiWidth, uiHeight);

		pRenderer->AddResetNotificationFunc(INTZTextureResetCallback, nullptr);
	}
		break;
	}
}

EXTERN_DLL_EXPORT void __cdecl PrependPostDepthEffect(ImageSpaceEffect* apEffect) {
	if (std::find(kPostDepthEffects.begin(), kPostDepthEffects.end(), apEffect) != kPostDepthEffects.end())
		return;

	kPostDepthEffects.insert(kPostDepthEffects.begin(), apEffect);
}

EXTERN_DLL_EXPORT void __cdecl AppendPostDepthEffect(ImageSpaceEffect* apEffect) {
	if (std::find(kPostDepthEffects.begin(), kPostDepthEffects.end(), apEffect) != kPostDepthEffects.end())
		return;

	kPostDepthEffects.push_back(apEffect);
}

EXTERN_DLL_EXPORT bool NVSEPlugin_Query(const NVSEInterface* nvse, PluginInfo* info) {
	info->infoVersion = PluginInfo::kInfoVersion;
	info->name = "Depth Resolve";
	info->version = 131;

	return !nvse->isEditor;
}

EXTERN_DLL_EXPORT bool NVSEPlugin_Load(NVSEInterface* nvse) {
	HMODULE hShaderLoader = GetModuleHandle("Fallout Shader Loader.dll");
	HMODULE hLODFlickerFix = GetModuleHandle("LODFlickerFix.dll");
	HMODULE hDOFFix = GetModuleHandle("DoF-Fix.dll");

	if (!hShaderLoader) {
		MessageBox(NULL, "Fallout Shader Loader not found.\nDepth Resolve cannot be used without it, please install it.", "Depth Resolve", MB_OK | MB_ICONERROR);
		ExitProcess(0);
	}

	if (!hLODFlickerFix) {
		MessageBox(NULL, "LOD Flicker Fix not found.\nDepth Resolve cannot be used without it, please install it.", "Depth Resolve", MB_OK | MB_ICONERROR);
		ExitProcess(0);
	}

	if (!hDOFFix) {
		_MESSAGE("Depth of Field Fix not found");
	}

	pMsgInterface = (NVSEMessagingInterface*)nvse->QueryInterface(kInterface_Messaging);
	uiPluginHandle = nvse->GetPluginHandle();
	pMsgInterface->RegisterListener(uiPluginHandle, "NVSE", MessageHandler);

	auto pQuery = (_NVSEPlugin_Query)GetProcAddress(hShaderLoader, "NVSEPlugin_Query");
	PluginInfo kInfo = {};
	pQuery(nvse, &kInfo);
	if (kInfo.version < uiShaderLoaderVersion) {
		char cBuffer[192];
		sprintf_s(cBuffer, "Fallout Shader Loader is outdated.\nPlease update it to use Depth Resolve!\nCurrent version: %i\nMinimum required version: %i", kInfo.version, uiShaderLoaderVersion);
		MessageBox(NULL, cBuffer, "Depth Resolve", MB_OK | MB_ICONERROR);
		ExitProcess(0);
	}

	InitHooks();

	return true;
}

BOOL WINAPI DllMain(
	HANDLE  hDllHandle,
	DWORD   dwReason,
	LPVOID  lpreserved
)
{
	return TRUE;
}