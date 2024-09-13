#pragma once

#include "Helpers.h"
#include "FileList.h"
#include "ProcessParams.h"
#include <string>
#include "HashCompareLPCTSTR.h"
#include <hash_map>

typedef stdext::hash_map<LPCTSTR, LPCTSTR, CHashCompareLPCTSTR> IniHashMap;

// INI settings
// All settings are first searched in a file name JPEGView.ini located in User/AppData/Roaming/JPEGView
// If not found there, the setting is searched in JPEGView.ini located in the EXE path of the application
class CSettingsProvider
{
public:
	// Singleton instance
	static CSettingsProvider& This();

	Helpers::CPUType AlgorithmImplementation() { return m_eCPUAlgorithm; }
	int NumberOfCoresToUse() { return m_nNumCores; }
	int MangaSinglePageVisibleHeight() { return m_nMangaSinglePageVisibleHeight; }
	EFilterType DownsamplingFilter() { return m_eDownsamplingFilter; }
	Helpers::ESorting Sorting() { return m_eSorting; }
	bool IsSortedUpcounting() { return m_bIsSortedUpcounting; }
	bool WrapAroundFolder() { return m_bWrapAroundFolder; }
	Helpers::ENavigationMode Navigation() { return m_eNavigation; }
	bool ForceGDIPlus() { return m_bForceGDIPlus; }
	COLORREF ColorBackground() { return m_colorBackground; }
	COLORREF ColorTransparency() { return m_colorTransparency; }
	bool AutoRotateEXIF() { return m_bAutoRotateEXIF; }
	bool ReloadWhenDisplayedImageChanged() { return m_bReloadWhenDisplayedImageChanged; }
	bool UseEmbeddedColorProfiles() { return m_bUseEmbeddedColorProfiles; }
	LPCTSTR FilesProcessedByWIC() { return m_sFilesProcessedByWIC; }
	LPCTSTR FileEndingsRAW() { return m_sFileEndingsRAW; }

	// Returns if a user INI file exists
	bool ExistsUserINI();

	// Gets the path where the global INI file and the EXE is located
	LPCTSTR GetEXEPath() { return m_sEXEPath; }
	// Get the file name with path of the global INI file (in EXE path)
	LPCTSTR GetGlobalINIFileName() { return m_sIniNameGlobal; }
	// Get the file name with path of the user INI file (in AppData path)
	LPCTSTR GetUserINIFileName() { return m_sIniNameUser; }

private:
	static CSettingsProvider* sm_instance;
	CString m_sEXEPath;
	CString m_sIniNameGlobal;
	CString m_sIniNameUser;
	bool m_bUserINIExists;

	bool m_bStoreToEXEPath;
	Helpers::CPUType m_eCPUAlgorithm;
	int m_nNumCores;
	int m_nMangaSinglePageVisibleHeight;
	EFilterType m_eDownsamplingFilter;
	Helpers::ESorting m_eSorting;
	bool m_bIsSortedUpcounting;
	bool m_bWrapAroundFolder;
	Helpers::ENavigationMode m_eNavigation;
	COLORREF m_colorBackground;
	COLORREF m_colorTransparency;
	bool m_bForceGDIPlus;
	bool m_bUseEmbeddedColorProfiles;
	bool m_bAutoRotateEXIF;
	bool m_bReloadWhenDisplayedImageChanged;
	CString m_sFilesProcessedByWIC;
	CString m_sFileEndingsRAW;

	TCHAR* m_pIniGlobalSectionBuffer;
	TCHAR* m_pIniUserSectionBuffer;
	IniHashMap* m_pGlobalKeys;
	IniHashMap* m_pUserKeys;

	void MakeSureUserINIExists();
	CString ReplacePlaceholdersFileNameFormat(const CString& sFileNameFormat);

	LPCTSTR ReadUserIniString(LPCTSTR key);
	LPCTSTR ReadGlobalIniString(LPCTSTR key);
	LPCTSTR ReadIniString(LPCTSTR key, LPCTSTR fileName, IniHashMap*& keyMap, TCHAR*& pBuffer);
	void ReadIniFile(LPCTSTR fileName, IniHashMap* keyMap, TCHAR*& pBuffer);

	CString GetString(LPCTSTR sKey, LPCTSTR sDefault);
	int GetInt(LPCTSTR sKey, int nDefault, int nMin, int nMax);
	double GetDouble(LPCTSTR sKey, double dDefault, double dMin, double dMax);
	bool GetBool(LPCTSTR sKey, bool bDefault);
	CRect GetRect(LPCTSTR sKey, const CRect& defaultRect);
	CSize GetSize(LPCTSTR sKey, const CSize& defaultSize);
	COLORREF GetColor(LPCTSTR sKey, COLORREF defaultColor);
	Helpers::EAutoZoomMode GetAutoZoomMode(LPCTSTR sKey, Helpers::EAutoZoomMode defaultZoomMode);
	LPCTSTR GetAutoZoomModeString(Helpers::EAutoZoomMode autoZoomMode);
	void WriteString(LPCTSTR sKey, LPCTSTR sString);
	void WriteDouble(LPCTSTR sKey, double dValue);
	void WriteBool(LPCTSTR sKey, bool bValue);
	void WriteInt(LPCTSTR sKey, int nValue);

	CSettingsProvider(void);
};
