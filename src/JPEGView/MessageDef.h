#pragma once

// Message posted when image (of any format, not only JPEG) has finished loading.
// LPARAM is the request handle to retrieve the image
#define WM_IMAGE_LOAD_COMPLETED (WM_APP + 6)

// Message posted when the currently shown image has been changed on disk and needs to be reloaded
#define WM_DISPLAYED_FILE_CHANGED_ON_DISK (WM_APP + 7)

// Message posted when the files in the current directory have been added or removed and thus the
// list of images in the directory needs to be reloaded
#define WM_ACTIVE_DIRECTORY_FILELIST_CHANGED (WM_APP + 8)

#define WM_ANOTHER_INSTANCE_QUIT (WM_APP + 23)

// Posted to main dialog for asychnonously loading the image with file name CMainDlg::m_sStartupFile
#define WM_LOAD_FILE_ASYNCH (WM_APP + 24)

// Gernot (to more easily execute a refresh when window is not active)
#define WM_REFRESHVIEW (WM_APP + 25)

#define KEY_MAGIC 2978465