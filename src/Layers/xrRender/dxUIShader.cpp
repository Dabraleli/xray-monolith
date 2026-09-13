#include "stdafx.h"
#include "dxUIShader.h"

// xrDebugNew.cpp provides this overload; the old one-argument declaration is stale.
void LogStackTrace(LPCSTR header, bool printStack);

xr_unordered_map<std::string, ref_shader> g_UIShadersCache;

static ref_shader& GetCachedUIShader(const char* sh, const char* tex)
{
    std::string key{ tex ? tex : "" };
    key += "_";
    key += sh;

    if (const auto it = g_UIShadersCache.find(key); it != g_UIShadersCache.end())
    {
        return it->second;
    }
    else
    {
        auto& shader = g_UIShadersCache[key];
        shader.create(sh, tex);
        return shader;
    }
}

void dxUIShader::Copy(IUIShader& _in)
{
	*this = *((dxUIShader*)&_in);
}

void dxUIShader::create(LPCSTR sh, LPCSTR tex, bool no_cache)
{
    if (strstr(Core.Params, "-coop_server_probe") && strstr(Core.Params, "-coop_server_no_render_streams") && strstr(Core.Params, "-coop_server_trace_shaders") && tex)
    {
        static bool common_reported = false, hud_reported = false;
        bool* reported = !xr_strcmp(tex, "ui\\ui_common") ? &common_reported : (!xr_strcmp(tex, "ui\\ui_hud") ? &hud_reported : nullptr);
        if (reported && !*reported) { *reported = true; Msg("[COOP_SERVER] UI_SHADER_ORIGIN %s", tex); LogStackTrace("UI shader creation diagnostic (not a crash)", true); }
    }
    // Headless coop server: Anomaly's world Lua builds HUD windows along the way (companion list,
    // artefact slots...). Without a device a shader compile is fatal; the windows exist, are
    // updated and never drawn (-coop_server_nodraw skips HUD rendering), so they need no shader.
    if (strstr(Core.Params, "-coop_server_probe") && strstr(Core.Params, "-coop_server_nodraw") && strstr(Core.Params, "-coop_server_no_game_ui") && strstr(Core.Params, "-coop_server_no_ui_resources"))
    {
        static u32 skipped = 0;
        if (skipped++ < 8) Msg("[COOP_SERVER] UI_SHADER_SKIPPED shader=%s texture=%s", sh ? sh : "", tex ? tex : "");
        return;
    }
    if (no_cache)
    {
        hShader.create(sh, tex);
    }
    else
    {
        hShader = GetCachedUIShader(sh, tex);
    }
}

//void dxUIShader::destroy() { hShader.destroy(); }
