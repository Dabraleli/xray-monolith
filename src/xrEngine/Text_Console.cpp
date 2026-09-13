#include "stdafx.h"
#include "Text_Console.h"
#include "line_editor.h"

// Coop uses the existing CTextConsole and Device HWND as the service window.
static WNDPROC coop_parent_proc = nullptr;
static WNDPROC coop_edit_proc = nullptr;
static CServerInfo coop_server_info;
static bool coop_console_close = false;

bool CoopConsoleEnabled()
{
    return strstr(Core.Params, "-coop_server_probe") && strstr(Core.Params, "-coop_server_nodraw") &&
        strstr(Core.Params, "-coop_server_console");
}

void CoopConsoleSetInfo(const CServerInfo& info) { coop_server_info = info; }

static LRESULT CALLBACK CoopEditProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    if (msg == WM_KEYDOWN && w == VK_RETURN)
    {
        char command[256] = {};
        GetWindowText(hwnd, command, sizeof(command));
        SetWindowText(hwnd, "");
        Msg("> %s", command);
        if (!_stricmp(command, "quit") || !_stricmp(command, "exit"))
            PostMessage(Device.m_hWnd, WM_CLOSE, 0, 0);
        else if (!_stricmp(command, "status"))
        {
            Msg("[COOP_CONSOLE] Status requested; engine_ready=%u", Device.b_is_Ready ? 1 : 0);
            for (u32 i = 0; i < coop_server_info.Size(); ++i) Msg("%s", coop_server_info[i].name);
        }
        else if (!_stricmp(command, "help") || !command[0])
            Msg("[COOP_CONSOLE] Probe commands: status, help, quit; anything else goes to the engine console (save <name>, load <name>, ...)");
        else if (Console && Device.b_is_Ready)
            Console->Execute(command); // the engine console: the server's save/load and the rest
        else
            Msg("[COOP_CONSOLE] engine not ready for: %s", command);
        return 0;
    }
    if (msg == WM_CHAR && w == VK_RETURN) return 0;
    return CallWindowProc(coop_edit_proc, hwnd, msg, w, l);
}

static LRESULT CALLBACK CoopParentProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l)
{
    CTextConsole* text = static_cast<CTextConsole*>(Console);
    if (msg == WM_TIMER && w == 42)
    {
        text->CoopRefresh();
        return 0;
    }
    if (msg == WM_SIZE)
    {
        if (w != SIZE_MINIMIZED) text->CoopResize();
        return 0; // resizing a GDI service window must not reset D3D
    }
    if (msg == WM_GETMINMAXINFO)
    {
        reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {640, 420};
        return 0;
    }
    if (msg == WM_CLOSE)
    {
        coop_console_close = true;
        Msg("[COOP_CONSOLE] CLOSE_REQUEST");
        if (strstr(Core.Params, "-coop_console_test")) PostQuitMessage(0);
        return 0;
    }
    // Keep simulation activation, but do not route GDI window messages through
    // game cursor/fullscreen/ImGui handling (also valid before renderer startup).
    if (msg == WM_ACTIVATE && !strstr(Core.Params, "-coop_console_test"))
        return CallWindowProc(coop_parent_proc, hwnd, msg, w, l);
    return DefWindowProc(hwnd, msg, w, l);
}

void CTextConsole::CoopResize()
{
    RECT rc; GetClientRect(Device.m_hWnd, &rc);
    MoveWindow(m_hConsoleWnd, 0, 0, rc.right, _max(1L, rc.bottom - 34), TRUE);
    MoveWindow(m_hLogWnd, 0, 0, rc.right, _max(1L, rc.bottom - 34), TRUE);
    if (m_coop_edit) MoveWindow(m_coop_edit, 8, rc.bottom - 29, _max(1L, rc.right - 16), 24, TRUE);
}

void CTextConsole::CoopRefresh()
{
    InvalidateRect(m_hLogWnd, nullptr, FALSE);
}

