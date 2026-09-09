#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>

void Overlay_Init(IDirect3DDevice9* pDev);
void Overlay_OnPresent(IDirect3DDevice9* pDev);
void Overlay_OnPreReset();
void Overlay_OnPostReset(IDirect3DDevice9* pDev);
void Overlay_Shutdown();
bool Overlay_IsMenuOpen();
void Overlay_AddMouseWheel(float wheelX, float wheelY);
