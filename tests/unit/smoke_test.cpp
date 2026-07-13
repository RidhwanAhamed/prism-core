#include <gtest/gtest.h>

#include "pce/version.h"
#include "pgae/version.h"
#include "psv/version.h"

// Task 0 acceptance: ctest runs green and all three module targets link.
// Real coverage starts in Task 1 (PSV round-trip, bounds, cold start).

TEST(Scaffold, ModulesLinkAndReportVersion) {
  EXPECT_STREQ(prism::psv::version(), "0.1.0");
  EXPECT_STREQ(prism::pce::version(), "0.1.0");
  EXPECT_STREQ(prism::pgae::version(), "0.1.0");
}