void CTextConsole::CoopScroll(int delta)
{
    m_coop_scroll = _max(0, m_coop_scroll + delta);
    CoopRefresh();
}

void CTextConsole::CoopInitialize()
{
    coop_console_close = false;
    m_coop_started = GetTickCount64();
    coop_parent_proc = WNDPROC(SetWindowLongPtr(Device.m_hWnd, GWLP_WNDPROC, LONG_PTR(CoopParentProc)));
    SetWindowLongPtr(Device.m_hWnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN);
    SetWindowText(Device.m_hWnd, "S.T.A.L.K.E.R.: Anomaly Coop Server");
    m_coop_edit = CreateWindowEx(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        8, 440, 620, 24, Device.m_hWnd, nullptr, GetModuleHandle(nullptr), nullptr);
    R_ASSERT(m_coop_edit);
    SendMessage(m_coop_edit, EM_SETLIMITTEXT, 255, 0);
    SendMessage(m_coop_edit, WM_SETFONT, WPARAM(m_hLogWndFont), TRUE);
    coop_edit_proc = WNDPROC(SetWindowLongPtr(m_coop_edit, GWLP_WNDPROC, LONG_PTR(CoopEditProc)));
    SetWindowPos(Device.m_hWnd, HWND_NOTOPMOST, 0, 0, 800, 600,
        SWP_NOMOVE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    CoopResize();
    R_ASSERT(SetTimer(Device.m_hWnd, 42, 250, nullptr));
    Device.seqFrame.Add(this);
    Msg("[COOP_CONSOLE] GDI_READY parent=Device_HWND engine_ready=%u", Device.b_is_Ready ? 1 : 0);
}

void CTextConsole::CoopDestroy()
{
    Device.seqFrame.Remove(this);
    KillTimer(Device.m_hWnd, 42);
    if (coop_parent_proc)
    {
        SetWindowLongPtr(Device.m_hWnd, GWLP_WNDPROC, LONG_PTR(coop_parent_proc));
        coop_parent_proc = nullptr;
    }
    if (m_coop_edit) { DestroyWindow(m_coop_edit); m_coop_edit = nullptr; }
    coop_server_info.ResetData();
}

void CTextConsole::DrawCoopLog(HDC dc)
{
    RECT rc; GetClientRect(m_hLogWnd, &rc);
    FillRect(dc, &rc, HBRUSH(GetStockObject(BLACK_BRUSH)));
    HFONT old = HFONT(SelectObject(dc, m_hLogWndFont));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(180, 240, 180));
    string128 heading;
    xr_sprintf(heading, "Anomaly Coop | Uptime %llu s | %s", (GetTickCount64() - m_coop_started) / 1000,
        strstr(Core.Params, "-coop_console_test") ? "Console test: no D3D" : "Server probe");
    TextOut(dc, 10, 6, heading, xr_strlen(heading));
    int y = 26;
    for (u32 i = 0; i < coop_server_info.Size(); ++i)
    {
        SetTextColor(dc, coop_server_info[i].color);
        TextOut(dc, 10 + (i % 2) * (rc.right / 2), y + (i / 2) * 18,
            coop_server_info[i].name, xr_strlen(coop_server_info[i].name));
    }
    int boundary = 178;
    if (!coop_server_info.Size())
        TextOut(dc, 10, 30, "Commands: status, help, quit", 26);
    xr_vector<xr_string> lines;
    CopyLogTail(lines, 1000);
    m_coop_scroll = _min(m_coop_scroll, _max(0, int(lines.size()) - 1));
    int bottom = rc.bottom - 20;
    for (int i = int(lines.size()) - 1 - m_coop_scroll; i >= 0 && bottom >= boundary; --i, bottom -= 17)
    {
        LPCSTR line = lines[i].c_str();
        COLORREF color = RGB(210, 210, 210);
        if (line[0] == '!' || strstr(line, "FATAL") || strstr(line, "ERROR")) color = RGB(255, 85, 85);
        else if (strstr(line, "[COOP_")) color = RGB(110, 220, 130);
        SetTextColor(dc, color);
        TextOut(dc, 10, bottom, line, xr_strlen(line));
    }
    SelectObject(dc, old);
}


