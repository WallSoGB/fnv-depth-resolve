#pragma once

#include "Gamebryo/NiTexture.hpp"
#include "Gamebryo/NiDX9TextureData.hpp"

class BSD3DTexture : public NiTexture {
private:
	static void* vtable[41];

	void Destroy(uint8_t aucFlags);
public:
	NiNewRTTI(BSD3DTexture, NiTexture);

	static BSD3DTexture* __fastcall CreateObject(IDirect3DTexture9* apD3DTexture, NiDX9Renderer* apRenderer);

	uint32_t GetWidthEx() const;
	uint32_t GetHeightEx() const;
};

class BSD3DTextureData : public NiDX9TextureData {
private:
	static void* vtable[45];

	void Destroy(uint8_t aucFlags);
public:
	NiNewRTTI(BSD3DTextureData, NiDX9TextureData);

	static BSD3DTextureData* __fastcall CreateObject(NiTexture* apTexture, IDirect3DTexture9* apD3DTexture, NiDX9Renderer* apRenderer);
};
