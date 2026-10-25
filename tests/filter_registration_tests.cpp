#include "filter/registration_status.h"
#include <gtest/gtest.h>
#include <string_view>

TEST(FilterRegistration, PositiveWin32ErrorsRemainFailures)
{
  EXPECT_EQ(wpce::filter::registration_hresult(ERROR_SUCCESS), S_OK);
  EXPECT_TRUE(FAILED(wpce::filter::registration_hresult(ERROR_ACCESS_DENIED)));
  EXPECT_EQ(wpce::filter::registration_hresult(ERROR_ACCESS_DENIED), HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
  EXPECT_TRUE(FAILED(wpce::filter::registration_hresult(ERROR_WRITE_FAULT)));
}
TEST(FilterRegistration, UsesTheMachineComClassRegistry)
{
  EXPECT_EQ(std::wstring_view(wpce::filter::kClsidRegistryRoot), L"SOFTWARE\\Classes\\CLSID\\");
}
