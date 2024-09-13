// MainDlg.h : interface of the CMainDlg class
//
/////////////////////////////////////////////////////////////////////////////

#pragma once

#include "MessageDef.h"
#include "ProcessParams.h"
#include "Helpers.h"

class CFileList;
class CJPEGProvider;
class CJPEGImage;
class CDirectoryWatcher;

enum EMouseEvent;

// The main dialog is a full screen modal dialog with no border and no window title
class CMainDlg : public CDialogImpl<CMainDlg>
{
public:
	enum { IDD = IDD_MAINDLG };

	// Used in GotoImage() call
	enum EImagePosition {
		POS_First,
		POS_Last,
		POS_Next,
        POS_NextSlideShow,
        POS_NextAnimation,
		POS_Previous,
		POS_Current,
		POS_Clipboard,
		POS_Toggle,
        POS_AwayFromCurrent
	};

	CMainDlg();
	~CMainDlg();


	BEGIN_MSG_MAP(CMainDlg)
		MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBackground)
		MESSAGE_HANDLER(WM_INITDIALOG, OnInitDialog)
		MESSAGE_HANDLER(WM_SIZE, OnSize)
		MESSAGE_HANDLER(WM_GETMINMAXINFO, OnGetMinMaxInfo)
		MESSAGE_HANDLER(WM_PAINT, OnPaint)
		MESSAGE_HANDLER(WM_LBUTTONDOWN, OnLButtonDown)
		MESSAGE_HANDLER(WM_LBUTTONDBLCLK, OnLButtonDown)
		MESSAGE_HANDLER(WM_MBUTTONDOWN, OnMButtonDown)
		MESSAGE_HANDLER(WM_MBUTTONDBLCLK, OnMButtonDown)
		MESSAGE_HANDLER(WM_MBUTTONUP, OnMButtonUp)
		MESSAGE_HANDLER(WM_XBUTTONDOWN, OnXButtonDown)
		MESSAGE_HANDLER(WM_XBUTTONDBLCLK, OnXButtonDown)
		MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
		MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
		MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
		MESSAGE_HANDLER(WM_GETDLGCODE, OnGetDlgCode)
		MESSAGE_HANDLER(WM_TIMER, OnTimer)
		MESSAGE_HANDLER(WM_RBUTTONDOWN, OnRButtonDown)
		MESSAGE_HANDLER(WM_RBUTTONDBLCLK, OnRButtonDown)
		MESSAGE_HANDLER(WM_IMAGE_LOAD_COMPLETED, OnImageLoadCompleted)
		MESSAGE_HANDLER(WM_DISPLAYED_FILE_CHANGED_ON_DISK, OnDisplayedFileChangedOnDisk)
		MESSAGE_HANDLER(WM_ACTIVE_DIRECTORY_FILELIST_CHANGED, OnActiveDirectoryFilelistChanged)
		MESSAGE_HANDLER(WM_ANOTHER_INSTANCE_QUIT, OnAnotherInstanceStarted)
		MESSAGE_HANDLER(WM_DROPFILES, OnDropFiles)
		MESSAGE_HANDLER(WM_LOAD_FILE_ASYNCH, OnLoadFileAsynch)
		MESSAGE_HANDLER(WM_REFRESHVIEW, OnRefreshView)								// Gernot (to more easily execute a refresh when window is not active)
		MESSAGE_HANDLER(WM_COPYDATA, OnCopyData)									// Gernot (for receiving file path)
		COMMAND_ID_HANDLER(IDOK, OnOK)
		COMMAND_ID_HANDLER(IDCANCEL, OnCancel)
	END_MSG_MAP()