extern char const* const ioc_prompt;
extern char const* const ch_cursor;
int g_svTextConsoleUpdateRate = 1;

CTextConsole::CTextConsole()
{
	m_pMainWnd = NULL;
	m_hConsoleWnd = NULL;
	m_hLogWnd = NULL;
	m_hLogWndFont = NULL;

	m_bScrollLog = true;
	m_dwStartLine = 0;

	m_bNeedUpdate = false;
	m_dwLastUpdateTime = Device.dwTimeGlobal;
	m_last_time = Device.dwTimeGlobal;
}

CTextConsole::~CTextConsole()
{
	m_pMainWnd = NULL;
}

//-------------------------------------------------------------------------------------------
LRESULT CALLBACK TextConsole_WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void CTextConsole::CreateConsoleWnd()
{
	HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(0);
	//----------------------------------
	RECT cRc;
	GetClientRect(*m_pMainWnd, &cRc);
	INT lX = cRc.left;
	INT lY = cRc.top;
	INT lWidth = cRc.right - cRc.left;
	INT lHeight = cRc.bottom - cRc.top;
	//----------------------------------
	const char* wndclass = "TEXT_CONSOLE";

	// Register the windows class
	WNDCLASS wndClass = {
		0, TextConsole_WndProc, 0, 0, hInstance,
		NULL,
		LoadCursor(hInstance, IDC_ARROW),
		GetStockBrush(GRAY_BRUSH),
		NULL, wndclass
	};
	RegisterClass(&wndClass);

	// Set the window's initial style
	u32 dwWindowStyle = WS_OVERLAPPED | WS_CHILD | WS_VISIBLE; // | WS_CLIPSIBLINGS;// | WS_CLIPCHILDREN;

	// Set the window's initial width
	RECT rc;
	SetRect(&rc, lX, lY, lWidth, lHeight);
	// AdjustWindowRect( &rc, dwWindowStyle, FALSE );

	// Create the render window
	m_hConsoleWnd = CreateWindow(wndclass, "XRAY Text Console", dwWindowStyle,
	                             lX, lY,
	                             lWidth, lHeight, *m_pMainWnd,
	                             0, hInstance, 0L);
	//---------------------------------------------------------------------------
	R_ASSERT2(m_hConsoleWnd, "Unable to Create TextConsole Window!");
};
//-------------------------------------------------------------------------------------------
LRESULT CALLBACK TextConsole_LogWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

