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
static constexpr uint32_t		uiShaderLoaderVersion = 131;

// Statics.
bool bRESZ;
bool bNVAPI;

static std::vector<ImageSpaceEffect*> kPostDepthEffects;

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

class BSShaderManagerEx {
public:
	static NiTexturePtr spINTZDepthTexture;

	static NiTexture* __fastcall GetINTZDepthTexture(NiDX9Renderer* apRenderer, uint32_t auiWidth, uint32_t auiHeight) {
		if (!spINTZDepthTexture) [[unlikely]] {
			IDirect3DDevice9* pDevice = apRenderer->GetD3DDevice();
			IDirect3DTexture9* pD3DTexture = nullptr;
			pDevice->CreateTexture(auiWidth, auiHeight, 1, D3DUSAGE_DEPTHSTENCIL, (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), D3DPOOL_DEFAULT, &pD3DTexture, nullptr);

			spINTZDepthTexture = BSD3DTexture::CreateObject(pD3DTexture, apRenderer);

			_MESSAGE("INTZ texture created (%d, %d)", auiWidth, auiHeight);
		}

		return spINTZDepthTexture;
	}

	static bool INTZTextureResetCallback(bool abBeforeReset, void* pvData) {
		if (abBeforeReset) {
			_MESSAGE("Releasing INTZ texture before reset");
			BSShaderManagerEx::spINTZDepthTexture = nullptr;
		}
		else {
			NiDX9Renderer* pRenderer = NiDX9Renderer::GetSingleton();
			if (!pRenderer || !pRenderer->GetD3DDevice()) [[unlikely]] {
				_MESSAGE("Device not available during reset callback");
				return false;
			}

			uint32_t uiWidth, uiHeight;
			if (BSShaderManager::bLetterBox) [[unlikely]] {
				uiWidth = BSShaderManager::iLetterboxWidth;
				uiHeight = BSShaderManager::iLetterboxHeight;
			}
			else [[likely]] {
				uiWidth = pRenderer->GetScreenWidth();
				uiHeight = pRenderer->GetScreenHeight();
			}
			BSShaderManagerEx::GetINTZDepthTexture(pRenderer, uiWidth, uiHeight);
		}

		return true;
	}
};

NiTexturePtr BSShaderManagerEx::spINTZDepthTexture = nullptr;

class ImageSpaceManagerEx {
public:
	static NiTexture* GetDepthTexture() {
		return BSShaderManagerEx::GetINTZDepthTexture(NiDX9Renderer::GetSingleton(), 0, 0);
	}
};

CallDetour kSortAlphaDetours[2];
CallDetour kRenderGeometryGroupDetour;
class BSShaderAccumulatorEx {
public:
	static void __fastcall ResolveDepth(BSRenderedTexture* apCurrentRenderTarget) {
		NiDX9Renderer* pRenderer = NiDX9Renderer::GetSingleton();
		IDirect3DDevice9* pDevice = pRenderer->GetD3DDevice();
		if (bNVAPI) {
			NiRenderTargetGroup* pRTGroup;
			if (apCurrentRenderTarget)
				pRTGroup = apCurrentRenderTarget->GetRenderTargetGroup();
			else
				pRTGroup = pRenderer->GetCurrentRenderTargetGroup();

			if (pRTGroup) [[likely]] {
				NiDepthStencilBuffer* pBuffer = pRTGroup->GetDepthStencilBuffer();
				if (pBuffer) [[likely]] {
					IDirect3DSurface9* pRTDepth = pBuffer->GetDX9RendererData()->m_pkD3DSurface;

					IDirect3DBaseTexture9* pDepthBuffer = BSShaderManagerEx::GetINTZDepthTexture(pRenderer, 0, 0)->GetDX9RendererData()->GetD3DTexture();
					if (NvAPI_D3D9_StretchRectEx(pDevice, pRTDepth, NULL, pDepthBuffer, NULL, D3DTEXF_NONE) == NVAPI_UNREGISTERED_RESOURCE) [[unlikely]] {
						NvAPI_D3D9_RegisterResource(pRTDepth);
						NvAPI_D3D9_RegisterResource(pDepthBuffer);
						NvAPI_D3D9_StretchRectEx(pDevice, pRTDepth, NULL, pDepthBuffer, NULL, D3DTEXF_NONE);
					}
				}
			}
		}
		else if (bRESZ) {
			IDirect3DBaseTexture9* pOrigTexture = nullptr;
			pDevice->GetTexture(0, &pOrigTexture);

			pDevice->SetTexture(0, BSShaderManagerEx::GetINTZDepthTexture(pRenderer, 0, 0)->GetDX9RendererData()->GetD3DTexture());

			DWORD uiOrigPointSize;
			DWORD uiOrgZEnable;
			DWORD uiOrgZWriteEnable;
			DWORD uiOrgColorWriteEnable;
			pDevice->GetRenderState(D3DRS_POINTSIZE, &uiOrigPointSize);
			pDevice->GetRenderState(D3DRS_ZENABLE, &uiOrgZEnable);
			pDevice->GetRenderState(D3DRS_ZWRITEENABLE, &uiOrgZWriteEnable);
			pDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &uiOrgColorWriteEnable);

			pDevice->SetRenderState(D3DRS_ZENABLE, 0);
			pDevice->SetRenderState(D3DRS_ZWRITEENABLE, 0);
			pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0);

