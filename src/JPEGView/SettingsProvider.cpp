#include "StdAfx.h"
#include "SettingsProvider.h"
#include "NLS.h"
#include <float.h>
#include <shlobj.h>
#include <algorithm>

static const TCHAR* DEFAULT_INI_FILE_NAME = _T("JPEGView.ini");
static const TCHAR* SECTION_NAME = _T("JPEGView");

CSettingsProvider* CSettingsProvider::sm_instance;

static CString ParseCommandLineForIniName(LPCTSTR sCommandLine) {
	if (sCommandLine == NULL) {
		return CString();
	}
	LPCTSTR iniFile = Helpers::stristr(sCommandLine, _T("/ini"));
	if (iniFile == NULL) {
		return CString();
	}
	iniFile = iniFile + _tcslen(_T("/ini")) + 1;
	LPCTSTR posSpace = _tcschr(iniFile, _T(' '));
	CString iniFileName = (posSpace == NULL) ? CString(iniFile) : CString(iniFile, (int)(posSpace - iniFile));
	return iniFileName;
}

CSettingsProvider& CSettingsProvider::This() {
	if (sm_instance == NULL) {
		sm_instance = new CSettingsProvider();
	}
	return *sm_instance;
}

CSettingsProvider::CSettingsProvider(void)
	{
	// parse command line to find the process startup path
	LPTSTR pCmdLine = ::GetCommandLine();
	LPTSTR pStart = pCmdLine;
	bool bQuot = false;
	if (pCmdLine[0] == _T('"')) {
		pStart++;
		bQuot = true;
	}
	LPTSTR pEnd = pStart;
	int nLastIdx = -1;
	int nIdx = 0;
	while (((bQuot && *pEnd != _T('"')) || (!bQuot && *pEnd != _T(' '))) && *pEnd != 0) {
		if (*pEnd == _T('\\')) {
			nLastIdx = nIdx;
		}
		pEnd++;
		nIdx++;
	}

	if (nIdx > 0 && nLastIdx > 0) {
		m_sEXEPath = CString(pStart, nLastIdx+1);
	} else {
		m_sEXEPath = _T(".\\");
	}

	// Global INI file (at EXE location)
	m_sIniNameGlobal = m_sEXEPath + DEFAULT_INI_FILE_NAME;

	// Read if the user INI file shall be used
	m_bUserINIExists = false;
	m_pIniGlobalSectionBuffer = NULL;
	m_pIniUserSectionBuffer = NULL;
	m_pGlobalKeys = NULL;
	m_pUserKeys = NULL;
	m_sIniNameUser = m_sIniNameGlobal;
	m_bStoreToEXEPath = GetBool(_T("StoreToEXEPath"), false);

	if (!m_bStoreToEXEPath) {
		// User INI file
		m_sIniNameUser = CString(Helpers::JPEGViewAppDataPath()) + DEFAULT_INI_FILE_NAME;
		MakeSureUserINIExists();
		m_bUserINIExists = (::GetFileAttributes(m_sIniNameUser) != INVALID_FILE_ATTRIBUTES);
	} else {
		Helpers::SetJPEGViewAppDataPath(m_sEXEPath);
	}

	//----------------------------------------------------------------------------------------------

/*GF*/	TCHAR debugtext[512];

	CString sCPU = GetString(_T("CPUType"), _T("AutoDetect"));
	if (sCPU.CompareNoCase(_T("Generic")) == 0) {
		m_eCPUAlgorithm = Helpers::CPU_Generic;
	}
	else if (sCPU.CompareNoCase(_T("SSE")) == 0) {
		m_eCPUAlgorithm = Helpers::CPU_SSE;
	}
	else if (sCPU.CompareNoCase(_T("AVX2")) == 0) {
		m_eCPUAlgorithm = Helpers::CPU_AVX2;
	}
	else {
		m_eCPUAlgorithm = Helpers::ProbeCPU();
	}
/*GF*/	swprintf(debugtext,255,TEXT("m_eCPUAlgorithm: %d (0=unknown, 1=generic, 2=sse, 3=avx2)"), m_eCPUAlgorithm);
/*GF*/	::OutputDebugStringW(debugtext);


	m_nNumCores = GetInt(_T("CPUCoresUsed"), 0, 0, 128);
	if (m_nNumCores == 0) {
		m_nNumCores = Helpers::NumConcurrentThreads();
/*GF*/	swprintf(debugtext,255,TEXT("m_nNumCores: %d (number of concurrent threads supported by the CPU)"), m_nNumCores);
/*GF*/	::OutputDebugStringW(debugtext);
	}

/*GF*/	m_nMangaSinglePageVisibleHeight = GetInt(_T("MangaSinglePageVisibleHeight"), 75, 1, 100);

	CString sDownSampling = GetString(_T("DownSamplingFilter"), _T("Catrom"));
	if (sDownSampling.CompareNoCase(_T("None")) == 0) {
		m_eDownsamplingFilter = Filter_Downsampling_None;
	}
	else if (sDownSampling.CompareNoCase(_T("Hermite")) == 0) {
		m_eDownsamplingFilter = Filter_Downsampling_Hermite;
	}
	else if (sDownSampling.CompareNoCase(_T("Mitchell")) == 0) {
		m_eDownsamplingFilter = Filter_Downsampling_Mitchell;
	}
	else if (sDownSampling.CompareNoCase(_T("Catrom")) == 0) {
		m_eDownsamplingFilter = Filter_Downsampling_Catrom;
	}
	else if (sDownSampling.CompareNoCase(_T("Lanczos2")) == 0) {
		m_eDownsamplingFilter = Filter_Downsampling_Lanczos2;
	}
	else {
		m_eDownsamplingFilter = Filter_Downsampling_Catrom;
	}
/*GF*/	swprintf(debugtext,255,TEXT("m_eDownsamplingFilter: %d (0=none, 1=hermite, 2=mitchell, 3=catrom, 4=lanczos2)"), m_eDownsamplingFilter);
/*GF*/	::OutputDebugStringW(debugtext);

	//----------------------------------------------------------------------------------------

	CString sSorting = GetString(_T("SortOrder"), _T("FileName"));

	if (sSorting.CompareNoCase(_T("CreationDate")) == 0) {
		m_eSorting = Helpers::FS_CreationTime;
	}
	else if (sSorting.CompareNoCase(_T("LastModDate")) == 0) {
		m_eSorting = Helpers::FS_LastModTime;
	}
	else if (sSorting.CompareNoCase(_T("FileName")) == 0) {
		m_eSorting = Helpers::FS_FileName;
	}
	else if (sSorting.CompareNoCase(_T("Random")) == 0) {
		m_eSorting = Helpers::FS_Random;
	}
	else if (sSorting.CompareNoCase(_T("FileSize")) == 0) {
		m_eSorting = Helpers::FS_FileSize;
	}
	else {
		m_eSorting = Helpers::FS_FileName;
	}

	m_bIsSortedUpcounting = GetBool(_T("IsSortedUpcounting"), true);

	m_bWrapAroundFolder = GetBool(_T("WrapAroundFolder"), true);

	CString sNavigation = GetString(_T("FolderNavigation"), _T("LoopFolder"));
	if (sNavigation.CompareNoCase(_T("LoopSameFolderLevel")) == 0) {
		m_eNavigation = Helpers::NM_LoopSameDirectoryLevel;
	}
	else if (sNavigation.CompareNoCase(_T("LoopSubFolders")) == 0) {
		m_eNavigation = Helpers::NM_LoopSubDirectories;
	}
	else {
		m_eNavigation = Helpers::NM_LoopDirectory;
	}

	//----------------------------------------------------------------------------------------

	m_colorBackground = GetColor(_T("BackgroundColor"), RGB(0, 0, 0));

	m_colorTransparency = GetColor(_T("TransparencyColor"), RGB(255, 0, 255));	

	m_bForceGDIPlus = GetBool(_T("ForceGDIPlus"), false);

	m_bUseEmbeddedColorProfiles = GetBool(_T("UseEmbeddedColorProfiles"), false);

	m_bAutoRotateEXIF = GetBool(_T("AutoRotateEXIF"), true);
	
	m_bReloadWhenDisplayedImageChanged = GetBool(_T("ReloadWhenDisplayedImageChanged"), true);

	m_sFilesProcessedByWIC = GetString(_T("FilesProcessedByWIC"), _T("*.wdp;*.mdp;*.hdp"));

	m_sFileEndingsRAW = GetString(_T("FileEndingsRAW"), _T("*.pef;*.dng;*.crw;*.nef;*.cr2;*.mrw;*.rw2;*.orf;*.x3f;*.arw;*.kdc;*.nrw;*.dcr;*.sr2;*.raf"));
	}


