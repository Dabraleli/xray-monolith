#include "stdafx.h"

static bool coop_no_render_streams() { return (strstr(Core.Params, "-coop_server_probe") && strstr(Core.Params, "-coop_server_nodraw") && strstr(Core.Params, "-coop_server_cpu_mesh") && strstr(Core.Params, "-coop_server_cpu_level") && strstr(Core.Params, "-coop_server_cpu_target") && strstr(Core.Params, "-coop_server_no_ui_resources") && strstr(Core.Params, "-coop_server_no_particle_graphics") && strstr(Core.Params, "-coop_server_no_render_streams")); }
#pragma hdrstop

#if defined(USE_DX10) || defined(USE_DX11)
#include "../xrRenderDX10/dx10BufferUtils.h"
#endif	//	USE_DX11

CBackend RCache;

// Create Quad-IB
#if defined(USE_DX10) || defined(USE_DX11)

void CBackend::RestoreQuadIBData()
{
}

void CBackend::CreateQuadIB()
{
	static const u32 dwTriCount = 4 * 1024;
	static const u32 dwIdxCount = dwTriCount * 2 * 3;
	u16 IndexBuffer[dwIdxCount];
	u16* Indices = IndexBuffer;

	D3D_BUFFER_DESC desc;
	desc.ByteWidth = dwIdxCount * 2;

	desc.Usage = D3D_USAGE_DEFAULT;
	desc.BindFlags = D3D_BIND_INDEX_BUFFER;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	D3D_SUBRESOURCE_DATA subData;
	subData.pSysMem = IndexBuffer;

	{
		int Cnt = 0;
		int ICnt = 0;
		for (int i = 0; i < dwTriCount; i++)
		{
			Indices[ICnt++] = u16(Cnt + 0);
			Indices[ICnt++] = u16(Cnt + 1);
			Indices[ICnt++] = u16(Cnt + 2);

			Indices[ICnt++] = u16(Cnt + 3);
			Indices[ICnt++] = u16(Cnt + 2);
			Indices[ICnt++] = u16(Cnt + 1);

			Cnt += 4;
		}
	}

	R_CHK(HW.pDevice->CreateBuffer ( &desc, &subData, &QuadIB));
	HW.stats_manager.increment_stats_ib(QuadIB);
}

#else	//	USE_DX11

void CBackend::RestoreQuadIBData()
{
	const u32 dwTriCount = 4 * 1024;
	u16* Indices = 0;
	R_CHK(QuadIB->Lock(0,0,(void**)&Indices,0));
	{
		int Cnt = 0;
		int ICnt = 0;
		for (int i = 0; i < dwTriCount; i++)
		{
			Indices[ICnt++] = u16(Cnt + 0);
			Indices[ICnt++] = u16(Cnt + 1);
			Indices[ICnt++] = u16(Cnt + 2);

			Indices[ICnt++] = u16(Cnt + 3);
			Indices[ICnt++] = u16(Cnt + 2);
			Indices[ICnt++] = u16(Cnt + 1);

			Cnt += 4;
		}
	}
	R_CHK(QuadIB->Unlock());
}

void CBackend::CreateQuadIB()
{
	const u32 dwTriCount = 4 * 1024;
	const u32 dwIdxCount = dwTriCount * 2 * 3;
	u16* Indices = 0;
	u32 dwUsage = D3DUSAGE_WRITEONLY;
	if (HW.Caps.geometry.bSoftware) dwUsage |= D3DUSAGE_SOFTWAREPROCESSING;
	R_CHK(HW.pDevice->CreateIndexBuffer (dwIdxCount*2,dwUsage,D3DFMT_INDEX16,D3DPOOL_DEFAULT,&QuadIB,NULL));
	HW.stats_manager.increment_stats_ib(QuadIB);

	R_CHK(QuadIB->Lock(0,0,(void**)&Indices,0));
	{
		int Cnt = 0;
		int ICnt = 0;
		for (int i = 0; i < dwTriCount; i++)
		{
			Indices[ICnt++] = u16(Cnt + 0);
			Indices[ICnt++] = u16(Cnt + 1);
			Indices[ICnt++] = u16(Cnt + 2);

			Indices[ICnt++] = u16(Cnt + 3);
			Indices[ICnt++] = u16(Cnt + 2);
			Indices[ICnt++] = u16(Cnt + 1);

			Cnt += 4;
		}
	}
	R_CHK(QuadIB->Unlock());
}

#endif	//	USE_DX11

// Device dependance
void CBackend::OnDeviceCreate()
{
    if (coop_no_render_streams())
    {
        QuadIB = nullptr;
        R_ASSERT(!Vertex.Buffer() && !Index.Buffer());
        Invalidate();
        Msg("[COOP_SERVER] RENDER_STREAMS_SKIPPED vertex=0 index=0 quad=0 debug_draw=0");
        return;
    }
	CreateQuadIB();

	// streams
	Vertex.Create();
	Index.Create();

	InitDebugDraw();

	// invalidate caching
	Invalidate();
}

void CBackend::OnDeviceDestroy()
{
    if (coop_no_render_streams())
    {
        R_ASSERT(!Vertex.Buffer() && !Index.Buffer() && !QuadIB);
        Msg("[COOP_SERVER] RENDER_STREAMS_RELEASE vertex=0 index=0 quad=0");
        return;
    }
	// streams
	Index.Destroy();
	Vertex.Destroy();

	DestroyDebugDraw();

	// Quad
	HW.stats_manager.decrement_stats_ib(QuadIB);
	_RELEASE(QuadIB);
}