			D3DXVECTOR3 vDummyPoint(0.0f, 0.0f, 0.0f);
			pDevice->DrawPrimitiveUP(D3DPT_POINTLIST, 1, vDummyPoint, sizeof(D3DXVECTOR3));

			pDevice->SetRenderState(D3DRS_POINTSIZE, 0x7FA05000);

			pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, uiOrgColorWriteEnable);
			pDevice->SetRenderState(D3DRS_ZWRITEENABLE, uiOrgZWriteEnable);
			pDevice->SetRenderState(D3DRS_ZENABLE, uiOrgZEnable);
			pDevice->SetRenderState(D3DRS_POINTSIZE, uiOrigPointSize);

			pDevice->SetTexture(0, pOrigTexture);
			
			if (pOrigTexture)
				pOrigTexture->Release();
		}
	}

	static void __fastcall RenderImageSpaceEffects(BSShaderAccumulator* apAccumulator, BSRenderedTexture* apTexture) {
		if (!kPostDepthEffects.empty()) {
			NiDX9Renderer* const pRenderer = NiDX9Renderer::GetSingleton();
			NiRenderTargetGroup* const pCurrentRT = pRenderer->GetCurrentRenderTargetGroup();

			BSRenderedTexture::StopOffscreen();

			for (const auto& pEffect : kPostDepthEffects) {
				if (!pEffect)
					continue;
				if (!pEffect->IsActive())
					continue;

				ImageSpaceManager::GetSingleton()->RenderEffect(pEffect, pRenderer, apTexture, apTexture, nullptr, true);
			}

			BSRenderedTexture::StartOffscreen(NiRenderer::CLEAR_NONE, pCurrentRT);
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

		// Normal render
		if constexpr (auiIndex == 0) {
			BSShaderAccumulator* pThis = reinterpret_cast<BSShaderAccumulator*>(this);
			BSRenderedTexture* pISTexture = BSShaderManager::GetCurrentRenderTarget();
			if (pThis->bWorldGeometry && pISTexture) {
				RenderImageSpaceEffects(pThis, pISTexture);
			}
		}

		ThisCall(kSortAlphaDetours[auiIndex].GetOverwrittenAddr(), this);
	}

	// Post-water alpha
	void RenderGeometryGroup(BSBatchRenderer::GroupType auiGeometryGroup, bool abAlphaPass) {
		ResolveDepth(nullptr);

		ThisCall(kRenderGeometryGroupDetour.GetOverwrittenAddr(), this, auiGeometryGroup, abAlphaPass);
	}
};

class TESMainEx {
public:
	void RenderDepthOfField(BSShaderAccumulator* apAccumulator, BSRenderedTexture* apRenderedTexture) {
		// Depth is set up already, clear the render targets though to make the ISE render correctly.
		BSRenderedTexture::StopOffscreen();
		return;
	}
};

VirtFuncDetour kDoFUpdateParamsDetour;
VirtFuncDetour kDoFReturnDetour;
class ImageSpaceEffectDepthOfFieldEx : public ImageSpaceEffectDepthOfField {
public:
	bool UpdateParamsEx(int a2) {
		bool bResult = ThisCall<bool>(kDoFUpdateParamsDetour.GetOverwrittenAddr(), this, a2);

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

		ThisCall(kDoFReturnDetour.GetOverwrittenAddr(), this);
	}
};


