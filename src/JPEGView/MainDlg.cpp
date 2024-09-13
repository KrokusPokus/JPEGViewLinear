// MainDlg.cpp : implementation of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////

#include "stdafx.h"
#include "resource.h"
#include <math.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <shlwapi.h>
#include <regex>

#include "MainDlg.h"
#include "FileList.h"
#include "Helpers.h"
#include "JPEGProvider.h"
#include "JPEGImage.h"
#include "SettingsProvider.h"
#include "BasicProcessing.h"
#include "MultiMonitorSupport.h"
#include "Clipboard.h"
#include "HelpersGUI.h"
#include "TimerEventIDs.h"
#include "ResizeFilter.h"
#include "ProcessingThreadPool.h"
#include "DirectoryWatcher.h"

//////////////////////////////////////////////////////////////////////////////////////////////
// Constants
//////////////////////////////////////////////////////////////////////////////////////////////

static const int NUM_THREADS = 1; // number of readahead threads to use

// number of readahead buffers to use (NUM_THREADS+1 is a good choice)
// setting this to higher values will not only read more files ahead, since NUM_THREADS is still only 1,
// it will keep more of the already read file data cached, and, more importantly, will prevent more resampled
// DIBs from being thrown away. (DIBs are deleted the same time as file cache data)
// Of course, with large images, we will be starting to use a crapload of memory really fast
// Keep in mind that there is only this one pool for buffering images already seen and images that are read ahead,
// so when using NUM_THREADS=1 and READ_AHEAD_BUFFERS=5, we will read ahead 1 image, and keep cached the 1 present
// image as well as the previous 3 images.
// When using READ_AHEAD_BUFFERS=2, as the original author suggested, we would have a file buffer of the
// next and current current file and a DIB buffer of the current, but no file nor dib buffer for the previous files.
static const int READ_AHEAD_BUFFERS = 5;


static const int ZOOM_TIMEOUT = 50; // refinement done after this many milliseconds
static const int PAN_STEP = 48; // number of pixels to pan if pan with cursor keys (SHIFT+up/down/left/right)
static const int NO_REQUEST = 1; // used in GotoImage() method
static const int NO_REMOVE_KEY_MSG = 2; // used in GotoImage() method
static const int KEEP_PARAMETERS = 4; // used in GotoImage() method
static TCHAR s_PrevFileExt[MAX_PATH];
static TCHAR s_PrevTitleText[MAX_PATH];

//////////////////////////////////////////////////////////////////////////////////////////////
// Public
//////////////////////////////////////////////////////////////////////////////////////////////

CMainDlg::CMainDlg()
	{
	CSettingsProvider& sp = CSettingsProvider::This();

	CResizeFilterCache::This(); // Access before multiple threads are created

	m_bHQResampling = true;

	m_pFileList = NULL;
	m_pJPEGProvider = NULL;
	m_pCurrentImage = NULL;
	m_bOutOfMemoryLastImage = false;
	m_bExceptionErrorLastImage = false;
	m_nLastLoadError = HelpersGUI::FileLoad_Ok;
	
	m_eForcedSorting = Helpers::FS_Undefined;

	m_nRotation = 0;
	m_bMangaMode = false;
	m_bZoomMode = false;
	m_bMovieMode = false;
	m_dZoom = -1.0;
	m_offsets = CPoint(0, 0);
	m_offsets_custom = CPoint(0, 0);
	m_nMouseX = m_nMouseY = 0;
	m_bFullScreenMode = false;
	m_bLockPaint = true;
	m_nCurrentTimeout = 0;
	m_startMouse.x = m_startMouse.y = -1;
	m_virtualImageSize = CSize(-1, -1);
	m_bInLowQTimer = false;
	m_bPanTimerActive = false;
	m_bTemporaryLowQ = false;
	m_bSpanVirtualDesktop = false;
	m_storedWindowPlacement.length = sizeof(WINDOWPLACEMENT);
	memset(&m_storedWindowPlacement2, 0, sizeof(WINDOWPLACEMENT));
	m_storedWindowPlacement2.length = sizeof(WINDOWPLACEMENT);
	m_monitorRect = CRect(0, 0, 0, 0);
	m_bMouseOn = false;
    m_bIsAnimationPlaying = false;
	m_nLastAnimationOffset = 0;
	m_nExpectedNextAnimationTickCount = 0;
	m_bDWMenabled = FALSE;
	m_DynDwmFlush = 0;
	m_dLastImageDisplayTime = 0.0;
	m_nMangaSinglePageVisibleHeight = CSettingsProvider::This().MangaSinglePageVisibleHeight();
	}

CMainDlg::~CMainDlg()
	{
	delete m_pDirectoryWatcher;
	delete m_pFileList;
	if (m_pJPEGProvider != NULL)
		delete m_pJPEGProvider;

	if (m_hmodDwmapi)
		FreeLibrary(m_hmodDwmapi);
	}

LRESULT CMainDlg::OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{	
	CPaintDC dc(this->m_hWnd);

	HRESULT hresult = !S_OK;
	m_hmodDwmapi = LoadLibrary(_T("Dwmapi"));
	if (m_hmodDwmapi)
		{
		MyDwmIsCompositionEnabledType DynDwmIsCompositionEnabled = (MyDwmIsCompositionEnabledType)GetProcAddress(m_hmodDwmapi, "DwmIsCompositionEnabled");
		if (DynDwmIsCompositionEnabled)
			{
			hresult = DynDwmIsCompositionEnabled(&m_bDWMenabled);
			if (hresult != S_OK)
				::OutputDebugStringW(_T("[JpegView] Dwmapi library found, but DynDwmIsCompositionEnabled call failed"));
			}

		if (m_bDWMenabled == TRUE)
			{
			m_DynDwmFlush = (MyDwmFlushType)GetProcAddress(m_hmodDwmapi, "DwmFlush");
			if (!m_DynDwmFlush)
				::OutputDebugStringW(_T("[JpegView] Dwmapi library found, but DynDwmFlush isn't available"));
			}
		}
	else
		{
		::OutputDebugStringW(_T("[JpegView] Dwmapi library of Windows 7 not found"));
		}

	m_pDirectoryWatcher = new CDirectoryWatcher(m_hWnd);
	
	CSettingsProvider& sp = CSettingsProvider::This();
/* Debugging */	Helpers::CPUType cpu = sp.AlgorithmImplementation();
/* Debugging */	EFilterType filter = sp.DownsamplingFilter();
/* Debugging */	TCHAR sCPU[64];
/* Debugging */	TCHAR sFilter[64];
/* Debugging */	TCHAR debugtext[512];
/* Debugging */	if (cpu == Helpers::CPU_Unknown)
/* Debugging */		swprintf(sCPU,64,TEXT("%s"), TEXT("Unknown CPU"));
/* Debugging */	else if (cpu == Helpers::CPU_Generic)
/* Debugging */		swprintf(sCPU,64,TEXT("%s"), TEXT("Generic CPU"));
/* Debugging */	else if (cpu == Helpers::CPU_SSE)
/* Debugging */		swprintf(sCPU,64,TEXT("%s"), TEXT("128 bit SSE2"));
/* Debugging */	else if (cpu == Helpers::CPU_AVX2)
/* Debugging */		swprintf(sCPU,64,TEXT("%s"), TEXT("256 bit AVX2"));

/* Debugging */	if (filter == Filter_Downsampling_None)
/* Debugging */		swprintf(sFilter,64,TEXT("%s"), TEXT("Filter_Downsampling_None"));
/* Debugging */	else if (filter == Filter_Downsampling_Hermite)
/* Debugging */		swprintf(sFilter,64,TEXT("%s"), TEXT("Filter_Downsampling_Hermite"));
/* Debugging */	else if (filter == Filter_Downsampling_Catrom)
/* Debugging */		swprintf(sFilter,64,TEXT("%s"), TEXT("Filter_Downsampling_Catrom"));
/* Debugging */	else if (filter == Filter_Downsampling_Mitchell)
/* Debugging */		swprintf(sFilter,64,TEXT("%s"), TEXT("Filter_Downsampling_Mitchell"));
/* Debugging */	else if (filter == Filter_Downsampling_Lanczos2)
/* Debugging */		swprintf(sFilter,64,TEXT("%s"), TEXT("Filter_Downsampling_Lanczos2"));
/* Debugging */	
/* Debugging */	swprintf(debugtext,255,TEXT("[JpegView] %s / %s"), sCPU, sFilter);
//* Debugging */	::OutputDebugStringW(debugtext);

	// intitialize list of files to show with startup file (and folder)
	m_pFileList = new CFileList(m_sStartupFile, *m_pDirectoryWatcher,
		(m_eForcedSorting == Helpers::FS_Undefined) ? sp.Sorting() : m_eForcedSorting, sp.IsSortedUpcounting(), sp.WrapAroundFolder(),
		0, m_eForcedSorting != Helpers::FS_Undefined);
	m_pFileList->SetNavigationMode(sp.Navigation());

	// >>>>> File name known here
	//::MessageBox(NULL,m_pFileList->Current(),TEXT("File name"), MB_OK | MB_ICONERROR);

	TCHAR sCurrentFileName[MAX_PATH];
	lstrcpy(sCurrentFileName,m_pFileList->Current());
	
	if (sCurrentFileName != NULL)
		{
		if (StrStrI(sCurrentFileName,TEXT("\\manga\\")))
			m_bFullScreenMode = true;
		else
			m_bFullScreenMode = false;
		}
	else
		m_bFullScreenMode = false;

	m_monitorRect = CMultiMonitorSupport::GetMonitorRect(0);	// Display rectangle of the monitor, use -1 for largest monitor, 0 for primary monitor or 1 to n for the secondary or other monitors
	m_clientRect = m_bFullScreenMode ? m_monitorRect : CMultiMonitorSupport::GetDefaultClientRectInWindowMode();

	// set icons (for toolbar)
	HICON hIcon = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
	SetIcon(hIcon, TRUE);
	HICON hIconSmall = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDR_MAINFRAME), IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
	SetIcon(hIconSmall, FALSE);

	// turn on/off mouse coursor
	m_bMouseOn = !m_bFullScreenMode;
	::ShowCursor(m_bMouseOn);

	// create thread pool for processing requests on multiple CPU cores
	CProcessingThreadPool::This().CreateThreadPoolThreads();

	// create JPEG provider and request first image - do no processing yet if not in fullscreen mode (as we do not know the size yet)
	m_pJPEGProvider = new CJPEGProvider(m_hWnd, NUM_THREADS, READ_AHEAD_BUFFERS);
	m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList, CJPEGProvider::FORWARD, m_pFileList->Current(), 0, CreateProcessParams(0), m_bOutOfMemoryLastImage, m_bExceptionErrorLastImage);

    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation())
        StartAnimation();

	m_nLastLoadError = GetLoadErrorAfterOpenFile();

	AfterNewImageLoaded();

	if (!m_bFullScreenMode)
		{
		// Window mode, set correct window size
		this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
		CRect windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
		this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
		}
	else
		{
		if (m_pCurrentImage != NULL)
			{
			CSize newSize = GetVirtualImageSize();
			CSize clippedSize(min(m_clientRect.Width(), newSize.cx), min(m_clientRect.Height(), newSize.cy));
			CPoint offsetsInImage = m_pCurrentImage->ConvertOffset(newSize, clippedSize, Helpers::LimitOffsets(m_offsets, m_clientRect.Size(), newSize));
			m_pCurrentImage->GetDIB(newSize, clippedSize, offsetsInImage, PFLAG_HighQualityResampling);
			}

		SetWindowLongW(GWL_STYLE, WS_VISIBLE);
		SetWindowPos(HWND_TOP, &m_monitorRect, SWP_NOZORDER);
		}

	m_bLockPaint = false;

	this->Invalidate(FALSE);
    //this->UpdateWindow();

	return TRUE;
	}

LRESULT CMainDlg::OnPaint(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	UINT t1 = 0;
	UINT t2 = 0;
	UINT t3 = 0;

/* Debugging */	TCHAR debugtext[512];

	if (m_bLockPaint)
		return 0;

	if (m_DynDwmFlush)
		m_DynDwmFlush();

	CPaintDC dc(m_hWnd);

	this->GetClientRect(&m_clientRect);
	CBrush backBrush;
	backBrush.CreateSolidBrush(RGB(0,0,0));

	if (m_pCurrentImage == NULL)
		{
		t2 = timeGetTime();

		dc.FillRect(&m_clientRect, backBrush);

		// Display errors and warnings

		HFONT hFont = CreateFont(15,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,ANSI_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,FF_DONTCARE,TEXT("TAHOMA"));
		dc.SelectFont(hFont);
		//dc.SelectStockFont(SYSTEM_FONT);
		dc.SetBkMode(TRANSPARENT);
		dc.SetTextColor(RGB(255,255,255));
		dc.SetBkColor(RGB(0,0,0));

		HelpersGUI::DrawImageLoadErrorText(dc, m_clientRect,
			(m_nLastLoadError == HelpersGUI::FileLoad_SlideShowListInvalid) ? m_sStartupFile :
			(m_nLastLoadError == HelpersGUI::FileLoad_NoFilesInDirectory) ? m_pFileList->CurrentDirectory() : CurrentFileName(),
			m_nLastLoadError,
			(m_bOutOfMemoryLastImage ? HelpersGUI::FileLoad_OutOfMemory : 0) | (m_bExceptionErrorLastImage ? HelpersGUI::FileLoad_ExceptionError : 0));
		
		DeleteObject(hFont);
		}
	else
		{
		// find out the new virtual image size and the size of the bitmap to request
		CSize newSize = GetVirtualImageSize();
		m_virtualImageSize = newSize;
		m_offsets = Helpers::LimitOffsets(m_offsets, m_clientRect.Size(), newSize);

		// Clip to client rectangle and request the DIB
		CSize clippedSize(min(m_clientRect.Width(), newSize.cx), min(m_clientRect.Height(), newSize.cy));
		CPoint offsetsInImage = m_pCurrentImage->ConvertOffset(newSize, clippedSize, m_offsets);

/* Debugging */	double t1 = Helpers::GetExactTickCount();

		void* pDIBData;
		pDIBData = m_pCurrentImage->GetDIB(newSize, clippedSize, offsetsInImage, (m_bTemporaryLowQ ? PFLAG_None : PFLAG_HighQualityResampling));

/* Debugging */	double t2 = Helpers::GetExactTickCount();

		// Paint the DIB
		if (pDIBData != NULL)
			{
			BITMAPINFO bmInfo;
			CPoint ptDIBStart = HelpersGUI::DrawDIB32bppWithBlackBorders(dc, bmInfo, pDIBData, backBrush, m_clientRect, clippedSize);
			}

/* Debugging */	double t3 = Helpers::GetExactTickCount();

/* Debugging */	_stprintf_s(debugtext, 256, _T("[JpegView] Loading: %.2f ms, Last op: %.2f ms, Last resize: %s, OnPaint GetDIB %d ms, OnPaint PaintDIB %d ms"), m_pCurrentImage->GetLoadTickCount(), m_pCurrentImage->LastOpTickCount(), CBasicProcessing::TimingInfo(), t2-t1, t3-t2);
//* Debugging */	::OutputDebugStringW(debugtext);
		}

	if (m_bInLowQTimer == true)
		StartLowQTimer(50);

	return 0;
	}