//################################################################################
//################################################################################
//################################################################################

bool CSettingsProvider::ExistsUserINI() {
	LPCTSTR sINIFileName = GetUserINIFileName();
	return ::GetFileAttributes(sINIFileName) != INVALID_FILE_ATTRIBUTES;
}

void CSettingsProvider::MakeSureUserINIExists() {
	if (m_bStoreToEXEPath) {
		return; // no user INI file needed
	}

	// Create JPEGView appdata directory and copy INI file if it does not exist
	::CreateDirectory(Helpers::JPEGViewAppDataPath(), NULL);
	if (::GetFileAttributes(m_sIniNameUser) == INVALID_FILE_ATTRIBUTES) {
		::CopyFile(m_sIniNameGlobal, m_sIniNameUser, TRUE);
	}
}

LPCTSTR CSettingsProvider::ReadUserIniString(LPCTSTR key) {
	return ReadIniString(key, m_sIniNameUser, m_pUserKeys, m_pIniUserSectionBuffer);
}

LPCTSTR CSettingsProvider::ReadGlobalIniString(LPCTSTR key) {
	return ReadIniString(key, m_sIniNameGlobal, m_pGlobalKeys, m_pIniGlobalSectionBuffer);
}

LPCTSTR CSettingsProvider::ReadIniString(LPCTSTR key, LPCTSTR fileName, IniHashMap*& keyMap, TCHAR*& pBuffer) {
	if (keyMap == NULL) {
		keyMap = new IniHashMap();
		ReadIniFile(m_sIniNameUser, keyMap, pBuffer);
	}
	hash_map<LPCTSTR, LPCTSTR, CHashCompareLPCTSTR>::const_iterator iter;
	iter = keyMap->find(key);
	if (iter == keyMap->end()) {
		return NULL; // not found
	}
	else {
		return iter->second;
	}
}

