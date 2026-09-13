#include "stdafx.h"

// Only the isolated coop probe omits weather rendering resources.
static bool coop_no_weather_graphics() { return (strstr(Core.Params, "-coop_server_probe") && strstr(Core.Params, "-coop_server_nodraw") && strstr(Core.Params, "-coop_server_cpu_target") && strstr(Core.Params, "-coop_server_no_weather_graphics")); }
#include "dxThunderboltDescRender.h"

void dxThunderboltDescRender::Copy(IThunderboltDescRender& _in)
{
	*this = *((dxThunderboltDescRender*)&_in);
}

void dxThunderboltDescRender::CreateModel(LPCSTR m_name)
{
    if (coop_no_weather_graphics()) { l_model = nullptr; return; }
	IReader* F = 0;
	F = FS.r_open("$game_meshes$", m_name);
	R_ASSERT2(F, "Empty 'lightning_model'.");
	l_model = ::RImplementation.model_CreateDM(F);
	FS.r_close(F);
}

void dxThunderboltDescRender::DestroyModel()
{
    if (coop_no_weather_graphics()) { return; }
	::RImplementation.model_Delete(l_model);
}