LRESULT CMainDlg::OnSize(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	if (wParam == 1)
		{
		// minimizing should be ignored
		return 0;
		}

	this->GetClientRect(&m_clientRect);
	m_nMouseX = m_nMouseY = -1;

	double dOldZoom = m_dZoom;
	m_dZoom = ConditionalZoomFactor();
	if (abs(m_dZoom - dOldZoom) > 0)
		{
		m_bInLowQTimer = true;
		StartLowQTimer(ZOOM_TIMEOUT);
		}

	this->Invalidate(FALSE);		// will cause an OnPaint() in which the offsets and zoom are recalculated before repainting
	//this->UpdateWindow();

	return 0;
	}

LRESULT CMainDlg::OnGetMinMaxInfo(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	if (m_pJPEGProvider != NULL)
		{
		MINMAXINFO* pMinMaxInfo = (MINMAXINFO*) lParam;
		pMinMaxInfo->ptMinTrackSize = CPoint(80,64);
		return 1;
		}

	return 0;
	}

LRESULT CMainDlg::OnAnotherInstanceStarted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	// Another instance has been started, terminate this one
	if (lParam == KEY_MAGIC && m_bFullScreenMode)
		{
		//SaveBookmark();
		this->EndDialog(0);
		}
	return 0;
	}

LRESULT CMainDlg::OnLoadFileAsynch(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled)
	{
    bHandled = lParam == KEY_MAGIC;
    if (lParam == KEY_MAGIC && ::IsWindowEnabled(m_hWnd))
		{
        StopMovieMode();
        StopAnimation();
	    MouseOn();
        if (m_sStartupFile.IsEmpty())
            OpenFile(false, false);
		else
            OpenFile(m_sStartupFile, false);
		}
    return 0;
	}

LRESULT CMainDlg::OnRefreshView(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& bHandled)
	{
	::OutputDebugStringW(TEXT("OnRefreshView()"));

	if (m_pFileList != NULL && m_pFileList->CurrentFileExists())
		{
		m_pFileList->Reload(NULL);
		this->Invalidate(FALSE);
		//this->UpdateWindow();
		}

	if (m_pCurrentImage != NULL)
		GotoImage(POS_Current);

    return 0;
	}

LRESULT CMainDlg::OnCopyData(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	COPYDATASTRUCT *pcds = NULL;
	pcds = (COPYDATASTRUCT *)lParam;
	unsigned int cbData;
	cbData = pcds->cbData;
	if (cbData > 1024)
		return 0;
	
	TCHAR lpData[1024];
	_tcscpy(lpData,(TCHAR *)pcds->lpData);
	if(lpData != NULL)
		{
		if (::PathFileExists(lpData))
			{
			TCHAR *pExt = NULL;						// TCHAR = WCHAR on unicode
			pExt = PathFindExtension(lpData);		// input: PTSTR, output: PTSTR
			TCHAR buffer1[1024] = TEXT("");
			wsprintf(buffer1,TEXT("|%s|"),pExt);
			if (StrStrI(TEXT("|.bmp|.jpg|.jpeg|.png|.gif|.tif|.tiff|.webp|"),buffer1) != NULL)
				{
				this->ShowWindow(SW_RESTORE);	// disabled since doing SW_SHOWNOACTIVATE from script
				OpenFile(lpData,false);
				SetForegroundWindow(m_hWnd);	// disabled since doing SW_SHOWNOACTIVATE from script
				}
			}
		}
	return 0;
	}

LRESULT CMainDlg::OnLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	GotoImage(POS_Previous);
	return 0;
	}

LRESULT CMainDlg::OnMButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	ExecuteCommand(IDM_FULL_SCREEN_MODE);
	return 0;
	}

LRESULT CMainDlg::OnMButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	return 0;
	}

LRESULT CMainDlg::OnXButtonDown(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	return 0;
	}

LRESULT CMainDlg::OnMouseMove(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	bool bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

	if (bCtrl)
		MouseOn();
	
	// Turn mouse pointer on when mouse has moved some distance
	int nOldMouseY = m_nMouseY;
	int nOldMouseX = m_nMouseX;
	m_nMouseX = GET_X_LPARAM(lParam);
	m_nMouseY = GET_Y_LPARAM(lParam);

	if (m_startMouse.x == -1 && m_startMouse.y == -1)
		{
		m_startMouse.x = m_nMouseX;
		m_startMouse.y = m_nMouseY;
		}

	return 0;
	}

LRESULT CMainDlg::OnMouseWheel(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	bool bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	int nDelta = GET_WHEEL_DELTA_WPARAM(wParam);

	if (bCtrl)
		{
		MouseOn();
		PerformZoom(double(nDelta)/WHEEL_DELTA, m_bMouseOn);
		}
	else
		{
		if (m_pCurrentImage != NULL)
			{
			if (nDelta < 0)
				{
				if (m_bMangaMode == true)
					{
					if (PerformPan(0, -PAN_STEP, false) == true)	// m_virtualImageSize.cy/25
						{
						this->Invalidate(FALSE);
						//this->UpdateWindow();
						}
					}
				else
					{
					unsigned int iRealHeight = unsigned int (m_dZoom * (m_pCurrentImage->OrigHeight()));
					if (iRealHeight > m_clientRect.Height())
						{
						if (PerformPan(0, -PAN_STEP, false) == true)	// m_virtualImageSize.cy/25
							{
							this->Invalidate(FALSE);
							//this->UpdateWindow();
							}
						}
					else
						{
						GotoImage(POS_Next);
						}
					}
				}
			else if (nDelta > 0)
				{
				if (m_bMangaMode == true)
					{
					if (PerformPan(0, PAN_STEP, false) == true)
						{
						this->Invalidate(FALSE);
						//this->UpdateWindow();
						}
					}
				else
					{
					unsigned int iRealHeight = unsigned int (m_dZoom * (m_pCurrentImage->OrigHeight()));
					if (iRealHeight > m_clientRect.Height())
						{
						if (PerformPan(0, PAN_STEP, false) == true)
							{
							this->Invalidate(FALSE);
							//this->UpdateWindow();
							}
						}
					else
						{
						GotoImage(POS_Previous);
						}
					}
				}
			}
		}
	return 0;
	}