void CTextConsole::CreateLogWnd()
{
	HINSTANCE hInstance = (HINSTANCE)GetModuleHandle(0);
	//----------------------------------
	RECT cRc;
	GetClientRect(m_hConsoleWnd, &cRc);
	INT lX = cRc.left;
	INT lY = cRc.top;
	INT lWidth = cRc.right - cRc.left;
	INT lHeight = cRc.bottom - cRc.top;
	//----------------------------------
	const char* wndclass = "TEXT_CONSOLE_LOG_WND";

	// Register the windows class
	WNDCLASS wndClass = {
		0, TextConsole_LogWndProc, 0, 0, hInstance,
		NULL,
		LoadCursor(NULL, IDC_ARROW),
		GetStockBrush(BLACK_BRUSH),
		NULL, wndclass
	};
	RegisterClass(&wndClass);

	// Set the window's initial style
	u32 dwWindowStyle = WS_OVERLAPPED | WS_CHILD | WS_VISIBLE; // | WS_CLIPSIBLINGS;
	// u32 dwWindowStyleEx = WS_EX_CLIENTEDGE;

	// Set the window's initial width
	RECT rc;
	SetRect(&rc, lX, lY, lWidth, lHeight);
	// AdjustWindowRect( &rc, dwWindowStyle, FALSE );

	// Create the render window
	m_hLogWnd = CreateWindow(wndclass, "XRAY Text Console Log", dwWindowStyle,
	                         lX, lY,
	                         lWidth, lHeight, m_hConsoleWnd,
	                         0, hInstance, 0L);
	//---------------------------------------------------------------------------
	R_ASSERT2(m_hLogWnd, "Unable to Create TextConsole Window!");
	//---------------------------------------------------------------------------
	ShowWindow(m_hLogWnd, SW_SHOW);
	UpdateWindow(m_hLogWnd);
	//-----------------------------------------------
	LOGFONT lf;
	lf.lfHeight = CoopConsoleEnabled() ? -15 : -12;
	lf.lfWidth = 0;
	lf.lfEscapement = 0;
	lf.lfOrientation = 0;
	lf.lfWeight = FW_NORMAL;
	lf.lfItalic = 0;
	lf.lfUnderline = 0;
	lf.lfStrikeOut = 0;
	lf.lfCharSet = DEFAULT_CHARSET;
	lf.lfOutPrecision = OUT_STRING_PRECIS;
	lf.lfClipPrecision = CLIP_STROKE_PRECIS;
	lf.lfQuality = DRAFT_QUALITY;
	lf.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
	xr_sprintf(lf.lfFaceName, sizeof(lf.lfFaceName), "%s", CoopConsoleEnabled() ? "Consolas" : "");

	m_hLogWndFont = CreateFontIndirect(&lf);
	R_ASSERT2(m_hLogWndFont, "Unable to Create Font for Log Window");
	//------------------------------------------------
	m_hDC_LogWnd = GetDC(m_hLogWnd);
	R_ASSERT2(m_hDC_LogWnd, "Unable to Get DC for Log Window!");
	//------------------------------------------------
	m_hDC_LogWnd_BackBuffer = CreateCompatibleDC(m_hDC_LogWnd);
	R_ASSERT2(m_hDC_LogWnd_BackBuffer, "Unable to Create Compatible DC for Log Window!");
	//------------------------------------------------
	GetClientRect(m_hLogWnd, &cRc);
	lWidth = cRc.right - cRc.left;
	lHeight = cRc.bottom - cRc.top;
	//----------------------------------
	m_hBB_BM = CreateCompatibleBitmap(m_hDC_LogWnd, lWidth, lHeight);
	R_ASSERT2(m_hBB_BM, "Unable to Create Compatible Bitmap for Log Window!");
	//------------------------------------------------
	m_hOld_BM = (HBITMAP)SelectObject(m_hDC_LogWnd_BackBuffer, m_hBB_BM);
	//------------------------------------------------
	m_hPrevFont = (HFONT)SelectObject(m_hDC_LogWnd_BackBuffer, m_hLogWndFont);
	//------------------------------------------------
	SetTextColor(m_hDC_LogWnd_BackBuffer, RGB(255, 255, 255));
	SetBkColor(m_hDC_LogWnd_BackBuffer, RGB(1, 1, 1));
	//------------------------------------------------
	m_hBackGroundBrush = GetStockBrush(BLACK_BRUSH);
}

void CTextConsole::Initialize()
{
	inherited::Initialize();

	m_pMainWnd = &Device.m_hWnd;
	m_dwLastUpdateTime = Device.dwTimeGlobal;
	m_last_time = Device.dwTimeGlobal;

	CreateConsoleWnd();
	CreateLogWnd();

	ShowWindow(m_hConsoleWnd, SW_SHOW);
	UpdateWindow(m_hConsoleWnd);

	m_server_info.ResetData();
    if (CoopConsoleEnabled()) CoopInitialize();
}

