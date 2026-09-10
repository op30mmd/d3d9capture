#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

void Overlay_Init(IDirect3DDevice9* pDev);
void Overlay_InitDXGI(IDXGISwapChain* pSwapChain);

void Overlay_OnPresent(IDirect3DDevice9* pDev);
void Overlay_OnPresentDXGI(IDXGISwapChain* pSwapChain);

void Overlay_OnPreReset();
void Overlay_OnPreResetDXGI();

void Overlay_OnPostReset(IDirect3DDevice9* pDev);
void Overlay_OnPostResetDXGI(IDXGISwapChain* pSwapChain);

void Overlay_Shutdown();
bool Overlay_IsMenuOpen();
void Overlay_AddMouseWheel(float wheelX, float wheelY);