void CSettingsProvider::ReadIniFile(LPCTSTR fileName, IniHashMap* keyMap, TCHAR*& pBuffer) {
	int bufferSize = 1024 * 2;

	pBuffer = NULL;
	int actualSize;
	do {
		delete[] pBuffer;
		bufferSize = bufferSize * 2;
		pBuffer = new TCHAR[bufferSize];
		actualSize = ::GetPrivateProfileSection(SECTION_NAME, pBuffer, bufferSize, fileName);
	} while (actualSize == bufferSize - 2);

	int index = 0;
	LPTSTR current = pBuffer;
	while (*current != 0) {
		while (*current != 0 && _istspace(*current)) current++;
		LPCTSTR key = current;
		while (*current != 0 && !_istspace(*current) && *current != _T('=')) current++;
		LPCTSTR value = current;
		if (*current != 0) {
			*current++ = 0;
			while (*current != 0 && _istspace(*current)) current++;
			value = current;
		}
		if (*key != 0) {
			(*keyMap)[key] = value;
		}
		current += _tcslen(value) + 1;
	}
}

CString CSettingsProvider::GetString(LPCTSTR sKey, LPCTSTR sDefault) {
	if (m_bUserINIExists) {
		// try first user path
		LPCTSTR value = ReadUserIniString(sKey);
		if (value != NULL) {
			return CString(value);
		}
	}
	// finally global path if not found in user path
	LPCTSTR value = ReadGlobalIniString(sKey);
	if (value == NULL) {
		return CString(sDefault);
	}
	return CString(value);
}

int CSettingsProvider::GetInt(LPCTSTR sKey, int nDefault, int nMin, int nMax) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return nDefault;
	}
	int nValue = (int)_wtof((LPCTSTR)s);
	return min(nMax, max(nMin, nValue));
}