void CTextConsole::Destroy()
{
    if (CoopConsoleEnabled())
    {
        if (m_coop_destroyed) return;
        m_coop_destroyed = true;
        CoopDestroy();
        inherited::Destroy();
        SelectObject(m_hDC_LogWnd_BackBuffer, m_hPrevFont);
        SelectObject(m_hDC_LogWnd_BackBuffer, m_hOld_BM);
        DeleteObject(m_hBB_BM);
        DeleteObject(m_hLogWndFont);
        DeleteDC(m_hDC_LogWnd_BackBuffer);
        ReleaseDC(m_hLogWnd, m_hDC_LogWnd);
        // Previous/stock objects belong to Windows, not to this console.
        DestroyWindow(m_hLogWnd);
        DestroyWindow(m_hConsoleWnd);
        Msg("[COOP_CONSOLE] GDI_RELEASE");
        return;
    }
	inherited::Destroy();

	SelectObject(m_hDC_LogWnd_BackBuffer, m_hPrevFont);
	SelectObject(m_hDC_LogWnd_BackBuffer, m_hOld_BM);

	if (m_hBB_BM) DeleteObject(m_hBB_BM);
	if (m_hOld_BM) DeleteObject(m_hOld_BM);
	if (m_hLogWndFont) DeleteObject(m_hLogWndFont);
	if (m_hPrevFont) DeleteObject(m_hPrevFont);
	if (m_hBackGroundBrush) DeleteObject(m_hBackGroundBrush);

	ReleaseDC(m_hLogWnd, m_hDC_LogWnd_BackBuffer);
	ReleaseDC(m_hLogWnd, m_hDC_LogWnd);

	DestroyWindow(m_hLogWnd);
	DestroyWindow(m_hConsoleWnd);
}

void CTextConsole::OnRender()
{
} //disable СConsole::OnRender()

void CTextConsole::OnPaint()
{
    if (CoopConsoleEnabled())
    {
        PAINTSTRUCT paint;
        BeginPaint(m_hLogWnd, &paint);
        DrawCoopLog(paint.hdc);
        EndPaint(m_hLogWnd, &paint);
        return;
    }
	RECT wRC;
	PAINTSTRUCT ps;
	BeginPaint(m_hLogWnd, &ps);

	if (/*m_bNeedUpdate*/ Device.dwFrame % 2)
	{
		// m_dwLastUpdateTime = Device.dwTimeGlobal;
		// m_bNeedUpdate = false;

		GetClientRect(m_hLogWnd, &wRC);
		DrawLog(m_hDC_LogWnd_BackBuffer, &wRC);
	}
	else
	{
		wRC = ps.rcPaint;
	}


	BitBlt(m_hDC_LogWnd,
	       wRC.left, wRC.top,
	       wRC.right - wRC.left, wRC.bottom - wRC.top,
	       m_hDC_LogWnd_BackBuffer,
	       wRC.left, wRC.top,
	       SRCCOPY); //(FullUpdate) ? SRCCOPY : NOTSRCCOPY);
	/*
	 Msg ("URect - %d:%d - %d:%d", ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
	 */
	EndPaint(m_hLogWnd, &ps);
}