LRESULT CMainDlg::OnKeyDown(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/)
	{
	bool bCtrl = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
	bool bShift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
	bool bAlt = (::GetKeyState(VK_MENU) & 0x8000) != 0;

	if (wParam == VK_ESCAPE)
		{
		StopMovieMode();
		StopAnimation();

		//SaveBookmark();
		CleanupAndTeminate();
		}
	else if (wParam == VK_DELETE)
		{
		DeleteImageShown();
		}
	else if (wParam == VK_PAGE_UP)
		{
		if (PerformPan(0,+(m_clientRect.Height()),false) == true)
			{
			this->Invalidate(FALSE);
			//this->UpdateWindow();
			}
		}
	else if (wParam == VK_PAGE_DOWN)
		{
		if (PerformPan(0,-(m_clientRect.Height()),false) == true)
			{
			this->Invalidate(FALSE);
			//this->UpdateWindow();
			}
		}
	else if (wParam == VK_SPACE)
		{
		GotoImage(POS_Next);
		}
	else if (wParam == VK_BACK)
		{
		GotoImage(POS_Previous);
		}
	else if (wParam == VK_HOME)
		{
		GotoImage(POS_First);
		}
	else if (wParam == VK_END)
		{
		GotoImage(POS_Last);
		}
	else if (wParam == VK_PLUS)
		{
		PerformZoom(1, m_bMouseOn);
		}
	else if (wParam == VK_MINUS)
		{
		PerformZoom(-1, m_bMouseOn);
		}
	else if (wParam == VK_RETURN)
		{
		ExecuteCommand(IDM_FULL_SCREEN_MODE);
		}
	else if (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT) 
		{
		if (lParam & 0xc0000000)
			return 1;

		if (m_pCurrentImage != NULL)
			{
			INT iRealHeight = INT (m_dZoom * (m_pCurrentImage->OrigHeight()));
			INT iRealWidth = INT (m_dZoom * (m_pCurrentImage->OrigWidth()));

			if (((iRealHeight > m_clientRect.Height()) && (wParam == VK_UP || wParam == VK_DOWN)) || ((iRealWidth > m_clientRect.Width()) && (wParam == VK_LEFT || wParam == VK_RIGHT)))
				{
				//* Debugging */	::OutputDebugStringW(TEXT("Here!!"));
				
				if (m_bPanTimerActive == false)
					{
					m_bPanTimerActive = true;
					::SetTimer(this->m_hWnd, PAN_TIMER_EVENT_ID, 10, NULL);
					}
				}
			else
				{
				if (m_bMangaMode == false)
					{
					if (wParam == VK_RIGHT)
						GotoImage(POS_Next);
					else if (wParam == VK_LEFT)
						GotoImage(POS_Previous);
					}
				else
					{
					if (iRealHeight > m_clientRect.Height())
						{
						if (m_bPanTimerActive == false)
							{
							if ((m_offsets.y >= ((iRealHeight-m_clientRect.Height())/2)) && (wParam == VK_LEFT))	// Upper Border!
								GotoImage(POS_Previous);
							else if ((-m_offsets.y >= ((iRealHeight-m_clientRect.Height())/2)) && (wParam == VK_RIGHT))	//Lower Border!
								GotoImage(POS_Next);
							else
								{
								m_bPanTimerActive = true;
								::SetTimer(this->m_hWnd, PAN_TIMER_EVENT_ID, 10, NULL);
								}
							}
						}
					else
						{
						if (m_bPanTimerActive == false)
							{
							if (wParam == VK_RIGHT)
								GotoImage(POS_Next);
							else if (wParam == VK_LEFT)
								GotoImage(POS_Previous);
							}
						}
					}
				}

			return 1;
			}
		}
	else if (wParam == VK_F1)
		{
		::MessageBox(CMainDlg::m_hWnd,TEXT("F1\t\tHotkey Help\nF5\t\tReload Page\n\nEsc\t\tClose Viewer\nDel\t\tDelete Page\n\nHome\t\tFirst Page\nEnd\t\tLast Page\n\nPage Up\t\tPrevious Page\nPage Down\tNext Page\n\nBackspace\tPrevious Page\nSpace\t\tNext Page\n\nUp\t\tPan Up / Previous Page\nDown\t\tPan Down / Next Page\nLeft\t\tPan Left / Previous Page\nRight\t\tPan Right / Next Page\n\n+\t\tZoom In\n-\t\tZoom Out\nReturn\t\tFullScreen/Window\nA\t\tSwitch Language\nCtrl+E\t\tEdit Page\nF\t\tSize\nCtrl+G\t\tEdit Page (Gimp)\nH\t\tHorizontal Mirroring\nI\t\tPage Info\nL\t\tRotate left\nCtrl+L\t\tOpen Folder\nM\t\tMinimize Viewer\nR\t\tRotate right\nV\t\tVertical Mirroring\nZ\t\tSwitch Sorting Normal/Random"),TEXT("ImageViewer Hotkeys"),MB_OK | MB_TASKMODAL);
		}
	else if (wParam == VK_F5)
		{
		if (m_pFileList != NULL && m_pFileList->CurrentFileExists())
			{
			m_pFileList->Reload(NULL);
			this->Invalidate(FALSE);
			//this->UpdateWindow();
			}

		if (m_pCurrentImage != NULL)
			GotoImage(POS_Current);
		}
	else if (wParam == '1')
		{
		if (m_dZoom != 0.5)
			{
			m_dZoom = 0.5;
			m_bInLowQTimer = true;
			StartLowQTimer(ZOOM_TIMEOUT);
			}

		this->Invalidate(FALSE);		// will cause an OnPaint() in which the offsets and zoom are recalculated before repainting
		//this->UpdateWindow();

		/*
		LPCTSTR sCurrentFileName = CurrentFileName();
		if (sCurrentFileName != NULL)
			{
			TCHAR sFullPath[MAX_PATH];
			lstrcpy(sFullPath,sCurrentFileName);
			
			TCHAR sFNameNoExt[MAX_PATH];
			_wsplitpath(sFullPath,NULL,NULL,sFNameNoExt,NULL);

			TCHAR sParentDir[MAX_PATH];
			lstrcpy(sParentDir,sFullPath);
			PathRemoveFileSpec(sParentDir);

			TCHAR sParentDir2[MAX_PATH];
			lstrcpy(sParentDir2,sParentDir);
			PathRemoveFileSpec(sParentDir2);

			TCHAR sParentDirName[MAX_PATH];
			lstrcpy(sParentDirName,sParentDir);
			PathStripPath(sParentDirName);

			TCHAR sAltPath[MAX_PATH] = TEXT("");
			if (StrStrI(sParentDirName,TEXT(" [en2]")) != NULL)
				{
				CString sAltParentDirNameC = ReplaceNoCase(sParentDirName,TEXT(" [en2]"),TEXT(""));
				wsprintf(sAltPath,TEXT("%s\\%s\\%s"),sParentDir2,(LPCTSTR)sAltParentDirNameC,sFNameNoExt);
				}
			else if (StrStrI(sParentDirName,TEXT(" [en1]")) != NULL)
				{
				CString sAltParentDirNameC = ReplaceNoCase(sParentDirName,TEXT(" [en1]"),TEXT(""));
				wsprintf(sAltPath,TEXT("%s\\%s\\%s"),sParentDir2,(LPCTSTR)sAltParentDirNameC,sFNameNoExt);
				}
			else
				{
				return 1;
				}

			TCHAR sTargetPath[MAX_PATH] = TEXT("");

			wsprintf(sTargetPath,TEXT("%s.jpg"),sAltPath);
			if (::PathFileExists(sTargetPath))
				OpenFile(sTargetPath,false);
			else
				{
				wsprintf(sTargetPath,TEXT("%s.png"),sAltPath);
				if (::PathFileExists(sTargetPath))
					OpenFile(sTargetPath,false);
				else
					{
					wsprintf(sTargetPath,TEXT("%s.jpeg"),sAltPath);
					if (::PathFileExists(sTargetPath))
						OpenFile(sTargetPath,false);
					else
						{
						wsprintf(sTargetPath,TEXT("%s.bmp"),sAltPath);
						if (::PathFileExists(sTargetPath))
							OpenFile(sTargetPath,false);
						}
					}
				}
			}
		*/
		}
	else if (wParam == '2')
		{
		if (m_dZoom != 0.25)
			{
			m_dZoom = 0.25;
			m_bInLowQTimer = true;
			StartLowQTimer(ZOOM_TIMEOUT);
			}

		this->Invalidate(FALSE);		// will cause an OnPaint() in which the offsets and zoom are recalculated before repainting
		//this->UpdateWindow();

		/*
		LPCTSTR sCurrentFileName = CurrentFileName();
		if (sCurrentFileName != NULL)
			{
			TCHAR sFullPath[MAX_PATH];
			lstrcpy(sFullPath,sCurrentFileName);
			
			TCHAR sFNameNoExt[MAX_PATH];
			_wsplitpath(sFullPath,NULL,NULL,sFNameNoExt,NULL);

			TCHAR sParentDir[MAX_PATH];
			lstrcpy(sParentDir,sFullPath);
			PathRemoveFileSpec(sParentDir);

			TCHAR sParentDir2[MAX_PATH];
			lstrcpy(sParentDir2,sParentDir);
			PathRemoveFileSpec(sParentDir2);

			TCHAR sParentDirName[MAX_PATH];
			lstrcpy(sParentDirName,sParentDir);
			PathStripPath(sParentDirName);

			TCHAR sAltPath[MAX_PATH] = TEXT("");
			if (StrStrI(sParentDirName,TEXT(" [en1]")) != NULL)
				{
				return 1;
				}
			else if (StrStrI(sParentDirName,TEXT(" [en2]")) != NULL)
				{
				CString sAltParentDirNameC = ReplaceNoCase(sParentDirName,TEXT(" [en2]"),TEXT(" [en1]"));
				wsprintf(sAltPath,TEXT("%s\\%s\\%s"),sParentDir2,(LPCTSTR)sAltParentDirNameC,sFNameNoExt);
				}
			else
				{
				wsprintf(sAltPath,TEXT("%s\\%s [en1]\\%s"),sParentDir2,sParentDirName,sFNameNoExt);
				}

			TCHAR sTargetPath[MAX_PATH] = TEXT("");

			wsprintf(sTargetPath,TEXT("%s.jpg"),sAltPath);
			if (::PathFileExists(sTargetPath))
				OpenFile(sTargetPath,false);
			else
				{
				wsprintf(sTargetPath,TEXT("%s.png"),sAltPath);
				if (::PathFileExists(sTargetPath))
					OpenFile(sTargetPath,false);
				else
					{
					wsprintf(sTargetPath,TEXT("%s.jpeg"),sAltPath);
					if (::PathFileExists(sTargetPath))
						OpenFile(sTargetPath,false);
					else
						{
						wsprintf(sTargetPath,TEXT("%s.bmp"),sAltPath);
						if (::PathFileExists(sTargetPath))
							OpenFile(sTargetPath,false);
						}
					}
				}
			}
		*/
		}
	else if (wParam == '3')
		{
		
		/*
		LPCTSTR sCurrentFileName = CurrentFileName();
		if (sCurrentFileName != NULL)
			{
			TCHAR sFullPath[MAX_PATH];
			lstrcpy(sFullPath,sCurrentFileName);
			
			TCHAR sFNameNoExt[MAX_PATH];
			_wsplitpath(sFullPath,NULL,NULL,sFNameNoExt,NULL);

			TCHAR sParentDir[MAX_PATH];
			lstrcpy(sParentDir,sFullPath);
			PathRemoveFileSpec(sParentDir);

			TCHAR sParentDir2[MAX_PATH];
			lstrcpy(sParentDir2,sParentDir);
			PathRemoveFileSpec(sParentDir2);

			TCHAR sParentDirName[MAX_PATH];
			lstrcpy(sParentDirName,sParentDir);
			PathStripPath(sParentDirName);

			TCHAR sAltPath[MAX_PATH] = TEXT("");
			if (StrStrI(sParentDirName,TEXT(" [en2]")) != NULL)
				{
				return 1;
				}
			else if (StrStrI(sParentDirName,TEXT(" [en1]")) != NULL)
				{
				CString sAltParentDirNameC = ReplaceNoCase(sParentDirName,TEXT(" [en1]"),TEXT(" [en2]"));
				wsprintf(sAltPath,TEXT("%s\\%s\\%s"),sParentDir2, (LPCTSTR)sAltParentDirNameC,sFNameNoExt);
				}
			else
				{
				wsprintf(sAltPath,TEXT("%s\\%s [en2]\\%s"),sParentDir2,sParentDirName,sFNameNoExt);
				}

			TCHAR sTargetPath[MAX_PATH] = TEXT("");

			wsprintf(sTargetPath,TEXT("%s.jpg"),sAltPath);
			if (::PathFileExists(sTargetPath))
				OpenFile(sTargetPath,false);
			else
				{
				wsprintf(sTargetPath,TEXT("%s.png"),sAltPath);
				if (::PathFileExists(sTargetPath))
					OpenFile(sTargetPath,false);
				else
					{
					wsprintf(sTargetPath,TEXT("%s.jpeg"),sAltPath);
					if (::PathFileExists(sTargetPath))
						OpenFile(sTargetPath,false);
					else
						{
						wsprintf(sTargetPath,TEXT("%s.bmp"),sAltPath);
						if (::PathFileExists(sTargetPath))
							OpenFile(sTargetPath,false);
						}
					}
				}
			}
		*/
		}
	else if (wParam == 'A')
		{
		LPCTSTR sCurrentFileName = CurrentFileName();
		if (sCurrentFileName != NULL)
			{
			TCHAR sFullPath[MAX_PATH];
			lstrcpy(sFullPath,sCurrentFileName);
			
			TCHAR sFNameNoExt[MAX_PATH];
			_wsplitpath(sFullPath,NULL,NULL,sFNameNoExt,NULL);

			TCHAR sParentDir[MAX_PATH];
			lstrcpy(sParentDir,sFullPath);
			PathRemoveFileSpec(sParentDir);

			TCHAR sParentDir2[MAX_PATH];
			lstrcpy(sParentDir2,sParentDir);
			PathRemoveFileSpec(sParentDir2);

			TCHAR sParentDirName[MAX_PATH];
			lstrcpy(sParentDirName,sParentDir);
			PathStripPath(sParentDirName);

			TCHAR sTestFile[MAX_PATH] = TEXT("");
			TCHAR sTestPath[MAX_PATH] = TEXT("");
			TCHAR sTargetFile[MAX_PATH] = TEXT("");
			BOOL bMatchFound=FALSE;

			std::wstring wexpr = TEXT("^(.+)\\s+\\[(en|de|ch|ko)\\]$");
			std::wregex we(wexpr);
			std::wsmatch wsMatch;
			std::wstring wtest = sParentDirName;
			if(std::regex_search(wtest, wsMatch, we))
				{
				// wsMatch.str(1).c_str()	contains the release title without language tag
				// wsMatch.str(2).c_str()	contains the language tag without brackets
				
				
				if (lstrcmpi(wsMatch.str(2).c_str(),TEXT("en")) == 0)
					{
					//::OutputDebugStringW(TEXT("A1a"));
					wsprintf(sTestPath,TEXT("%s\\%s [de]"),sParentDir2,wsMatch.str(1).c_str());
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("A1b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}

				if ((bMatchFound==FALSE) || (lstrcmpi(wsMatch.str(2).c_str(),TEXT("de")) == 0))
					{
					//::OutputDebugStringW(TEXT("A2a"));
					wsprintf(sTestPath,TEXT("%s\\%s [ch]"),sParentDir2,wsMatch.str(1).c_str());
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("A2b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}

				if ((bMatchFound==FALSE) || (lstrcmpi(wsMatch.str(2).c_str(),TEXT("ch")) == 0))
					{
					//::OutputDebugStringW(TEXT("A3a"));
					wsprintf(sTestPath,TEXT("%s\\%s [ko]"),sParentDir2,wsMatch.str(1).c_str());
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("A3b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}

				if ((bMatchFound==FALSE) || (lstrcmpi(wsMatch.str(2).c_str(),TEXT("ko")) == 0))
					{
					//::OutputDebugStringW(TEXT("A4a"));
					wsprintf(sTestPath,TEXT("%s\\%s"),sParentDir2,wsMatch.str(1).c_str());
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("A4b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}

				if (bMatchFound==FALSE)
					{
					//::OutputDebugStringW(TEXT("A5a"));
					wsprintf(sTestPath,TEXT("%s\\%s [en]"),sParentDir2,wsMatch.str(1).c_str());
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("A5b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}
				}
			else
				{
				//::OutputDebugStringW(TEXT("B1a"));
				wsprintf(sTestPath,TEXT("%s\\%s [en]"),sParentDir2,sParentDirName);
				if (::PathFileExists(sTestPath))
					{
					//::OutputDebugStringW(TEXT("B1b"));
					wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
					bMatchFound = TRUE;
					}
				
				if (bMatchFound==FALSE)
					{
					//::OutputDebugStringW(TEXT("B2a"));
					wsprintf(sTestPath,TEXT("%s\\%s [de]"),sParentDir2,sParentDirName);
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("B2b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}

				if (bMatchFound==FALSE)
					{
					//::OutputDebugStringW(TEXT("B3a"));
					wsprintf(sTestPath,TEXT("%s\\%s [ch]"),sParentDir2,sParentDirName);
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("B3b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}

				if (bMatchFound==FALSE)
					{
					//::OutputDebugStringW(TEXT("B4a"));
					wsprintf(sTestPath,TEXT("%s\\%s [ko]"),sParentDir2,sParentDirName);
					if (::PathFileExists(sTestPath))
						{
						//::OutputDebugStringW(TEXT("B4b"));
						wsprintf(sTestFile,TEXT("%s\\%s"),sTestPath,sFNameNoExt);
						bMatchFound = TRUE;
						}
					}
				}

			if (bMatchFound==TRUE)
				{
				//::OutputDebugStringW(TEXT("C1"));

				wsprintf(sTargetFile,TEXT("%s.jpg"),sTestFile);
				if (::PathFileExists(sTargetFile))
					OpenFile(sTargetFile,false);
				else
					{
					wsprintf(sTargetFile,TEXT("%s.png"),sTestFile);
					if (::PathFileExists(sTargetFile))
						OpenFile(sTargetFile,false);
					else
						{
						wsprintf(sTargetFile,TEXT("%s.jpeg"),sTestFile);
						if (::PathFileExists(sTargetFile))
							OpenFile(sTargetFile,false);
						else
							{
							wsprintf(sTargetFile,TEXT("%s.bmp"),sTestFile);
							if (::PathFileExists(sTargetFile))
								OpenFile(sTargetFile,false);
							}
						}
					}
				}
			}
		}
	else if (bCtrl && wParam == 'C')
		{
		ExecuteCommand(IDM_COPY);
		}
	else if (bCtrl && wParam == 'E')
		{
		LPCTSTR sCurrentFileName = CurrentFileName();
		if (sCurrentFileName != NULL)
			{
			if (m_bFullScreenMode)
				{
				m_bFullScreenMode = !m_bFullScreenMode;
				CRect windowRect;
				this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
				if (::IsRectEmpty(&(m_storedWindowPlacement2.rcNormalPosition)))
					{
					// never set to window mode before, use default position
					windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
					this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
					}
				else
					{
					m_storedWindowPlacement2.flags = this->SetWindowPlacement(&m_storedWindowPlacement2);
					}
				this->MouseOn();

				m_dZoom = -1;
				m_bInLowQTimer = true;
				StartLowQTimer(ZOOM_TIMEOUT);
				this->SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);
				}
			::ShellExecute(m_hWnd,_T("edit"),sCurrentFileName,NULL,NULL,SW_SHOW);
			}
		}
	else if (wParam == 'F')
		{
		double dOldZoom = m_dZoom;

		m_bZoomMode = !m_bZoomMode;
		m_dZoom = ConditionalZoomFactor();
		
		if ((m_offsets.x != 0) || (m_offsets.y != 0))
			{
			m_offsets_custom.x = m_offsets.x;
			m_offsets_custom.y = m_offsets.y;
			m_offsets = CPoint(0, 0);
			}
		else if ((m_offsets_custom.x != 0) || (m_offsets_custom.y != 0))
			{
			m_offsets.x = m_offsets_custom.x;
			m_offsets.y = m_offsets_custom.y;
			}

		if (abs(m_dZoom - dOldZoom) > 0)
			{
			m_bInLowQTimer = true;
			StartLowQTimer(ZOOM_TIMEOUT);

			this->Invalidate(FALSE);
			//this->UpdateWindow();
			}
		}
	else if (bCtrl && wParam == 'G')
		{
		LPCTSTR sCurrentFileName = CurrentFileName();
		if (sCurrentFileName != NULL)
			{
			TCHAR AppPath[MAX_PATH];
			::GetModuleFileName(NULL,AppPath,MAX_PATH);
			PathRemoveFileSpec(AppPath);

			TCHAR AHKpath[MAX_PATH];
			if (Is64BitOS())
				PathCombine(AHKpath,AppPath,TEXT("..\\AHK\\AutoHotkey_x64.exe"));
			else
				PathCombine(AHKpath,AppPath,TEXT("..\\AHK\\AutoHotkey_x32.exe"));

			TCHAR ScriptPath[MAX_PATH];
			PathCombine(ScriptPath,AppPath,TEXT("..\\_Links\\D\\Gimp [Image Editor].vlg"));

			if ((::PathFileExists(AHKpath))&&(::PathFileExists(ScriptPath)))
				{
				if (m_bFullScreenMode)
					{
					m_bFullScreenMode = !m_bFullScreenMode;
					CRect windowRect;
					this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
					if (::IsRectEmpty(&(m_storedWindowPlacement2.rcNormalPosition)))
						{
						// never set to window mode before, use default position
						windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
						this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
						}
					else
						{
						m_storedWindowPlacement2.flags = this->SetWindowPlacement(&m_storedWindowPlacement2);
						}
					this->MouseOn();

					m_dZoom = -1;
					m_bInLowQTimer = true;
					StartLowQTimer(ZOOM_TIMEOUT);
					this->SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);
					}

				TCHAR sParameters[1024];
				wsprintf(sParameters,TEXT("\"%s\" \"%s\""),ScriptPath,sCurrentFileName);
				::ShellExecute(m_hWnd,_T("open"),AHKpath,sParameters,NULL,SW_SHOW);
				}
			}
		}
	else if (wParam == 'H')
		{
		ExecuteCommand(IDM_MIRROR_H);
		}
	else if (wParam == 'I')
		{
		if (m_pCurrentImage != NULL)
			{
			unsigned int iRealWidth = unsigned int (m_dZoom * (m_pCurrentImage->OrigWidth()));
			unsigned int iRealHeight = unsigned int (m_dZoom * (m_pCurrentImage->OrigHeight()));

			LPCTSTR sFullPath = CurrentFileName();
			if (sFullPath != NULL)
				{
				UINT nFileSize = (UINT)Helpers::GetFileSize(sFullPath);

				TCHAR sParentDir[MAX_PATH];
				lstrcpy(sParentDir,sFullPath);
				PathRemoveFileSpec(sParentDir);

				TCHAR sParentDirName[MAX_PATH];
				lstrcpy(sParentDirName,sParentDir);
				PathStripPath(sParentDirName);

				TCHAR sFName[MAX_PATH];
				lstrcpy(sFName,sFullPath);
				PathStripPath(sFName);

				TCHAR buff[1024];
				_stprintf_s(buff,1024,_T("%s\\%s\n\nFile Size:\t\t%d Bytes\nImage Size:\t%dx%d\nZoomed Size:\t%dx%d\t(Zoom Factor: %f)\nWindow Size:\t%dx%d"),sParentDirName,sFName,nFileSize,m_pCurrentImage->OrigWidth(),m_pCurrentImage->OrigHeight(),iRealWidth,iRealHeight,m_dZoom,m_clientRect.Width(),m_clientRect.Height());
				::MessageBox(CMainDlg::m_hWnd,buff,TEXT("Image Info"),MB_OK | MB_TASKMODAL);
				}
			else
				{
				TCHAR buff[1024];
				_stprintf_s(buff,1024,_T("Image Size:\t%dx%d\nZoomed Size:\t%dx%d\t(Zoom Factor: %f)\nWindow Size:\t%dx%d"),m_pCurrentImage->OrigWidth(),m_pCurrentImage->OrigHeight(),iRealWidth,iRealHeight,m_dZoom,m_clientRect.Width(),m_clientRect.Height());
				::MessageBox(CMainDlg::m_hWnd,buff,TEXT("Image Info"),MB_OK | MB_TASKMODAL);
				}
			}
		}
	else if (wParam == 'L')
		{
		if (bCtrl == true)
			{
			LPCTSTR sCurrentFileName = CurrentFileName();
			if (sCurrentFileName != NULL)
				{
				if (m_bFullScreenMode)
					{
					m_bFullScreenMode = !m_bFullScreenMode;
					CRect windowRect;
					this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
					if (::IsRectEmpty(&(m_storedWindowPlacement2.rcNormalPosition)))
						{
						// never set to window mode before, use default position
						windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
						this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
						}
					else
						{
						m_storedWindowPlacement2.flags = this->SetWindowPlacement(&m_storedWindowPlacement2);
						}
					this->MouseOn();

					m_dZoom = -1;
					m_bInLowQTimer = true;
					StartLowQTimer(ZOOM_TIMEOUT);
					this->SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);
					}

				TCHAR sParentDir[MAX_PATH];
				lstrcpy(sParentDir,sCurrentFileName);
				PathRemoveFileSpec(sParentDir);

				TCHAR sFName[MAX_PATH];
				lstrcpy(sFName,sCurrentFileName);
				PathStripPath(sFName);

				HWND hWnd_KeyLaunch;
				hWnd_KeyLaunch = FindWindow(TEXT("AutoHotkey"),TEXT("GF_Hotkeys"));
				if (hWnd_KeyLaunch != NULL)
					{
					TCHAR StringToSend[MAX_PATH];
					wsprintf(StringToSend,TEXT("ExplorerSwitch|%s|%s"),sParentDir,sFName);
					COPYDATASTRUCT cds;
					cds.cbData	= static_cast<DWORD>((wcslen(StringToSend) + 1) * sizeof(TCHAR));
					cds.lpData	= StringToSend;
					::SendMessage(hWnd_KeyLaunch,WM_COPYDATA,NULL,reinterpret_cast<LPARAM>(&cds));
					}
				else
					ShellExecute(m_hWnd,_T("open"),sParentDir,NULL,NULL,SW_SHOWNORMAL);
				}
			}
		else
			{
			ExecuteCommand(IDM_ROTATE_270);
			}
		}
	else if (wParam == 'M')
		{
		ExecuteCommand(IDM_MINIMIZE);
		}
	else if (wParam == 'R')
		{
		ExecuteCommand(IDM_ROTATE_90);
		}
	else if (wParam == 'V')
		{
		ExecuteCommand(IDM_MIRROR_V);
		}
	else if (wParam == 'Z')
		{
		if (m_pFileList != NULL)
			{
			if (m_pFileList->GetSorting() == Helpers::FS_FileName)
				m_pFileList->SetSorting(Helpers::FS_Random, m_pFileList->IsSortedUpcounting());
			else if (m_pFileList->GetSorting() == Helpers::FS_Random)
				m_pFileList->SetSorting(Helpers::FS_FileName, m_pFileList->IsSortedUpcounting());

			m_pFileList->Reload(NULL);
			this->Invalidate(FALSE);
			//this->UpdateWindow();
			}
		}
	
	return 1;
	}

LRESULT CMainDlg::OnGetDlgCode(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	// need to request key messages, else the dialog proc eats them all up
	return DLGC_WANTALLKEYS;
	}

LRESULT CMainDlg::OnImageLoadCompleted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	// route to JPEG provider
	m_pJPEGProvider->OnImageLoadCompleted((int)lParam);
	return 0;
	}

LRESULT CMainDlg::OnDisplayedFileChangedOnDisk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged() && m_pCurrentImage != NULL && !m_pCurrentImage->IsClipboardImage() &&
		m_pFileList != NULL && m_pFileList->CanOpenCurrentFileForReading()) {
		ExecuteCommand(IDM_RELOAD);
	}
	return 0;
}

LRESULT CMainDlg::OnActiveDirectoryFilelistChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/) {
	if (CSettingsProvider::This().ReloadWhenDisplayedImageChanged() && m_pFileList != NULL && m_pFileList->CurrentFileExists()) {
		m_pFileList->Reload(NULL, false);
		Invalidate(FALSE);
	}
	return 0;
}

LRESULT CMainDlg::OnDropFiles(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	HDROP hDrop = (HDROP) wParam;

	if (hDrop != NULL)
		{
		const int BUFF_SIZE = 512;
		TCHAR buff[BUFF_SIZE];

		if (::DragQueryFile(hDrop, 0, (LPTSTR) &buff, BUFF_SIZE - 1) > 0)
			{
			if (::GetFileAttributes(buff) & FILE_ATTRIBUTE_DIRECTORY)
				{
				_tcsncat_s(buff, BUFF_SIZE, _T("\\"), BUFF_SIZE);
				}

			OpenFile(buff, false);
			}

		::DragFinish(hDrop);
		}
	return 0;
	}

LRESULT CMainDlg::OnTimer(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	if (wParam == ANIMATION_TIMER_EVENT_ID || (wParam == SLIDESHOW_TIMER_EVENT_ID && m_nCurrentTimeout > 0))
		{
		// Remove all timer messages for slideshow and animation events that accumulated in the queue
		MSG msg;
		while (::PeekMessage(&msg, this->m_hWnd, WM_TIMER, WM_TIMER, PM_REMOVE))
			{
			if (msg.wParam != SLIDESHOW_TIMER_EVENT_ID && msg.wParam != ANIMATION_TIMER_EVENT_ID)
				{
				BOOL bNotUsed;
				OnTimer(WM_TIMER, msg.wParam, msg.lParam, bNotUsed);
				}
            if (msg.wParam == SLIDESHOW_TIMER_EVENT_ID && wParam == ANIMATION_TIMER_EVENT_ID)
				{
                // if there are queued slideshow timer events and we process an animation event, the slideshow event has preceedence
                wParam = SLIDESHOW_TIMER_EVENT_ID;
				}
			}

		// Goto next image if no other messages to process are pending
		if (!::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_NOREMOVE))
			{
            int nRealDisplayTimeMs = ::GetTickCount() - m_nLastSlideShowImageTickCount;
            if (m_nCurrentTimeout > 250 && wParam == SLIDESHOW_TIMER_EVENT_ID)
				{
                if (m_nCurrentTimeout - nRealDisplayTimeMs > 100)
					{
                    // restart timer
                    ::Sleep(m_nCurrentTimeout - nRealDisplayTimeMs);
                    ::KillTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID);
                    ::SetTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID, m_nCurrentTimeout, NULL);
					}
				}

			GotoImage((wParam == ANIMATION_TIMER_EVENT_ID) ? POS_NextAnimation : POS_NextSlideShow, NO_REMOVE_KEY_MSG);

            //if (wParam == SLIDESHOW_TIMER_EVENT_ID && UseSlideShowTransitionEffect())
            //    AnimateTransition();

            if (wParam != ANIMATION_TIMER_EVENT_ID)
                m_nLastSlideShowImageTickCount = ::GetTickCount();
			}
		}
	else if (wParam == ZOOM_TIMER_EVENT_ID)
		{
		::KillTimer(this->m_hWnd, ZOOM_TIMER_EVENT_ID);
		if (m_bTemporaryLowQ || m_bInLowQTimer)
			{
			m_bTemporaryLowQ = false;
			m_bInLowQTimer = false;
			if (m_bHQResampling && m_pCurrentImage != NULL)
				{
				this->Invalidate(FALSE);
				//this->UpdateWindow();
				}
			}
		}
	else if (wParam == PAN_TIMER_EVENT_ID)
		{
		if (m_pCurrentImage != NULL)
			{
			::KillTimer(this->m_hWnd, PAN_TIMER_EVENT_ID);
			::timeBeginPeriod(1);

			bool bUp = 0;
			bool bDown = 0;
			bool bLeft = 0;
			bool bRight = 0;

			static int PanXbase = 0;
			static int PanYbase = 0;

			INT BorderDistanceY = 0;
			int PanX = 0;
			int PanY = 0;
			INT iRealHeight = 0;
			INT iRealWidth = 0;
			bool DoStopPan = 1;

			if (m_DynDwmFlush)
				{
				while (m_bPanTimerActive == true)
					{
					bUp = (::GetAsyncKeyState(VK_UP) & 0x8000) != 0;
					bDown = (::GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
					bLeft = (::GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
					bRight = (::GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

					if ((bUp == false) && (bDown == false) && (bLeft == false) && (bRight == false))
						{
						::timeEndPeriod(1);
						m_bPanTimerActive = false;
						break;

						/*
						DoStopPan = 1;

						//if (m_offsets.y>0)
						//	BorderDistanceY = ((iRealHeight-m_clientRect.Height())/2)-m_offsets.y;	// Top
						//else
						//	BorderDistanceY = ((iRealHeight-m_clientRect.Height())/2)+m_offsets.y;	// Bottom
						
						while((PanXbase!=0)||(PanYbase!=0))
							{
							if (PanXbase>0)
								PanXbase--;
							else if (PanXbase<0)
								PanXbase++;

							if (PanYbase>0)
								PanYbase--;
							else if (PanYbase<0)
								PanYbase++;

							if (PerformPan(PanXbase,PanYbase,false) == true)
								{
								this->UpdateWindow();

								MSG msg;
								while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));

								Sleep(1);
								}
							else
								{
								MSG msg;
								while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));

								Sleep(10);
								}

							bUp = (::GetAsyncKeyState(VK_UP) & 0x8000) != 0;
							bDown = (::GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
							bLeft = (::GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
							bRight = (::GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
							if ((bUp != false) || (bDown != false) || (bLeft != false) || (bRight != false))
								{
								PanXbase=0;
								PanYbase=0;
								DoStopPan=0;
								}
							}

						if (DoStopPan=1)
							{
							bUp = (::GetAsyncKeyState(VK_UP) & 0x8000) != 0;
							bDown = (::GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
							bLeft = (::GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
							bRight = (::GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;
							if ((bUp == false) && (bDown == false) && (bLeft == false) && (bRight == false))
								{
								::timeEndPeriod(1);
								m_bPanTimerActive = false;
								break;
								}
							}
						*/
						}
					else
						{
						//PanY ist die Pixelzahl des gezoomten bildes!
						iRealHeight = INT (m_dZoom * (m_pCurrentImage->OrigHeight()));
						iRealWidth = INT (m_dZoom * (m_pCurrentImage->OrigWidth()));

						/*
						if ((iRealHeight > m_clientRect.Height()) && (iRealWidth <= m_clientRect.Width()) && (m_bMangaMode == true))
							{
							if ((((bLeft == true) && (bRight == false)) || ((bUp == true) && (bDown == false))) && (PanYbase<10))
								PanYbase++;

							if ((((bLeft == false) && (bRight == true)) || ((bUp == false) && (bDown == true))) && (PanYbase>-10))
								PanYbase--;
							}
						else
							{
							if ((bUp == true) && (bDown == false) && (PanYbase<10))
								PanYbase++;
							else if ((bUp == false) && (bDown == true) && (PanYbase>-10))
								PanYbase--;

							if ((bLeft == true) && (bRight == false) && (PanXbase<10))
								PanXbase++;
							else if (( bLeft == false) && (bRight == true) && (PanXbase>-10))
								PanXbase--;
							}
						*/

						if ((iRealHeight > m_clientRect.Height()) && (iRealWidth <= m_clientRect.Width()) && (m_bMangaMode == true))
							{
							if (((bLeft == true) && (bRight == false)) || ((bUp == true) && (bDown == false)))
								PanYbase = 10;
							else if (((bLeft == false) && (bRight == true)) || ((bUp == false) && (bDown == true)))
								PanYbase = -10;
							else
								PanYbase = 0;
							}
						else
							{
							if ((bUp == true) && (bDown == false))
								PanYbase = 10;
							else if ((bUp == false) && (bDown == true))
								PanYbase = -10;
							else
								PanYbase = 0;

							if ((bLeft == true) && (bRight == false))
								PanXbase = 10;
							else if (( bLeft == false) && (bRight == true))
								PanXbase = -10;
							else
								PanXbase = 0;
							}

						if ((PanXbase != 0) && (PanYbase != 0))	// diagonal movement
							{
							PanX = (int)(PanXbase * 0.7);
							PanY = (int)(PanYbase * 0.7);
							}
						else
							{
							PanX = PanXbase;
							PanY = PanYbase;
							}
							
						if (PerformPan(PanX,PanY,false) == true)
							{
							//this->Invalidate(FALSE);
							this->UpdateWindow();

							MSG msg;
							while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));

							Sleep(1);
							}
						else
							{
							MSG msg;
							while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));

							Sleep(10);
							}
						}
					}
				}
			else
				{
				UINT timestart = timeGetTime();
				UINT timenow = 0;
				
				UINT modu_old = 4294967295;
				UINT modu = 0;
				
				while (m_bPanTimerActive == true)
					{
					bUp = (::GetAsyncKeyState(VK_UP) & 0x8000) != 0;
					bDown = (::GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
					bLeft = (::GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
					bRight = (::GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0;

					if ((bUp == false) && (bDown == false)  && (bLeft == false)  && (bRight == false))
						{
						::timeEndPeriod(1);
						m_bPanTimerActive = false;
						break;
						}
					else
						{
						timenow = timeGetTime();
						modu = ((timenow-timestart)*100) % 1668;	 // 1663 on intel
						INT BorderDistanceY = 0;

						if (modu < modu_old)
							{
							//PanY ist die Pixelzahl des gezoomten bildes!
							if (m_bMangaMode == false)
								{
								if ((bUp == true) && (bDown == false))
									PanY = 10;
								else if ((bUp == false) && (bDown == true))
									PanY = -10;

								if (( bLeft == true) && (bRight == false))
									PanX = 10;
								else if (( bLeft == false) && (bRight == true))
									PanX = -10;
								}
							else
								{
								iRealHeight = INT (m_dZoom * (m_pCurrentImage->OrigHeight()));
								iRealWidth = INT (m_dZoom * (m_pCurrentImage->OrigWidth()));

								if ((iRealHeight > m_clientRect.Height()) && (iRealWidth <= m_clientRect.Width()))
									{
									if ((bLeft == true) && (bRight == false))
										PanY = 10;
									else if ((bUp == true) && (bDown == false))
										PanY = 10;

									if ((bLeft == false) && (bRight == true))
										PanY = -10;
									else if ((bUp == false) && (bDown == true))
										PanY = -10;

									if (m_offsets.y>0)
										BorderDistanceY = ((iRealHeight-m_clientRect.Height())/2)-m_offsets.y;	// Top
									else
										BorderDistanceY = ((iRealHeight-m_clientRect.Height())/2)+m_offsets.y;	// Bottom
									
									if (BorderDistanceY<=100)
										{
										if (BorderDistanceY<6)
											PanY = (int)(PanY*0.1);
										else if (BorderDistanceY<8)
											PanY = (int)(PanY*0.2);
										else if (BorderDistanceY<12)
											PanY = (int)(PanY*0.3);
										else if (BorderDistanceY<17)
											PanY = (int)(PanY*0.4);
										else if (BorderDistanceY<24)
											PanY = (int)(PanY*0.5);
										else if (BorderDistanceY<34)
											PanY = (int)(PanY*0.6);
										else if (BorderDistanceY<50)
											PanY = (int)(PanY*0.7);
										else if (BorderDistanceY<70)
											PanY = (int)(PanY*0.8);
										else
											PanY = (int)(PanY*0.9);
										}
									}
								else
									{
									if ((bUp == true) && (bDown == false))
										PanY = 10;
									else if ((bUp == false) && (bDown == true))
										PanY = -10;

									if (( bLeft == true) && (bRight == false))
										PanX = 10;
									else if (( bLeft == false) && (bRight == true))
										PanX = -10;
									}
								}
								
							if ((PanX != 0) && (PanY != 0))	// diagonal movement
								{
								PanX = (int)(PanX * 0.7);
								PanY = (int)(PanY * 0.7);
								}

							if (PerformPan(PanX,PanY,false) == true)
								{
								//this->Invalidate(FALSE);
								this->UpdateWindow();
								}

							MSG msg;
							while (::PeekMessage(&msg, this->m_hWnd, 0, 0, PM_REMOVE));	// Empty message queue at 60 Hz
							}

						modu_old = modu;
						Sleep(1);
						}
					}
				}
			}
		}
	return 0;
	}

LRESULT CMainDlg::OnRButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/)
	{
	GotoImage(POS_Next);
	return 1;
	}

LRESULT CMainDlg::OnEraseBackground(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/)
	{
	// prevent erasing background
	return 0;
	}

LRESULT CMainDlg::OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
	// NOP
	return 0;
	}

LRESULT CMainDlg::OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
	{
	//SaveBookmark();
	CleanupAndTeminate();
	return 0;
	}



///////////////////////////////////////////////////////////////////////////////////
// Private
///////////////////////////////////////////////////////////////////////////////////

void CMainDlg::ExecuteCommand(int nCommand)
	{
	switch (nCommand)
		{
		case IDM_MINIMIZE:
			this->ShowWindow(SW_MINIMIZE);
			break;
		case IDM_OPEN:
			break;
		case IDM_SAVE:
		case IDM_SAVE_SCREEN:
			break;
		case IDM_RELOAD:
			GotoImage(POS_Current);
			break;
		case IDM_COPY:
			if (m_pCurrentImage != NULL)
				 CClipboard::CopyFullImageToClipboard(this->m_hWnd, m_pCurrentImage, PFLAG_HighQualityResampling);
			break;
		case IDM_COPY_FULL:
			break;
		case IDM_PASTE:
			break;
		case IDM_BATCH_COPY:
			break;
		case IDM_SHOW_FILENAME:
			break;
		case IDM_SHOW_NAVPANEL:
			break;
		case IDM_NEXT:
			GotoImage(POS_Next);
			break;
		case IDM_PREV:
			GotoImage(POS_Previous);
			break;
		case IDM_LOOP_FOLDER:
		case IDM_LOOP_RECURSIVELY:
		case IDM_LOOP_SIBLINGS:
			m_pFileList->SetNavigationMode(
				(nCommand == IDM_LOOP_FOLDER) ? Helpers::NM_LoopDirectory :
				(nCommand == IDM_LOOP_RECURSIVELY) ? Helpers::NM_LoopSubDirectories : 
				Helpers::NM_LoopSameDirectoryLevel);
			break;
		case IDM_SORT_MOD_DATE:
		case IDM_SORT_CREATION_DATE:
		case IDM_SORT_NAME:
		case IDM_SORT_RANDOM:
		case IDM_SORT_SIZE:
			m_pFileList->SetSorting(
				(nCommand == IDM_SORT_CREATION_DATE) ? Helpers::FS_CreationTime : 
				(nCommand == IDM_SORT_MOD_DATE) ? Helpers::FS_LastModTime : 
				(nCommand == IDM_SORT_RANDOM) ? Helpers::FS_Random : 
				(nCommand == IDM_SORT_SIZE) ? Helpers::FS_FileSize : Helpers::FS_FileName, m_pFileList->IsSortedUpcounting());
			break;
		case IDM_STOP_MOVIE:
			break;
		case IDM_SLIDESHOW_1:
		case IDM_SLIDESHOW_2:
		case IDM_SLIDESHOW_3:
		case IDM_SLIDESHOW_4:
		case IDM_SLIDESHOW_5:
		case IDM_SLIDESHOW_7:
		case IDM_SLIDESHOW_10:
		case IDM_SLIDESHOW_20:
		case IDM_MOVIE_5_FPS:
		case IDM_MOVIE_10_FPS:
		case IDM_MOVIE_25_FPS:
		case IDM_MOVIE_30_FPS:
		case IDM_MOVIE_50_FPS:
		case IDM_MOVIE_100_FPS:
			break;
		case IDM_SAVE_PARAM_DB:
			break;
		case IDM_CLEAR_PARAM_DB:
			break;
		case IDM_ROTATE_90:
		case IDM_ROTATE_270:
			if (m_pCurrentImage != NULL)
				{
				uint32 nRotationDelta = (nCommand == IDM_ROTATE_90) ? 90 : 270;
				m_nRotation = (m_nRotation + nRotationDelta) % 360;
				m_pCurrentImage->Rotate(nRotationDelta);
				m_dZoom = -1;
				this->Invalidate(FALSE);
				//this->UpdateWindow();
				}
			break;
		case IDM_MIRROR_H:
		case IDM_MIRROR_V:
			if (m_pCurrentImage != NULL)
				{
				m_pCurrentImage->Mirror(nCommand == IDM_MIRROR_H);
				this->Invalidate(FALSE);
				//this->UpdateWindow();
				}
			break;
		case IDM_AUTO_CORRECTION:
			break;
		case IDM_LDC:
			break;
		case IDM_LANDSCAPE_MODE:
			break;
		case IDM_KEEP_PARAMETERS:
			break;
		case IDM_SAVE_PARAMETERS:
			//SaveBookmark();
			break;
		case IDM_FIT_TO_SCREEN:
			break;
		case IDM_FILL_WITH_CROP:
			break;
		case IDM_SPAN_SCREENS:
			if (CMultiMonitorSupport::IsMultiMonitorSystem())
				{
				m_dZoom = -1.0;
				this->Invalidate(FALSE);
				//this->UpdateWindow();

				if (m_bSpanVirtualDesktop)
					{
					this->SetWindowPlacement(&m_storedWindowPlacement);
					}
				else
					{
					this->GetWindowPlacement(&m_storedWindowPlacement);
					CRect rectAllScreens = CMultiMonitorSupport::GetVirtualDesktop();
					this->SetWindowPos(HWND_TOP, &rectAllScreens, SWP_NOZORDER);
					}
				m_bSpanVirtualDesktop = !m_bSpanVirtualDesktop;
				this->GetClientRect(&m_clientRect);
				}
			break;
		case IDM_FULL_SCREEN_MODE:
			m_bFullScreenMode = !m_bFullScreenMode;
			if (!m_bFullScreenMode)
				{
				CRect windowRect;
				this->SetWindowLongW(GWL_STYLE, this->GetWindowLongW(GWL_STYLE) | WS_OVERLAPPEDWINDOW | WS_VISIBLE);
				if (::IsRectEmpty(&(m_storedWindowPlacement2.rcNormalPosition)))
					{
					// never set to window mode before, use default position
					windowRect = CMultiMonitorSupport::GetDefaultWindowRect();
					this->SetWindowPos(HWND_TOP, windowRect.left, windowRect.top, windowRect.Width(), windowRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
					}
				else
					{
					m_storedWindowPlacement2.flags = this->SetWindowPlacement(&m_storedWindowPlacement2);
					}

				this->MouseOn();
				}
			else
				{
				this->GetWindowPlacement(&m_storedWindowPlacement2);
				HMONITOR hMonitor = ::MonitorFromWindow(m_hWnd, MONITOR_DEFAULTTONEAREST);
				MONITORINFO monitorInfo;
				monitorInfo.cbSize = sizeof(MONITORINFO);
				if (::GetMonitorInfo(hMonitor, &monitorInfo))
					{
					CRect monitorRect(&(monitorInfo.rcMonitor));
					this->SetWindowLongW(GWL_STYLE, WS_VISIBLE);
					this->SetWindowPos(HWND_TOP, monitorRect.left, monitorRect.top, monitorRect.Width(), monitorRect.Height(), SWP_NOZORDER | SWP_NOCOPYBITS);
					}
				this->MouseOn();
				}

			m_dZoom = -1;
			m_bInLowQTimer = true;
			StartLowQTimer(ZOOM_TIMEOUT);
			this->SetWindowPos(NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOCOPYBITS | SWP_FRAMECHANGED);
			break;
		case IDM_ZOOM_400:
			break;
		case IDM_ZOOM_200:
			break;
		case IDM_ZOOM_100:
			break;
		case IDM_ZOOM_50:
			break;
		case IDM_ZOOM_25:
			break;
		case IDM_AUTO_ZOOM_FIT_NO_ZOOM:
		case IDM_AUTO_ZOOM_FILL_NO_ZOOM:
		case IDM_AUTO_ZOOM_FIT:
		case IDM_AUTO_ZOOM_FILL: 
			break;
		case IDM_EDIT_GLOBAL_CONFIG:
		case IDM_EDIT_USER_CONFIG:
			break;
		case IDM_BACKUP_PARAMDB:
			break;
		case IDM_RESTORE_PARAMDB:
			break;
		case IDM_ABOUT:
			break;
		case IDM_EXIT:
			//SaveBookmark();
			CleanupAndTeminate();
			break;
		case IDM_CROP_SEL:
			break;
		case IDM_LOSSLESS_CROP_SEL:
			break;
		case IDM_COPY_SEL:
			break;
		case IDM_CROPMODE_FREE:
			break;
		case IDM_CROPMODE_FIXED_SIZE:
			break;
		case IDM_CROPMODE_5_4:
			break;
		case IDM_CROPMODE_4_3:
			break;
		case IDM_CROPMODE_3_2:
			break;
		case IDM_CROPMODE_16_9:
			break;
		case IDM_CROPMODE_16_10:
			break;
		case IDM_TOUCH_IMAGE:
		case IDM_TOUCH_IMAGE_EXIF:
			break;
		case IDM_TOUCH_IMAGE_EXIF_FOLDER:
			break;
	}
}

void CMainDlg::OpenFile(LPCTSTR sFileName, bool bAfterStartup) {
	m_bTemporaryLowQ = true;
	::KillTimer(this->m_hWnd, ZOOM_TIMER_EVENT_ID);
	m_bInLowQTimer = m_bTemporaryLowQ = false;

	StopMovieMode();
	StopAnimation();

	// recreate file list based on image opened
	Helpers::ESorting eOldSorting = m_pFileList->GetSorting();
	bool oOldUpcounting = m_pFileList->IsSortedUpcounting();
	delete m_pFileList;
	m_sStartupFile = sFileName;
	m_pFileList = new CFileList(m_sStartupFile, *m_pDirectoryWatcher, eOldSorting, oOldUpcounting, CSettingsProvider::This().WrapAroundFolder());

	// free current image and all read ahead images
	m_pJPEGProvider->NotifyNotUsed(m_pCurrentImage);
	m_pJPEGProvider->ClearAllRequests();
	m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList, CJPEGProvider::FORWARD, 
		m_pFileList->Current(), 0, CreateProcessParams(0), m_bOutOfMemoryLastImage, m_bExceptionErrorLastImage);
	m_nLastLoadError = GetLoadErrorAfterOpenFile();

	AfterNewImageLoaded();
    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation()) {
        StartAnimation();
	}
	m_startMouse.x = m_startMouse.y = -1;
	MouseOff();
	this->Invalidate(FALSE);
}

void CMainDlg::GotoImage(EImagePosition ePos) {
	GotoImage(ePos, 0);
}

void CMainDlg::GotoImage(EImagePosition ePos, int nFlags)
	{
	// Timer handling for slideshows
	if (ePos == POS_Next || ePos == POS_NextSlideShow)
		{
		if (m_nCurrentTimeout > 0)
			{
			StartSlideShowTimer(m_nCurrentTimeout);
			}
        StopAnimation();
		}
	else if (ePos != POS_NextAnimation)
		{
		StopMovieMode();
        StopAnimation();
		}

    int nFrameIndex = 0;
	bool bCheckIfSameImage = true;
	m_pFileList->SetCheckpoint();
	CFileList* pOldFileList = m_pFileList;
    int nOldFrameIndex = (m_pCurrentImage == NULL) ? 0 : m_pCurrentImage->FrameIndex();
	CJPEGProvider::EReadAheadDirection eDirection = CJPEGProvider::FORWARD;
	switch (ePos)
		{
		case POS_First:
			m_pFileList->First();
			break;
		case POS_Last:
			m_pFileList->Last();
			break;
        case POS_Next:
        case POS_NextAnimation:
				{
                bool bGotoNextImage = true;
                nFrameIndex = Helpers::GetFrameIndex(m_pCurrentImage, true, ePos == POS_NextAnimation,  bGotoNextImage);
                if (bGotoNextImage)
					m_pFileList = m_pFileList->Next();
			    break;
				}
        case POS_NextSlideShow:
			m_pFileList = m_pFileList->Next();
			break;
		case POS_Previous:
				{
                bool bGotoPrevImage;
                nFrameIndex = Helpers::GetFrameIndex(m_pCurrentImage, false, false, bGotoPrevImage);
			    if (bGotoPrevImage)
					m_pFileList = m_pFileList->Prev();
			    eDirection = CJPEGProvider::BACKWARD;
			    break;
				}
		case POS_Toggle:
			m_pFileList->ToggleBetweenMarkedAndCurrentFile();
			eDirection = CJPEGProvider::TOGGLE;
			break;
		case POS_Current:
			bCheckIfSameImage = false; // do something even when not moving iterator on filelist
			break;
		case POS_AwayFromCurrent:
			m_pFileList = m_pFileList->AwayFromCurrent();
			break;
		}

	if (bCheckIfSameImage && (m_pFileList == pOldFileList && nOldFrameIndex == nFrameIndex && !m_pFileList->ChangedSinceCheckpoint()))
		return; // not placed on a new image, don't do anything

	if (ePos != POS_Current && ePos != POS_NextAnimation && ePos != POS_Clipboard && ePos != POS_AwayFromCurrent) {
		MouseOff();
	}

    if (!m_bIsAnimationPlaying)
	    m_bInLowQTimer = m_bTemporaryLowQ = false;

	m_pJPEGProvider->NotifyNotUsed(m_pCurrentImage);
	if (ePos == POS_Current || ePos == POS_AwayFromCurrent) {
		m_pJPEGProvider->ClearRequest(m_pCurrentImage, ePos == POS_AwayFromCurrent);
	}
	m_pCurrentImage = NULL;

	// do not perform a new image request if flagged
	if (nFlags & NO_REQUEST)
		return;

	if (ePos == POS_Previous)
		m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList, eDirection, m_pFileList->Current(), nFrameIndex, CreateProcessParams(1), m_bOutOfMemoryLastImage, m_bExceptionErrorLastImage);
	else
		m_pCurrentImage = m_pJPEGProvider->RequestImage(m_pFileList, eDirection, m_pFileList->Current(), nFrameIndex, CreateProcessParams(0), m_bOutOfMemoryLastImage, m_bExceptionErrorLastImage);

	m_nLastLoadError = (m_pCurrentImage == NULL) ? HelpersGUI::FileLoad_LoadError : HelpersGUI::FileLoad_Ok;
	
	if (ePos != POS_NextAnimation)
		AfterNewImageLoaded();

    // if it is an animation (currently only animated GIF) start movie automatically
    if (m_pCurrentImage != NULL && m_pCurrentImage->IsAnimation())
		{
        if (m_bIsAnimationPlaying)
			{
            AdjustAnimationFrameTime();
			}
		else
			{
            StartAnimation();
			}
		}

	m_dLastImageDisplayTime = Helpers::GetExactTickCount();
    if (!(ePos == POS_NextSlideShow && UseSlideShowTransitionEffect()))
		{
	    this->Invalidate(FALSE);
		// this will force to wait until really redrawn, preventing to process images but do not show them
	    this->UpdateWindow();
		}

	// remove key messages accumulated so far
	if (!(nFlags & NO_REMOVE_KEY_MSG))
		{
		MSG msg;
		while (::PeekMessage(&msg, this->m_hWnd, WM_KEYFIRST, WM_KEYLAST, PM_REMOVE));
		}
	}

void CMainDlg::DeleteImageShown()
	{
	TCHAR sFileToDelete[MAX_PATH];
	lstrcpy(sFileToDelete,CurrentFileName());	// "CurrentFileName()" returns the full path to the presently shown file, not only its name!

	if (::PathFileExists(sFileToDelete))
		{
		MouseOn();

		TCHAR sParentDir[MAX_PATH];
		lstrcpy(sParentDir,sFileToDelete);
		PathRemoveFileSpec(sParentDir);

		TCHAR sParentDirName[MAX_PATH];
		lstrcpy(sParentDirName,sParentDir);
		PathStripPath(sParentDirName);

		TCHAR sFName[MAX_PATH];
		lstrcpy(sFName,sFileToDelete);
		PathStripPath(sFName);

		TCHAR tempstr[1024];
		wsprintf(tempstr,TEXT("Do you really want to delete this file permanently?\n\n%s\\%s"),sParentDirName,sFName);
		if ((::MessageBox(CMainDlg::m_hWnd,tempstr,TEXT("Delete File"), MB_YESNO | MB_ICONQUESTION | MB_TASKMODAL)) == IDYES)
			{
			if (::PathFileExists(sFileToDelete))
				{
				if (m_pFileList->Size() == 1)
					{
					StopMovieMode();
					StopAnimation();
					
					// m_pJPEGProvider is instance of "CJPEGProvider" class that reads and processes JPEG files using read ahead with own threads
					delete m_pJPEGProvider; // delete this early to properly shut down the loading threads
					m_pJPEGProvider = NULL;

					DeleteFile(sFileToDelete);
					EndDialog(0);
					}
				else
					{
					GotoImage(POS_Next);
					//LPCTSTR sCurrentFileName = m_pFileList->Current();
					//m_pFileList->Reload(sCurrentFileName, true);
					m_pFileList->ListFileDelete(sFileToDelete);
					Sleep(100);	// hopefully the JPEGProvider timers have ended by now and the file can be write-accessed again
					DeleteFile(sFileToDelete);
					}
				}
			}
		MouseOff();
		}
	else
		{
		GotoImage(POS_Next);
		m_pFileList->Reload(m_pFileList->Current());
		}

	return;
	}

void CMainDlg::PerformZoom(double dValue, bool bZoomToMouse)
	{
	if (m_pCurrentImage == NULL)
		return;

	double dRelative = 0.0;
	double dZoomWindow = 0.0;
//* Debugging */	TCHAR debugtext[512];

	double dOldZoom = m_dZoom;
	m_dZoom = m_dZoom * pow(1.1, dValue);	// dValue is almost always 1. Therefore, this usually does (m_dZoom*1.1)
	
	dRelative = (m_dZoom/1.0);
	if (dRelative < 1.0)
		dRelative = 1/dRelative;

//* Debugging */	swprintf(debugtext,255,TEXT("old: %f  new: %f  rel_1: %f"), dOldZoom, m_dZoom, dRelative);
//* Debugging */	::OutputDebugStringW(debugtext);

	if (dRelative<1.05)		// Special case 1: Image zoomed size close to image real size -> Use real size
		{
//* Debugging */	::OutputDebugStringW(TEXT("Setting m_dZoom=1.0"));

		m_dZoom = 1.0;
		}
	else
		{
		dZoomWindow = GetZoomFactorForFitToWindow();

		dRelative = (m_dZoom/dZoomWindow);
		if (dRelative < 1.0)
			dRelative = 1/dRelative; 

//* Debugging */	swprintf(debugtext,255,TEXT("old: %f  new: %f  rel_2: %f"), dOldZoom, m_dZoom, dRelative);
//* Debugging */	::OutputDebugStringW(debugtext);

		if (dRelative<1.05)		// Special case 2: Image zoomed size close to ZoomToWindow size -> Use ZoomToWindow size
			{
//* Debugging */	swprintf(debugtext,255,TEXT("Setting m_dZoom=dZoomWindow=%f)"), dZoomWindow);
//* Debugging */	::OutputDebugStringW(debugtext);

			m_dZoom = dZoomWindow;
			}
		}

	int nOldXSize = Helpers::RoundToInt(m_pCurrentImage->OrigWidth() * dOldZoom);
	int nOldYSize = Helpers::RoundToInt(m_pCurrentImage->OrigHeight() * dOldZoom);
	int nNewXSize = Helpers::RoundToInt(m_pCurrentImage->OrigWidth() * m_dZoom);
	int nNewYSize = Helpers::RoundToInt(m_pCurrentImage->OrigHeight() * m_dZoom);

//* Debugging */	swprintf(debugtext,255,TEXT("nNewXSize=%d  nNewYSize=%d)"), nNewXSize, nNewYSize);
//* Debugging */	::OutputDebugStringW(debugtext);

	if (nNewXSize > 65535 || nNewYSize > 65535)
		{
		// Never create images more than 65535 pixels wide or high - the basic processing cannot handle it

		if (nNewXSize>=nNewYSize)
			{
			nNewXSize = 65535;
			m_dZoom = ((double)nNewXSize / (double)(m_pCurrentImage->OrigWidth()));
			nNewYSize = Helpers::RoundToInt(m_pCurrentImage->OrigHeight() * m_dZoom);
			}
		else
			{
			nNewYSize = 65535;
			m_dZoom = ((double)nNewYSize / (double)(m_pCurrentImage->OrigHeight()));
			nNewXSize = Helpers::RoundToInt(m_pCurrentImage->OrigWidth() * m_dZoom);
			}
		}
	else if (nNewXSize < 1 || nNewYSize < 1)
		{
		m_dZoom = dOldZoom;
		nNewXSize = nOldXSize;
		nNewYSize = nOldYSize;
		}

	if (bZoomToMouse)
		{
		// zoom to mouse
		int nOldX = nOldXSize/2 - m_clientRect.Width()/2 + m_nMouseX - m_offsets.x;
		int nOldY = nOldYSize/2 - m_clientRect.Height()/2 + m_nMouseY - m_offsets.y;
		double dFac = m_dZoom/dOldZoom;
		m_offsets.x = Helpers::RoundToInt(nNewXSize/2 - m_clientRect.Width()/2 + m_nMouseX - nOldX*dFac);
		m_offsets.y = Helpers::RoundToInt(nNewYSize/2 - m_clientRect.Height()/2 + m_nMouseY - nOldY*dFac);
		}
	else
		{
		// zoom to center
		m_offsets.x = Helpers::RoundToInt(m_offsets.x*m_dZoom/dOldZoom);
		m_offsets.y = Helpers::RoundToInt(m_offsets.y*m_dZoom/dOldZoom);
		}

	if (fabs(dOldZoom - m_dZoom) > 0.0)
		{
		m_bInLowQTimer = true;
		StartLowQTimer(ZOOM_TIMEOUT);
		this->Invalidate(FALSE);
	    //this->UpdateWindow();

		if ((m_bMangaMode == true) && (m_bZoomMode == false))
			{
			int nImageW = m_pCurrentImage->OrigWidth();
			int nImageH = m_pCurrentImage->OrigHeight();
			int nWinW = m_clientRect.Width();
			int nWinH = m_clientRect.Height();
		
			double dImageAR = (double)nImageW/nImageH;
			if (dImageAR < 1.0)		// single page
				{
				if (((nImageW * m_dZoom) <= nWinW) && ((nImageH * m_dZoom) >= nWinH))
					{
					m_nMangaSinglePageVisibleHeight = (int)((nWinH / (nImageH * m_dZoom)) * 100);
					}
				}
			}
		}
	}

bool CMainDlg::PerformPan(int dx, int dy, bool bAbsolute)
	{
	if ((m_virtualImageSize.cx > 0 && m_virtualImageSize.cx > m_clientRect.Width()) || (m_virtualImageSize.cy > 0 && m_virtualImageSize.cy > m_clientRect.Height()))
		{
		if (bAbsolute)
			{
			m_offsets = CPoint(dx, dy);
			}
		else
			{
			m_offsets = CPoint(m_offsets.x + dx, m_offsets.y + dy);
			}

		this->Invalidate(FALSE);
		return true;
		}

	return false;
	}

double CMainDlg::GetZoomFactorForFitToWindow()
	{
	if (m_pCurrentImage != NULL)
		{
		int nImageW = m_pCurrentImage->OrigWidth();
		int nImageH = m_pCurrentImage->OrigHeight();
		int nWinW = m_clientRect.Width();
		int nWinH = m_clientRect.Height();

		double dZoom;
		double dAR1 = (double)nImageW/nWinW;
		double dAR2 = (double)nImageH/nWinH;
		dZoom = 1.0/(max(dAR1, dAR2));
		return dZoom;
		}
	else
		return 1.0;
	}

double CMainDlg::ConditionalZoomFactor()
	{	
	int nImageW;
	int nImageH;
	int nWinW = m_clientRect.Width();
	int nWinH = m_clientRect.Height();
	double dZoom;
	
	if (m_pCurrentImage == NULL)
		dZoom = 1.0;
	else
		{
		nImageW = m_pCurrentImage->OrigWidth();
		nImageH = m_pCurrentImage->OrigHeight();

		if (m_bMangaMode == true)
			{
			if (m_bZoomMode == false)				// Original Size, but shrink to window width (aka ResetZoomTo100Percents)
				{
				double dImageAR = (double)nImageW/nImageH;
				if (dImageAR >= 1.0)							// If it's wider than high, assume a double page
					{
					/*
					if (nImageW>nWinW)							// if wider than window, shrink to window width
						dZoom = (double)nWinW/nImageW;
					else if (nImageH<nWinH)						// else if height smaller than window, enlarge to window height
						{
						double dAR1 = (double)nImageW/nWinW;
						double dAR2 = (double)nImageH/nWinH;
						double dAR = max(dAR1, dAR2);
						dZoom = 1.0/dAR;
						}
					else
						dZoom = 1.0;
					*/
					dZoom = (double)nWinW/nImageW;		// Fit to screen width
					}
				else								// single page
					{
					double dAR1 = (((double)nImageH/100.0)*m_nMangaSinglePageVisibleHeight) / (double)nWinH;	// Zoom to user specified height
					double dAR2 = (double)nImageW/nWinW;														// ...but limit maximum image width to window width
					double dAR = max(dAR1, dAR2);
					dZoom = 1.0/dAR;
					}
				}
			else						// ResetZoomToFitWindow (Shrink/enlarge to window)
				{			
				double dAR1 = (double)nImageW/nWinW;
				double dAR2 = (double)nImageH/nWinH;
				double dAR = max(dAR1, dAR2);
				dZoom = 1.0/dAR;
				}
			}
		else
			{
			if (m_bZoomMode == false)							// Zoom to window for default (m_bZoomMode = false) 
				{
				double dAR1 = (double)nImageW/nWinW;
				double dAR2 = (double)nImageH/nWinH;
				double dAR = max(dAR1, dAR2);
				dZoom = 1.0/dAR;
				}
			else												// Leave true size for (m_bZoomMode == true) 
				dZoom = 1.0;

			/*
			if (m_bZoomMode == false)							// Shrink to window, if bigger than window; Original size, if smaller than window
				{			
				double dAR1 = (double)nImageW/nWinW;
				double dAR2 = (double)nImageH/nWinH;
				double dAR = max(dAR1, dAR2);

				if (dAR <= 1.0)
					dZoom = 1.0;
				else
					dZoom = 1.0/dAR;
				}
			else											// Original Size, if bigger than window; Enlarge to window, if smaller than window
				{
				if ((nImageW>nWinW)||(nImageH>nWinH))
					dZoom = 1.0;
				else
					{
					double dAR1 = (double)nImageW/nWinW;
					double dAR2 = (double)nImageH/nWinH;
					double dAR = max(dAR1, dAR2);
					dZoom = 1.0/dAR;
					}
				}
			*/
			}
		}

	return dZoom;
	}

CSize CMainDlg::GetVirtualImageSize()
	{
	if (m_pCurrentImage == NULL)
		{
		return CSize(1,1);
		}

	if (m_dZoom < 0.0)
		m_dZoom = ConditionalZoomFactor();

	int nNewXSize = (int)(m_pCurrentImage->OrigWidth() * m_dZoom);
	int nNewYSize = (int)(m_pCurrentImage->OrigHeight() * m_dZoom);

	if (nNewXSize > 65535 || nNewYSize > 65535)
		{
		double dFac = 65535.0/max(nNewXSize,nNewYSize);
		m_dZoom = m_dZoom*dFac;
		nNewXSize = (int)(m_pCurrentImage->OrigWidth() * m_dZoom);
		nNewYSize = (int)(m_pCurrentImage->OrigHeight() * m_dZoom);
		}

	return CSize(nNewXSize,nNewYSize);	
	}

// CreateProcessParams() is called one time for each image and/or animation frame
// It is only called as parameter in RequestImage() in OnInitDialog(), GotoImage() and OpenFile()
CProcessParams CMainDlg::CreateProcessParams(bool ToPreviousImage)
	{
	int nClientWidth = m_clientRect.Width();
	int nClientHeight = m_clientRect.Height();

	LPCTSTR sCurrentFileName = CurrentFileName();
	if (sCurrentFileName != NULL)
		{
		if (StrStrI(sCurrentFileName,TEXT("\\manga\\")))
			{
			if (m_bMangaMode == false)
				m_bZoomMode = false;			// reset ZoomMode if we switch from eBooks to normal images

			m_bMangaMode = true;

			if (ToPreviousImage == false)
				m_offsets = CPoint(-65000,65000);	// manga mode: focus top right (RTL)
			else
				m_offsets = CPoint(65000,-65000);	// manga mode: focus bottom left (RTL)

			m_offsets_custom = m_offsets;
			}
		else
			{
			if (m_bMangaMode == true)
				m_bZoomMode = false;		// reset ZoomMode if we switch from normal images to eBooks
			
			m_bMangaMode = false;
			m_offsets = CPoint(0,0);		// image mode: focus image center
			m_offsets_custom = m_offsets;
			}
		}

	m_bHQResampling = true;
	m_nRotation = 0;
	m_dZoom = -1;

	// TargetWidth,TargetHeight is the dimension of the target output screen, the image is fit into this rectangle
	// nRotation must be 0, 90, 270 degrees
	// dZoom is the zoom factor compared to intial image size (1.0 means no zoom)
	// offsets are relative to center of image and refer to original image size (not zoomed)
	// CProcessParams(int nTargetWidth, int nTargetHeight, int nRotation, double dZoom, CPoint offsets, EProcessingFlags eProcFlags)
	return CProcessParams(nClientWidth, nClientHeight, 0, -1, m_offsets, PFLAG_HighQualityResampling);
	}

void CMainDlg::StartSlideShowTimer(int nMilliSeconds)
	{
	m_nCurrentTimeout = nMilliSeconds;
	::SetTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID, nMilliSeconds, NULL);
	//m_pNavPanelCtl->EndNavPanelAnimation();
    m_nLastSlideShowImageTickCount = ::GetTickCount();
	}

void CMainDlg::StopSlideShowTimer(void) {
	if (m_nCurrentTimeout > 0) {
		m_nCurrentTimeout = 0;
		::KillTimer(this->m_hWnd, SLIDESHOW_TIMER_EVENT_ID);
	}
}

void CMainDlg::StartMovieMode(double dFPS)
	{
	// if more than this number of frames are requested per seconds, it is considered to be a movie
	const double cdFPSMovie = 4.9;

	// Turn off high quality resamping and auto corrections when requested to play many frames per second
	if (dFPS > cdFPSMovie)
		m_bHQResampling = false;

	m_bMovieMode = true;
	StartSlideShowTimer(Helpers::RoundToInt(1000.0/dFPS));
    this->Invalidate(FALSE);
	//this->UpdateWindow();
	}

void CMainDlg::StopMovieMode()
	{
	if (m_bMovieMode)
		{
		m_bHQResampling = true;
		m_bMovieMode = false;
		StopSlideShowTimer();
		this->Invalidate(FALSE);
	    //this->UpdateWindow();
		}
	}

void CMainDlg::StartLowQTimer(int nTimeout)
	{
	m_bTemporaryLowQ = true;
	::KillTimer(this->m_hWnd, ZOOM_TIMER_EVENT_ID);
	::SetTimer(this->m_hWnd, ZOOM_TIMER_EVENT_ID, nTimeout, NULL);
	}

void CMainDlg::MouseOff()
	{
	if (m_bMouseOn)
		{
		if (m_bFullScreenMode)
			{
			while (::ShowCursor(FALSE) >= 0);
			}
		m_startMouse.x = m_startMouse.y = -1;
		m_bMouseOn = false;
		}
	}

void CMainDlg::MouseOn()
	{
	if (!m_bMouseOn)
		{
		::ShowCursor(TRUE);
		m_bMouseOn = true;
		}
	}

void CMainDlg::AfterNewImageLoaded()
	{
	if (m_pCurrentImage != NULL)
		{
		UpdateWindowTitle();
		m_bHQResampling = true;
		m_pDirectoryWatcher->SetCurrentFile(CurrentFileName());
		}
	}
/*
void CMainDlg::SaveBookmark()
	{
	TCHAR sCurrentFileName[MAX_PATH];
	lstrcpy(sCurrentFileName,CurrentFileName());
	bool IsEndpoint = m_pFileList->IsEndpoint();

	if (sCurrentFileName != NULL)
		{
		if (StrStrI(sCurrentFileName,TEXT("\\manga\\")))
			{
			TCHAR AppPath[MAX_PATH];
			TCHAR INIpath[MAX_PATH];
			TCHAR SavePath[MAX_PATH];

			::GetModuleFileName(NULL,AppPath,MAX_PATH);
			PathRemoveFileSpec(AppPath);
			PathCombine(INIpath,AppPath,TEXT("..\\..\\3\\Settings\\AHK_Globals3.ini"));
			
			// If "AHK_Globals3.ini" exists, try to find the SafePath that way
			if (::PathFileExists(INIpath))
				{
				CString sFolderUserData;

				TCHAR buff[1024];
				buff[1023] = 0;
				::GetPrivateProfileString(_T("AutoHotkey"),_T("g_FolderUserD"), _T(""), buff, 1023, INIpath);
				if (buff[0] != 0)
					sFolderUserData = CString(buff);
				
				// Convert relative path to absolute path, if neccessary
				if ((StrStrI(sFolderUserData,TEXT("\\"))) == NULL)
					{
					TCHAR TempPath[MAX_PATH];
					wsprintf(TempPath,TEXT("..\\..\\..\\%s\\1\\Lesezeichen\\JPEGView.ini"), (LPCTSTR)sFolderUserData);
					PathCombine(SavePath,AppPath,TempPath);
					}
				else
					{
					wsprintf(SavePath,TEXT("%s\\1\\Lesezeichen\\JPEGView.ini"), (LPCTSTR)sFolderUserData);
					}
				}
			else
				PathCombine(SavePath,AppPath,TEXT("..\\..\\..\\D\\1\\Lesezeichen\\JPEGView.ini"));
			
			// Now we know the path of the bookmark file for JPEGView
			if (::PathFileExists(SavePath))
				{
				// The Ini-Key name equals the parent folder name. 
				TCHAR ParentName[MAX_PATH];
				lstrcpy(ParentName,sCurrentFileName);
				PathRemoveFileSpec(ParentName);
				PathStripPath(ParentName);

				CString ParentNameC = ReplaceNoCase(ParentName,TEXT(" [en]"),TEXT(""));
				ParentNameC.Replace('[','<');
				ParentNameC.Replace(']','>');

				//MessageBox(NULL,ParentNameC,TEXT("ParentNameC is"), MB_OK | MB_ICONERROR);

				// The Ini-Value equals the filename including its extension. 
				TCHAR FileName[MAX_PATH];
				lstrcpy(FileName,sCurrentFileName);
				PathStripPath(FileName);

				CString FileNameC = CString(FileName);
				FileNameC.Replace('[','<');
				FileNameC.Replace(']','>');

				// We try to read the value of the same name
				TCHAR sINIvalue[1024];
				sINIvalue[1023] = 0;
				::GetPrivateProfileString(_T("JPEGView"),ParentNameC,_T(""),sINIvalue,1023,SavePath);
				
				// continue only if the existant value is different, or if no value exists
				if ((sINIvalue[0] == 0) || (lstrcmpi(sINIvalue,FileNameC) != 0))
					{
					if (IsEndpoint == true)
						{
						//MessageBox(NULL,TEXT("Endpoint !!"),TEXT("Writing to Key:"), MB_OK | MB_ICONERROR);
						::WritePrivateProfileString(_T("JPEGView"),ParentNameC,NULL,SavePath);
						}
					else
						{
						//MessageBox(NULL,TEXT("no endpoint..."),TEXT("Writing to Key:"), MB_OK | MB_ICONERROR);
						::WritePrivateProfileString(_T("JPEGView"),ParentNameC,FileNameC,SavePath);
						}
					}
				}
			else
				{
				MouseOn();

				TCHAR TempInfo[1024];
				wsprintf(TempInfo,TEXT("Lesezeichen kann nicht gespeichert werden. Pfad existiert nicht:\n%s"),SavePath);
				::MessageBox(NULL,TempInfo,TEXT("JpegView Warnung"), MB_OK | MB_ICONERROR);
				}
			}
		}

	return;
	}
*/

CRect CMainDlg::ScreenToDIB(const CSize& sizeDIB, const CRect& rect)
	{
	int nOffsetX = (sizeDIB.cx - m_clientRect.Width())/2;
	int nOffsetY = (sizeDIB.cy - m_clientRect.Height())/2;

	CRect rectDIB = CRect(rect.left + nOffsetX, rect.top + nOffsetY, rect.right + nOffsetX, rect.bottom + nOffsetY);
	
	CRect rectClipped;
	rectClipped.IntersectRect(rectDIB, CRect(0, 0, sizeDIB.cx, sizeDIB.cy));
	return rectClipped;
	}

bool CMainDlg::ScreenToImage(float & fX, float & fY) 
	{
	if (m_pCurrentImage == NULL)
		{
		return false;
		}
	int nOffsetX = (m_pCurrentImage->DIBWidth() - m_clientRect.Width())/2;
	int nOffsetY = (m_pCurrentImage->DIBHeight() - m_clientRect.Height())/2;

	fX += nOffsetX;
	fY += nOffsetY;

	m_pCurrentImage->DIBToOrig(fX, fY);

	return true;
	}

bool CMainDlg::ImageToScreen(float & fX, float & fY)
	{
	if (m_pCurrentImage == NULL)
		{
		return false;
		}
	m_pCurrentImage->OrigToDIB(fX, fY);

	int nOffsetX = (m_pCurrentImage->DIBWidth() - m_clientRect.Width())/2;
	int nOffsetY = (m_pCurrentImage->DIBHeight() - m_clientRect.Height())/2;

	fX -= nOffsetX;
	fY -= nOffsetY;

	return true;
	}

LPCTSTR CMainDlg::CurrentFileName()
	{
	if (m_pCurrentImage != NULL && m_pCurrentImage->IsClipboardImage())
		{
		return _T("Clipboard Image");
		}
	if (m_pFileList != NULL)
		{
		return m_pFileList->Current();
		}
	else
		{
		return NULL;
		}
	}

void CMainDlg::UpdateWindowTitle()
	{
	static HICON hIconBig = NULL;
	static HICON hIconSmall = NULL;

	LPCTSTR sCurrentFileName = CurrentFileName();

	if (sCurrentFileName == NULL)
		{
		if (lstrcmpi(_T("JPEGView"),s_PrevTitleText) != 0)
			{
			_stprintf_s(s_PrevTitleText, MAX_PATH, _T("%s"),_T("JPEGView"));
			this->SetWindowText(_T("JPEGView"));
			}
		}
	else
		{
		if (lstrcmpi(sCurrentFileName,s_PrevTitleText) != 0)
			{
			_stprintf_s(s_PrevTitleText, MAX_PATH, _T("%s"),sCurrentFileName);
			this->SetWindowText(sCurrentFileName);
			}

		TCHAR *pExt = NULL;								// TCHAR = WCHAR on unicode
		pExt = PathFindExtension(sCurrentFileName);		// input: PTSTR, output: PTSTR

		if (lstrcmpi(pExt,s_PrevFileExt) != 0)
			{
			if (*pExt != '\0')
				{
				//::OutputDebugStr(_T("UpdateWindowTitle() for "));
				//::OutputDebugStr(CurrentFileName());
				//::OutputDebugStr(_T("s_PrevFileExt: "));
				//::OutputDebugStr(s_PrevFileExt);
				//::OutputDebugStr(_T("\n"));

				wsprintf(s_PrevFileExt, _T("%s"),pExt);

				if (hIconBig != NULL)
					{
					DestroyIcon(hIconBig);
					hIconBig = NULL;
					}

				if (hIconSmall != NULL)
					{
					DestroyIcon(hIconSmall);
					hIconSmall = NULL;
					}

				TCHAR buffer_1[MAX_PATH];
				::GetModuleFileName(NULL,buffer_1,MAX_PATH);
				PathRemoveFileSpec(buffer_1);
				LPTSTR lpStr1;
				lpStr1 = buffer_1;

				TCHAR buffer_2[] = TEXT("..\\Icons");
				LPTSTR lpStr2;
				lpStr2 = buffer_2;

				TCHAR buffer_3[MAX_PATH] = TEXT("");
				LPTSTR lpStr3;
				lpStr3 = buffer_3;
				PathCombine(lpStr3,lpStr1,lpStr2);

				TCHAR buffer_4[1024] = TEXT("");
				LPTSTR lpStr4;
				lpStr4 = buffer_4;

				wsprintf(lpStr4,TEXT("%s\\doc_%s.ico"),lpStr3,pExt+1);

				if ((::PathFileExists(lpStr4))&&(m_pCurrentImage != NULL))
					{
					hIconBig = (HICON)::LoadImage(NULL,lpStr4,IMAGE_ICON,::GetSystemMetrics(SM_CXICON),::GetSystemMetrics(SM_CYICON),LR_LOADFROMFILE);
					hIconSmall = (HICON)::LoadImage(NULL,lpStr4,IMAGE_ICON,::GetSystemMetrics(SM_CXSMICON),::GetSystemMetrics(SM_CYSMICON),LR_LOADFROMFILE);
					}
				}
			else
				{
				//::OutputDebugStr(_T("UpdateWindowTitle() for "));
				//::OutputDebugStr(CurrentFileName());
				//::OutputDebugStr(_T("s_PrevFileExt: "));
				//::OutputDebugStr(s_PrevFileExt);
				//::OutputDebugStr(_T("*pExt = '\0'"));
				//::OutputDebugStr(_T("\n"));

				*s_PrevFileExt = '\0';

				if (hIconBig != NULL)
					{
					DestroyIcon(hIconBig);
					hIconBig = NULL;
					}

				if (hIconSmall != NULL)
					{
					DestroyIcon(hIconSmall);
					hIconSmall = NULL;
					}
				}

			if (hIconBig == NULL)
				hIconBig = (HICON)::LoadImage(::GetModuleHandle(0),MAKEINTRESOURCE(IDR_MAINFRAME),IMAGE_ICON,::GetSystemMetrics(SM_CXICON),::GetSystemMetrics(SM_CYICON),LR_VGACOLOR);
			if (hIconSmall == NULL)
				hIconSmall = (HICON)::LoadImage(::GetModuleHandle(0),MAKEINTRESOURCE(IDR_MAINFRAME),IMAGE_ICON,::GetSystemMetrics(SM_CXSMICON),::GetSystemMetrics(SM_CYSMICON),LR_VGACOLOR);

			if (hIconBig != NULL)
				::SendMessage(CMainDlg::m_hWnd,WM_SETICON,ICON_BIG,(LPARAM)hIconBig);
			if (hIconSmall != NULL)
				::SendMessage(CMainDlg::m_hWnd,WM_SETICON,ICON_SMALL,(LPARAM)hIconSmall);
			}
		}
	}

int CMainDlg::GetLoadErrorAfterOpenFile()
	{
	if (m_pCurrentImage == NULL)
		{
		if (CurrentFileName() == NULL)
			{
			if (m_pFileList->IsSlideShowList())
				{
				return HelpersGUI::FileLoad_SlideShowListInvalid;
				}
			return HelpersGUI::FileLoad_NoFilesInDirectory;
			}
		return HelpersGUI::FileLoad_LoadError;
		}
	return HelpersGUI::FileLoad_Ok;
	}

bool CMainDlg::UseSlideShowTransitionEffect()
	{
    //return m_bFullScreenMode && m_nCurrentTimeout >= 1000 && m_eTransitionEffect != Helpers::TE_None;
	return false;
	}

void CMainDlg::CleanupAndTeminate()
	{
    StopMovieMode();
    StopAnimation();
    delete m_pJPEGProvider; // delete this early to properly shut down the loading threads
    m_pJPEGProvider = NULL;
	EndDialog(0);
	}
    
void CMainDlg::StartAnimation()
	{
    if (m_bIsAnimationPlaying)
        return;

    m_bIsAnimationPlaying = true;
	int nNewFrameTime = max(10, m_pCurrentImage->FrameTimeMs());
	::SetTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID, nNewFrameTime, NULL);

	m_nLastSlideShowImageTickCount = ::GetTickCount();
	m_nLastAnimationOffset = 0;
	m_nExpectedNextAnimationTickCount = ::GetTickCount() + nNewFrameTime;
	}