VirtFuncDetour kRadialBlurReturnDetour;
class ImageSpaceEffectRadialBlurEx : public ImageSpaceEffect {
public:
	void ReturnTexturesEx() {
		if (kTextures.GetAt(3))
			kTextures.GetAt(3)->ClearTexture();

		ThisCall(kRadialBlurReturnDetour.GetOverwrittenAddr(), this);
	}
};

template<bool abImplicit>
class DepthStencilHooks {
public:
	static inline CallDetour kDepthAddSurfaceDetour;
	static inline CallDetour kDepthAddSurfaceDetourAlt;

	void AddSurface(Ni2DBuffer::RendererData* apRendererData) {
		ThisCall(kDepthAddSurfaceDetour.GetOverwrittenAddr(), this, apRendererData);
		auto pSurface = reinterpret_cast<Ni2DBuffer::NiDX9TextureBufferData*>(apRendererData)->m_pkD3DSurface;
		if (bNVAPI && pSurface) {
			_MESSAGE("Registering %s surface %08X", abImplicit ? "Implicit" : "Additional", pSurface);
			NvAPI_D3D9_RegisterResource(pSurface);
		}
	}

	static NiDepthStencilBuffer* AddSurfaceAlt(uint32_t auiWidth, uint32_t auiHeight, Ni2DBuffer::RendererData* apRendererData)
		requires (abImplicit == true)
	{
		NiDepthStencilBuffer* pBuffer = CdeclCall<NiDepthStencilBuffer*>(kDepthAddSurfaceDetourAlt.GetOverwrittenAddr(), auiWidth, auiHeight, apRendererData);
		auto pSurface = reinterpret_cast<Ni2DBuffer::NiDX9TextureBufferData*>(apRendererData)->m_pkD3DSurface;
		if (bNVAPI && pSurface) {
			_MESSAGE("Registering %s surface %08X", abImplicit ? "Implicit" : "Additional", pSurface);
			NvAPI_D3D9_RegisterResource(pSurface);
		}
		return pBuffer;
	}

	static inline VirtFuncDetour kDepthRecreateDetour;
	bool RecreateSurface(LPDIRECT3DDEVICE9 apDevice) {
		bool bRecreated = ThisCall<bool>(kDepthRecreateDetour.GetOverwrittenAddr(), this, apDevice);
		if (bNVAPI && bRecreated) {
			Ni2DBuffer::NiDX9TextureBufferData* pThis = reinterpret_cast<Ni2DBuffer::NiDX9TextureBufferData*>(this);
			auto pSurface = pThis->m_pkD3DSurface;
			if (pSurface) {
				_MESSAGE("Recreating %s surface %08X", abImplicit ? "Implicit" : "Additional", pSurface);
				NvAPI_D3D9_RegisterResource(pSurface);
			}
		}
		return bRecreated;
	}

	static inline VirtFuncDetour kDepthReleaseDetour;
	void ReleaseSurface() {
		Ni2DBuffer::NiDX9TextureBufferData* pThis = reinterpret_cast<Ni2DBuffer::NiDX9TextureBufferData*>(this);
		auto pSurface = pThis->m_pkD3DSurface;
		if (bNVAPI && pSurface) {
			_MESSAGE("Releasing %s surface %08X", abImplicit ? "Implicit" : "Additional", pSurface);
			NvAPI_D3D9_UnregisterResource(pSurface);
		}

		ThisCall(kDepthReleaseDetour.GetOverwrittenAddr(), this);
	}

	static inline CallDetour kDepthRemoveDetour;
	void RemoveSurface(Ni2DBuffer::NiDX9TextureBufferData*& apRendererData) 
		requires (abImplicit == false)
	{
		if (bNVAPI && apRendererData) {
			auto pSurface = apRendererData->m_pkD3DSurface;
			if (pSurface) {
				_MESSAGE("Deleting %s surface %08X", abImplicit ? "Implicit" : "Additional", pSurface);
				NvAPI_D3D9_UnregisterResource(pSurface);
			}
		}
		ThisCall(kDepthRemoveDetour.GetOverwrittenAddr(), this, &apRendererData);
	}

	void RemoveSurface() 
		requires (abImplicit == true)
	{
		if (bNVAPI) {
			Ni2DBuffer::NiDX9TextureBufferData* pThis = reinterpret_cast<Ni2DBuffer::NiDX9TextureBufferData*>(this);
			auto pSurface = pThis->m_pkD3DSurface;
			if (pSurface) {
				_MESSAGE("Deleting %s surface %08X", abImplicit ? "Implicit" : "Additional", pSurface);
				NvAPI_D3D9_UnregisterResource(pSurface);
			}
		}
		ThisCall(kDepthRemoveDetour.GetOverwrittenAddr(), this);
	}

