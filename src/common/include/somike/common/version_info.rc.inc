// Shared VERSIONINFO resource. Include from a target's .rc after defining:
//   SOMIKE_FILE_DESCRIPTION, SOMIKE_INTERNAL_NAME, SOMIKE_ORIGINAL_FILENAME,
//   SOMIKE_FILE_TYPE (VFT_APP or VFT_DLL)
#include <winver.h>
#include "somike/version.h"

VS_VERSION_INFO VERSIONINFO
 FILEVERSION     SOMIKE_VERSION_MAJOR, SOMIKE_VERSION_MINOR, SOMIKE_VERSION_PATCH, 0
 PRODUCTVERSION  SOMIKE_VERSION_MAJOR, SOMIKE_VERSION_MINOR, SOMIKE_VERSION_PATCH, 0
 FILEFLAGSMASK   VS_FFI_FILEFLAGSMASK
 FILEFLAGS       0
 FILEOS          VOS_NT_WINDOWS32
 FILETYPE        SOMIKE_FILE_TYPE
 FILESUBTYPE     VFT2_UNKNOWN
BEGIN
    BLOCK "StringFileInfo"
    BEGIN
        BLOCK "040904b0"
        BEGIN
            VALUE "CompanyName",      "Somike contributors"
            VALUE "FileDescription",  SOMIKE_FILE_DESCRIPTION
            VALUE "FileVersion",      SOMIKE_VERSION_STRING
            VALUE "InternalName",     SOMIKE_INTERNAL_NAME
            VALUE "OriginalFilename", SOMIKE_ORIGINAL_FILENAME
            VALUE "ProductName",      "Somike"
            VALUE "ProductVersion",   SOMIKE_VERSION_STRING
        END
    END
    BLOCK "VarFileInfo"
    BEGIN
        VALUE "Translation", 0x409, 1200
    END
END