void CTextConsole::DrawLog(HDC hDC, RECT* pRect)
{
	TEXTMETRIC tm;
	GetTextMetrics(hDC, &tm);

	RECT wRC = *pRect;
	GetClientRect(m_hLogWnd, &wRC);
	FillRect(hDC, &wRC, m_hBackGroundBrush);

	int Width = wRC.right - wRC.left;
	int Height = wRC.bottom - wRC.top;
	wRC = *pRect;
	int y_top_max = (int)(0.32f * Height);

	//---------------------------------------------------------------------------------
	LPCSTR s_edt = ec().str_edit();
	LPCSTR s_cur = ec().str_before_cursor();

	u32 cur_len = xr_strlen(s_cur) + xr_strlen(ch_cursor) + 1;
	PSTR buf = (PSTR)_alloca(cur_len * sizeof(char));
	xr_strcpy(buf, cur_len, s_cur);
	xr_strcat(buf, cur_len, ch_cursor);
	buf[cur_len - 1] = 0;

	u32 cur0_len = xr_strlen(s_cur);

	int xb = 25;

	SetTextColor(hDC, RGB(255, 255, 255));
	TextOut(hDC, xb, Height - tm.tmHeight - 1, buf, cur_len - 1);
	buf[cur0_len] = 0;

	SetTextColor(hDC, RGB(0, 0, 0));
	TextOut(hDC, xb, Height - tm.tmHeight - 1, buf, cur0_len);


	SetTextColor(hDC, RGB(255, 255, 255));
	TextOut(hDC, 0, Height - tm.tmHeight - 3, ioc_prompt, xr_strlen(ioc_prompt)); // ">>> "

	SetTextColor(hDC, (COLORREF)bgr2rgb(get_mark_color(mark11)));
	TextOut(hDC, xb, Height - tm.tmHeight - 3, s_edt, xr_strlen(s_edt));

	SetTextColor(hDC, RGB(205, 205, 225));
	u32 log_line = LogFile.size() - 1;
	string16 q, q2;
	itoa(log_line, q, 10);
	xr_strcpy(q2, sizeof(q2), "[");
	xr_strcat(q2, sizeof(q2), q);
	xr_strcat(q2, sizeof(q2), "]");
	u32 qn = xr_strlen(q2);

	TextOut(hDC, Width - 8 * qn, Height - tm.tmHeight - tm.tmHeight, q2, qn);

	int ypos = Height - tm.tmHeight - tm.tmHeight;
	for (int i = LogFile.size() - 1 - scroll_delta; i >= 0; --i)
	{
		ypos -= tm.tmHeight;
		if (ypos < y_top_max)
		{
			break;
		}
		LPCSTR ls = LogFile[i].c_str();

		if (!ls)
		{
			continue;
		}
		Console_mark cm = (Console_mark)ls[0];
		COLORREF c2 = (COLORREF)bgr2rgb(get_mark_color(cm));
		SetTextColor(hDC, c2);
		u8 b = (is_mark(cm)) ? 2 : 0;
		LPCSTR pOut = ls + b;

		BOOL res = TextOut(hDC, 10, ypos, pOut, xr_strlen(pOut));
		if (!res)
		{
			R_ASSERT2(0, "TextOut(..) return NULL");
		}
	}

	if (g_pGameLevel && (Device.dwTimeGlobal - m_last_time > 500))
	{
		m_last_time = Device.dwTimeGlobal;

		m_server_info.ResetData();
		g_pGameLevel->GetLevelInfo(&m_server_info);
	}

	ypos = 5;
	for (u32 i = 0; i < m_server_info.Size(); ++i)
	{
		SetTextColor(hDC, m_server_info[i].color);
		TextOut(hDC, 10, ypos, m_server_info[i].name, xr_strlen(m_server_info[i].name));

		ypos += tm.tmHeight;
		if (ypos > y_top_max)
		{
			break;
		}
	}
}

/*
void CTextConsole::IR_OnKeyboardPress( int dik ) !!!!!!!!!!!!!!!!!!!!!
{
m_bNeedUpdate = true;
inherited::IR_OnKeyboardPress( dik );
}
*/
void CTextConsole::OnFrame()
{
    if (CoopConsoleEnabled())
    {
        if (coop_console_close && g_pGameLevel && g_pGameLevel->bReady)
        {
            coop_console_close = false;
            Msg("[COOP_CONSOLE] STOP_REQUEST");
            Execute("quit");
        }
        return;
    }
	inherited::OnFrame();
	/* if ( !m_bNeedUpdate && m_dwLastUpdateTime + 1000/g_svTextConsoleUpdateRate > Device.dwTimeGlobal )
	 {
	 return;
	 }
	 */
	InvalidateRect(m_hConsoleWnd, NULL, FALSE);
	SetCursor(LoadCursor(NULL, IDC_ARROW));
	// m_bNeedUpdate = true;
}
