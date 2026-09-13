#include "stdafx.h"

static bool coop_no_render_streams() { return (strstr(Core.Params, "-coop_server_probe") && strstr(Core.Params, "-coop_server_nodraw") && strstr(Core.Params, "-coop_server_cpu_mesh") && strstr(Core.Params, "-coop_server_cpu_level") && strstr(Core.Params, "-coop_server_cpu_target") && strstr(Core.Params, "-coop_server_no_ui_resources") && strstr(Core.Params, "-coop_server_no_particle_graphics") && strstr(Core.Params, "-coop_server_no_render_streams")); }
#include "dxWallMarkArray.h"

#include "dxUIShader.h"

void dxWallMarkArray::Copy(IWallMarkArray& _in)
{
	*this = *(dxWallMarkArray*)&_in;
}

dxWallMarkArray::~dxWallMarkArray()
{
	for (ShaderIt it = m_CollideMarks.begin(); it != m_CollideMarks.end(); ++it)
		it->destroy();
}

void dxWallMarkArray::AppendMark(LPCSTR s_textures)
{
    if (coop_no_render_streams())
    {
        static bool reported = false;
        if (!reported) { Msg("[COOP_SERVER] WALLMARK_SHADERS_SKIPPED materials=retained"); reported = true; }
        return;
    }
	ref_shader s;

#if defined(USE_DX11)

	LPCSTR sh_name = "effects\\wallmark";

	if (RImplementation.o.ssfx_blood)
	{
		// Use the blood shader for any texture with the name wm_blood_*
		if (strstr(s_textures, "wm_blood_"))
			sh_name = "effects\\wallmark_blood";
	}

	s.create(sh_name, s_textures);

#else
	s.create("effects\\wallmark", s_textures);
#endif

	m_CollideMarks.push_back(s);
}

void dxWallMarkArray::clear()
{
	return m_CollideMarks.clear();
}

bool dxWallMarkArray::empty()
{
	return m_CollideMarks.empty();
}

wm_shader dxWallMarkArray::GenerateWallmark()
{
	wm_shader res;
	if (!m_CollideMarks.empty())
		((dxUIShader*)&*res)->hShader = m_CollideMarks[::Random.randI(0, m_CollideMarks.size())];
	return res;
}

ref_shader* dxWallMarkArray::dxGenerateWallmark()
{
	return m_CollideMarks.empty() ? NULL : &m_CollideMarks[::Random.randI(0, m_CollideMarks.size())];
}
