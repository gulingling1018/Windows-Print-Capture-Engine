#include <windows.h>
#include <initguid.h>

// {B1C8E3F0-2D4A-5B6C-7D8E-9F0A1B2C3D4E}
DEFINE_GUID(CLSID_XpsCaptureFilter_guid, 0xb1c8e3f0, 0x2d4a, 0x5b6c, 0x7d, 0x8e, 0x9f, 0x0a, 0x1b, 0x2c, 0x3d, 0x4e);

namespace wpce::filter
{

extern const CLSID CLSID_XpsCaptureFilter = CLSID_XpsCaptureFilter_guid;

}  // namespace wpce::filter