double CSettingsProvider::GetDouble(LPCTSTR sKey, double dDefault, double dMin, double dMax) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return dDefault;
	}
	double dValue = _wtof((LPCTSTR)s);
	return min(dMax, max(dMin, dValue));
}

bool CSettingsProvider::GetBool(LPCTSTR sKey, bool bDefault) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return bDefault;
	}
	if (s.CompareNoCase(_T("true")) == 0) {
		return true;
	} else if (s.CompareNoCase(_T("false")) == 0) {
		return false;
	} else {
		return bDefault;
	}
}

CRect CSettingsProvider::GetRect(LPCTSTR sKey, const CRect& defaultRect) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return defaultRect;
	}
	int nLeft, nTop, nRight, nBottom;
	if (_stscanf((LPCTSTR)s, _T(" %d %d %d %d "), &nLeft, &nTop, &nRight, &nBottom) == 4) {
		CRect newRect = CRect(nLeft, nTop, nRight, nBottom);
		newRect.NormalizeRect();
		return newRect;
	} else {
		return defaultRect;
	}
}

CSize CSettingsProvider::GetSize(LPCTSTR sKey, const CSize& defaultSize) {
	CString s = GetString(sKey, _T(""));
	if (s.IsEmpty()) {
		return defaultSize;
	}
	int nWidth, nHeight;
	if (_stscanf((LPCTSTR)s, _T(" %d %d "), &nWidth, &nHeight) == 2) {
		nWidth = max(1, nWidth);
		nHeight = max(1, nHeight);
		return CSize(nWidth, nHeight);
	} else {
		return defaultSize;
	}
}

COLORREF CSettingsProvider::GetColor(LPCTSTR sKey, COLORREF defaultColor) {
	int nRed, nGreen, nBlue;
	CString sColor = GetString(sKey, _T(""));
	if (sColor.IsEmpty()) {
		return defaultColor;
	}
	if (_stscanf(sColor, _T(" %d %d %d"), &nRed, &nGreen, &nBlue) == 3) {
		return RGB(nRed, nGreen, nBlue);
	} else {
		return defaultColor;
	}
}

Helpers::EAutoZoomMode CSettingsProvider::GetAutoZoomMode(LPCTSTR sKey, Helpers::EAutoZoomMode defaultZoomMode) {
	CString sAutoZoomMode = GetString(sKey, _T("FitNoZoom"));
	if (sAutoZoomMode.CompareNoCase(_T("Fit")) == 0) {
		return Helpers::ZM_FitToScreen;
	}
	else if (sAutoZoomMode.CompareNoCase(_T("Fill")) == 0) {
		return Helpers::ZM_FillScreen;
	}
	else if (sAutoZoomMode.CompareNoCase(_T("FillNoZoom")) == 0) {
		return Helpers::ZM_FillScreenNoZoom;
	}
	else {
		return defaultZoomMode;
	}
}

LPCTSTR CSettingsProvider::GetAutoZoomModeString(Helpers::EAutoZoomMode autoZoomMode) {
	if (autoZoomMode == Helpers::ZM_FillScreen) {
		return _T("Fill");
	}
	else if (autoZoomMode == Helpers::ZM_FitToScreen) {
		return _T("Fit");
	}
	else if (autoZoomMode == Helpers::ZM_FillScreenNoZoom) {
		return _T("FillNoZoom");
	}
	return _T("FitNoZoom");
}

void CSettingsProvider::WriteString(LPCTSTR sKey, LPCTSTR sString) {
	::WritePrivateProfileString(SECTION_NAME, sKey, sString, m_sIniNameUser);
}

void CSettingsProvider::WriteDouble(LPCTSTR sKey, double dValue) {
	TCHAR buff[32];
	_stprintf_s(buff, 32, _T("%.2f"), dValue);
	WriteString(sKey, buff);
}

void CSettingsProvider::WriteBool(LPCTSTR sKey, bool bValue) {
	WriteString(sKey, bValue ? _T("true") : _T("false"));
}

void CSettingsProvider::WriteInt(LPCTSTR sKey, int nValue) {
	TCHAR buff[32];
	_stprintf_s(buff, 32, _T("%d"), nValue);
	WriteString(sKey, buff);
}
