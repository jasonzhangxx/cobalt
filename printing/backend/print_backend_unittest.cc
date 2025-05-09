// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "printing/backend/print_backend.h"

#include "base/memory/scoped_refptr.h"
#include "printing/mojom/print.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include "base/types/expected.h"
#endif  // BUILDFLAG(IS_WIN)

namespace printing {

// PrintBackendTest makes use of a real print backend instance, and thus will
// interact with printer drivers installed on a system.  This can be useful on
// machines which a developer has control over the driver installations, but is
// less useful on bots which are managed by the infra team.
// These tests are intended to be run manually by developers using the
// --run-manual flag.
class PrintBackendTest : public testing::Test {
 public:
  void SetUp() override {
    print_backend_ = PrintBackend::CreateInstance(/*locale=*/"");
  }

  PrintBackend* GetPrintBackend() { return print_backend_.get(); }

 private:
  scoped_refptr<PrintBackend> print_backend_;
};

// Check behavior of `EnumeratePrinters()`.  At least one of the tests
// {EnumeratePrintersSomeInstalled, EnumeratePrintersNoneInstalled} should
// fail, since a single machine can't have both some and no printers installed.
// A developer running these manually can verify that the appropriate test is
// passing for the given state of installed printer drivers on the system being
// checked.
TEST_F(PrintBackendTest, MANUAL_EnumeratePrintersSomeInstalled) {
  PrinterList printer_list;

  EXPECT_EQ(GetPrintBackend()->EnumeratePrinters(printer_list),
            mojom::ResultCode::kSuccess);
  EXPECT_FALSE(printer_list.empty());

  DLOG(WARNING) << "Number of printers found: " << printer_list.size();
  for (const auto& printer : printer_list) {
    DLOG(WARNING) << "Found printer: `" << printer.printer_name << "`";
  }
}

TEST_F(PrintBackendTest, MANUAL_EnumeratePrintersNoneInstalled) {
  PrinterList printer_list;

  EXPECT_EQ(GetPrintBackend()->EnumeratePrinters(printer_list),
            mojom::ResultCode::kSuccess);
  EXPECT_TRUE(printer_list.empty());
}

#if BUILDFLAG(IS_WIN)

// This test is for the XPS API that read the XML capabilities of a
// specific printer.
TEST_F(PrintBackendTest, MANUAL_GetXmlPrinterCapabilitiesForXpsDriver) {
  PrinterList printer_list;
  EXPECT_EQ(GetPrintBackend()->EnumeratePrinters(printer_list),
            mojom::ResultCode::kSuccess);
  for (const auto& printer : printer_list) {
    EXPECT_TRUE(
        GetPrintBackend()
            ->GetXmlPrinterCapabilitiesForXpsDriver(printer.printer_name)
            .has_value());
  }
}

#endif  // BUILDFLAG(IS_WIN)

}  // namespace printing