// Handler prototypes (uncomment arguments if needed):
//	LRESULT MessageHandler(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/)
//	LRESULT CommandHandler(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/)
//	LRESULT NotifyHandler(int /*idCtrl*/, LPNMHDR /*pnmh*/, BOOL& /*bHandled*/)

	LRESULT OnInitDialog(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnOK(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnCancel(WORD /*wNotifyCode*/, WORD wID, HWND /*hWndCtl*/, BOOL& /*bHandled*/);
	LRESULT OnEraseBackground(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnPaint(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnSize(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnGetMinMaxInfo(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnNCLButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMButtonUp(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnXButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMouseMove(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnMouseWheel(UINT /*uMsg*/, WPARAM wParam, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnKeyDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnGetDlgCode(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnTimer(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnRButtonDown(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM /*lParam*/, BOOL& /*bHandled*/);
	LRESULT OnImageLoadCompleted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnAnotherInstanceStarted(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnLoadFileAsynch(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnDisplayedFileChangedOnDisk(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnActiveDirectoryFilelistChanged(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnDropFiles(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);
	LRESULT OnRefreshView(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);				// Gernot
	LRESULT OnCopyData(UINT /*uMsg*/, WPARAM /*wParam*/, LPARAM lParam, BOOL& /*bHandled*/);				// Gernot

	// Called by main()
	void SetStartupFile(LPCTSTR sStartupFile) { m_sStartupFile = sStartupFile; }

	// Called by the different controller classes
	HWND GetHWND() { return m_hWnd; }
	double GetZoom() { return m_dZoom; }
	LPCTSTR CurrentFileName();
	CFileList* GetFileList() { return m_pFileList; }
	const CRect& ClientRect() { return m_clientRect; }
	const CRect& MonitorRect() { return m_monitorRect; }
	const CSize& VirtualImageSize() { return m_virtualImageSize; }

	void UpdateWindowTitle();
	void MouseOff();
	void MouseOn();
	void GotoImage(EImagePosition ePos);
	bool PerformPan(int dx, int dy, bool bAbsolute);
	bool ScreenToImage(float & fX, float & fY); 
	bool ImageToScreen(float & fX, float & fY);
	void ExecuteCommand(int nCommand);
	//Helpers::ETransitionEffect GetTransitionEffect() { return m_eTransitionEffect; }

private:

	CString m_sStartupFile; // file passed on command line
	Helpers::ESorting m_eForcedSorting; // forced sorting mode on command line
	CFileList* m_pFileList; // used for navigation
	CDirectoryWatcher* m_pDirectoryWatcher; // notifies the main window when the current file changed or a file in the current directory was added or deleted
	CJPEGProvider * m_pJPEGProvider; // reads JPEG files (read ahead)
	CJPEGImage * m_pCurrentImage; // currently displayed image
	bool m_bOutOfMemoryLastImage; // true if the last image could not be requested because not enough memory
	bool m_bExceptionErrorLastImage; // true if the last image could not be requested because of an unhandled exception
	int m_nLastLoadError; // one of HelpersGUI::EFileLoadError
	
	// Current parameter set
	int m_nRotation; // this can only be 0, 90, 180 or 270
	bool m_bZoomMode;
	bool m_bMangaMode;
	double m_dZoom;
	bool m_bHQResampling;
	bool m_bMovieMode;
	
	CPoint m_offsets; // Note: These offsets are center of image based
	CPoint m_offsets_custom;
	int m_nMouseX, m_nMouseY;
	bool m_bFullScreenMode;
	bool m_bLockPaint;
	int m_nCurrentTimeout;
	POINT m_startMouse;
	CSize m_virtualImageSize;
	bool m_bInLowQTimer;
	bool m_bPanTimerActive;
	bool m_bTemporaryLowQ;
	bool m_bSpanVirtualDesktop;
	bool m_bMouseOn;
	double m_dLastImageDisplayTime;
	int m_nMangaSinglePageVisibleHeight;

	HMODULE m_hmodDwmapi;
	BOOL m_bDWMenabled;
	typedef HRESULT (WINAPI *MyDwmIsCompositionEnabledType)(BOOL*);
	typedef HRESULT (WINAPI *MyDwmFlushType)(void);
	MyDwmFlushType m_DynDwmFlush;

    bool m_bIsAnimationPlaying;
	int m_nLastAnimationOffset;
	int m_nExpectedNextAnimationTickCount;
	WINDOWPLACEMENT m_storedWindowPlacement;
	WINDOWPLACEMENT m_storedWindowPlacement2;
	CRect m_monitorRect;
	CRect m_clientRect;
    DWORD m_nLastSlideShowImageTickCount;

	void OpenFile(LPCTSTR sFileName, bool bAfterStartup);
	void GotoImage(EImagePosition ePos, int nFlags);
	void DeleteImageShown();
	void PerformZoom(double dValue, bool bZoomToMouse);
	double GetZoomFactorForFitToWindow();

	// Gets the image size to be used when fitting the image to screen, either using 'fit to screen'
	// or 'fill with crop' method. If 'fill with crop' is used, the bLimitAR can be set to avoid
	// filling when to less pixels remain visible
	// Outputs also the zoom factor to resize to this new size.
	// nWidth, nHeight are the original image width and height
	double ConditionalZoomFactor();

	// If dZoom > 0: Gets the virtual image size when applying the given zoom factor
	// If dZoom < 0: Gets the image size to fit the image to the given screen size, using the given auto zoom mode.
	//               dZoom is filled with the used zoom factor to resize to this new size in this case.
	// The returned size is limited to 65535 in cx and cy
	CSize GetVirtualImageSize();

	CProcessParams CreateProcessParams(bool ToPreviousImage);
	void StartSlideShowTimer(int nMilliSeconds);
	void StopSlideShowTimer(void);
	void StartMovieMode(double dFPS);
	void StopMovieMode();
	void StartLowQTimer(int nTimeout);
	//void SaveBookmark();
	void AfterNewImageLoaded();
	CRect ScreenToDIB(const CSize& sizeDIB, const CRect& rect);
	int GetLoadErrorAfterOpenFile();
	bool UseSlideShowTransitionEffect();
	void AnimateTransition();
    void CleanupAndTeminate();
    // this is for animated GIFs
    void StartAnimation();
    void AdjustAnimationFrameTime();
    void StopAnimation();
	BOOL Is64BitOS();
	CString ReplaceNoCase(LPCTSTR instr,LPCTSTR oldstr,LPCTSTR newstr);
};