void CMainDlg::AdjustAnimationFrameTime() {
	// restart timer with new frame time
	::KillTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID);
	m_nLastAnimationOffset += ::GetTickCount() - m_nExpectedNextAnimationTickCount;
	m_nLastAnimationOffset = min(m_nLastAnimationOffset, max(100, m_pCurrentImage->FrameTimeMs())); // prevent offset from getting too big
	int nNewFrameTime = max(10, m_pCurrentImage->FrameTimeMs() - max(0, m_nLastAnimationOffset));
	m_nExpectedNextAnimationTickCount = ::GetTickCount() + max(10, m_pCurrentImage->FrameTimeMs());
	::SetTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID, nNewFrameTime, NULL);
}

void CMainDlg::StopAnimation()
	{
    if (!m_bIsAnimationPlaying)
        return;

    ::KillTimer(this->m_hWnd, ANIMATION_TIMER_EVENT_ID);
    m_bIsAnimationPlaying = false;
	}

BOOL CMainDlg::Is64BitOS()
	{
	BOOL bIs64BitOS = FALSE;

	typedef BOOL (WINAPI *LPFN_ISWOW64PROCESS) (HANDLE, PBOOL);

	LPFN_ISWOW64PROCESS
		fnIsWow64Process = (LPFN_ISWOW64PROCESS)GetProcAddress(GetModuleHandle(TEXT("kernel32")),"IsWow64Process");
 
	if (NULL != fnIsWow64Process)
		{
		if (!fnIsWow64Process(GetCurrentProcess(),&bIs64BitOS))
			{
			//error
			}
		}
	return bIs64BitOS;
	}

CString CMainDlg::ReplaceNoCase(LPCTSTR instr,LPCTSTR oldstr,LPCTSTR newstr)
	{
	CString output( instr );

	// lowercase-versions to search in.
	CString input_lower( instr );
	CString oldone_lower( oldstr );
	input_lower.MakeLower();
	oldone_lower.MakeLower();

	// search in the lowercase versions,
	// replace in the original-case version.
	int pos=0;
	while ((pos=input_lower.Find(oldone_lower,pos)) != -1)
		{

		// need for empty "newstr" cases.
		input_lower.Delete( pos, lstrlen(oldstr) );	
		input_lower.Insert( pos, newstr );

		// actually replace.
		output.Delete( pos, lstrlen(oldstr) );
		output.Insert( pos, newstr );
		}

	return output;
	}
