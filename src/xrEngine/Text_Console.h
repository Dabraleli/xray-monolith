#pragma once
#include "XR_IOConsole.h"
#include "IGame_Level.h"

class ENGINE_API CTextConsole : public CConsole
{
private:
	typedef CConsole inherited;

private:
    HWND m_coop_edit = nullptr;
    ULONGLONG m_coop_started = 0;
    int m_coop_scroll = 0;
    bool m_coop_destroyed = false;
	HWND* m_pMainWnd;

	HWND m_hConsoleWnd;
	void CreateConsoleWnd();

	HWND m_hLogWnd;
	void CreateLogWnd();

	bool m_bScrollLog;
	u32 m_dwStartLine;
	void DrawLog(HDC hDC, RECT* pRect);

private:
	HFONT m_hLogWndFont;
	HFONT m_hPrevFont;
	HBRUSH m_hBackGroundBrush;

	HDC m_hDC_LogWnd;
	HDC m_hDC_LogWnd_BackBuffer;
	HBITMAP m_hBB_BM, m_hOld_BM;

	bool m_bNeedUpdate;
	u32 m_dwLastUpdateTime;

	u32 m_last_time;
	CServerInfo m_server_info;

public:
	CTextConsole();
	virtual ~CTextConsole();

	virtual void Initialize();
	virtual void Destroy();

	virtual void OnRender();
	virtual void _BCL OnFrame();

	// virtual void IR_OnKeyboardPress (int dik);

	void AddString(LPCSTR string);
	void OnPaint();
    void CoopInitialize();
    void CoopDestroy();
    void CoopResize();
    void CoopRefresh();
    void CoopScroll(int delta);
    void DrawCoopLog(HDC dc);
}; // class TextConsole

ENGINE_API bool CoopConsoleEnabled();
ENGINE_API void CoopConsoleSetInfo(const CServerInfo& info);
