#include "gtest/gtest.h"
#include <RInside.h>

TEST(libpressio_r_metric, integation1) {
  RInside R(0, nullptr);              // create an embedded R instance
  R["txt"] = "Hello, world!\n";       // assign a char* (string) to 'txt'
  R.parseEvalQ("cat(txt)");           // eval the init string, ignoring any returns
}