	DepthStencilHooks() {
		if constexpr (abImplicit) {
			// NiDX9ImplicitDepthStencilBufferData
			kDepthAddSurfaceDetour.ReplaceCallEx(0xE7D915, &DepthStencilHooks::AddSurface);
			kDepthAddSurfaceDetourAlt.ReplaceCall(0xE7D909, DepthStencilHooks::AddSurfaceAlt);
			kDepthRecreateDetour.ReplaceVirtualFuncEx(0x10EF16C, &DepthStencilHooks::RecreateSurface);
			kDepthReleaseDetour.ReplaceVirtualFuncEx(0x10EF17C, &DepthStencilHooks::ReleaseSurface);
			kDepthRemoveDetour.ReplaceCallEx(0xE7D693, &DepthStencilHooks::RemoveSurface);
		}
		else {
			// NiDX9AdditionalDepthStencilBufferData
			kDepthAddSurfaceDetour.ReplaceCallEx(0xE7DB3F, &DepthStencilHooks::AddSurface);
			kDepthRecreateDetour.ReplaceVirtualFuncEx(0x10EF1DC, &DepthStencilHooks::RecreateSurface);
			kDepthReleaseDetour.ReplaceVirtualFuncEx(0x10EF1EC, &DepthStencilHooks::ReleaseSurface);
			kDepthRemoveDetour.ReplaceCallEx(0xE7DBE3, &DepthStencilHooks::RemoveSurface);
		}
	}
};

CallDetour kInitDeviceCapsDetour;
class NiDX9RendererEx : public NiDX9Renderer {
public:
	bool InitializeDeviceCaps(D3DPRESENT_PARAMETERS& arPresentParams) {
		const bool bResult = ThisCall<bool>(kInitDeviceCapsDetour.GetOverwrittenAddr(), this, &arPresentParams);
		if (bResult) {
			const bool bDXVK = CheckDXVK();

			_MESSAGE("DXVK status: %u", bDXVK);

			IDirect3D9* pD3D9 = GetD3D9();
			D3DDISPLAYMODE kDisplayMode;
			pD3D9->GetAdapterDisplayMode(D3DADAPTER_DEFAULT, &kDisplayMode);
			bRESZ = pD3D9->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, kDisplayMode.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, (D3DFORMAT)MAKEFOURCC('R', 'E', 'S', 'Z')) == D3D_OK;

			_MESSAGE("RESZ status: %u", bRESZ);

			if (!bRESZ && bDXVK) {
				MessageBox(NULL, "Incompatible DXVK version.\nYour version of DXVK is incompatible with Depth Resolve. Use version <= 2.6.1, or >= 2.7.1.", "Depth Resolve", MB_OK | MB_ICONERROR);
				ExitProcess(0);
			}

			bNVAPI = !bRESZ && NvAPI_Initialize() == NVAPI_OK;
			if (bNVAPI) {
				// Hooks for buffer lifetime management
				DepthStencilHooks<false>(); // NiDX9AdditionalDepthStencilBufferData
				DepthStencilHooks<true>(); // NiDX9ImplicitDepthStencilBufferData
			}

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
				uiWidth = arPresentParams.BackBufferWidth;
				uiHeight = arPresentParams.BackBufferHeight;
			}
			BSShaderManagerEx::GetINTZDepthTexture(this, uiWidth, uiHeight);

			AddResetNotificationFunc(BSShaderManagerEx::INTZTextureResetCallback, nullptr);
		}

		return bResult;
	}
};

void InitHooks() {
	// Initializer
	kInitDeviceCapsDetour.ReplaceCallEx(0xE73374, &NiDX9RendererEx::InitializeDeviceCaps);

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

	kDoFUpdateParamsDetour.ReplaceVirtualFuncEx(0x10BC42C, &ImageSpaceEffectDepthOfFieldEx::UpdateParamsEx);
	kDoFReturnDetour.ReplaceVirtualFuncEx(0x10BC424, &ImageSpaceEffectDepthOfFieldEx::ReturnTexturesEx);
	kRadialBlurReturnDetour.ReplaceVirtualFuncEx(0x10ADC7C, &ImageSpaceEffectRadialBlurEx::ReturnTexturesEx);
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