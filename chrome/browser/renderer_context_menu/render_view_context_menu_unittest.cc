// Copyright 2014 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/renderer_context_menu/render_view_context_menu.h"

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "chrome/app/chrome_command_ids.h"
#include "chrome/browser/autofill/personal_data_manager_factory.h"
#include "chrome/browser/download/chrome_download_manager_delegate.h"
#include "chrome/browser/download/download_core_service.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/extensions/menu_manager.h"
#include "chrome/browser/extensions/menu_manager_factory.h"
#include "chrome/browser/extensions/test_extension_environment.h"
#include "chrome/browser/extensions/test_extension_prefs.h"
#include "chrome/browser/feed/web_feed_tab_helper.h"
#include "chrome/browser/password_manager/chrome_password_manager_client.h"
#include "chrome/browser/prefs/incognito_mode_prefs.h"
#include "chrome/browser/renderer_context_menu/render_view_context_menu_test_util.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/translate/chrome_translate_client.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/chrome_pages.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/webui_url_constants.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "chrome/test/base/scoped_testing_local_state.h"
#include "chrome/test/base/search_test_utils.h"
#include "chrome/test/base/test_browser_window.h"
#include "chrome/test/base/testing_browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "components/autofill/content/browser/test_autofill_client_injector.h"
#include "components/autofill/content/browser/test_autofill_driver_injector.h"
#include "components/autofill/content/browser/test_content_autofill_client.h"
#include "components/autofill/core/browser/autofill_test_utils.h"
#include "components/autofill/core/browser/personal_data_manager.h"
#include "components/autofill/core/browser/test_autofill_driver.h"
#include "components/autofill/core/common/autofill_features.h"
#include "components/custom_handlers/protocol_handler_registry.h"
#include "components/feed/feed_feature_list.h"
#include "components/lens/buildflags.h"
#include "components/lens/lens_features.h"
#include "components/policy/core/common/policy_pref_names.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "components/proxy_config/proxy_config_pref_names.h"
#include "components/search_engines/template_url_service.h"
#include "components/translate/core/browser/language_state.h"
#include "components/translate/core/browser/translate_manager.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/common/url_pattern.h"
#include "services/network/test/test_shared_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/abseil-cpp/absl/types/optional.h"
#include "third_party/blink/public/common/navigation/impression.h"
#include "third_party/blink/public/mojom/context_menu/context_menu.mojom.h"
#include "ui/base/clipboard/clipboard.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_CHROMEOS)
#include "chrome/browser/chromeos/policy/dlp/dlp_policy_constants.h"
#include "chrome/browser/chromeos/policy/dlp/dlp_rules_manager.h"
#include "chrome/browser/chromeos/policy/dlp/dlp_rules_manager_impl.h"
#include "chrome/browser/chromeos/policy/dlp/dlp_rules_manager_test_utils.h"
#endif

using extensions::Extension;
using extensions::MenuItem;
using extensions::MenuManager;
using extensions::MenuManagerFactory;
using extensions::URLPatternSet;

namespace {

// Generates a ContextMenuParams that matches the specified contexts.
static content::ContextMenuParams CreateParams(int contexts) {
  content::ContextMenuParams rv;
  rv.is_editable = false;
  rv.media_type = blink::mojom::ContextMenuDataMediaType::kNone;
  rv.page_url = GURL("http://test.page/");

  static constexpr char16_t selected_text[] = u"sel";
  if (contexts & MenuItem::SELECTION) {
    rv.selection_text = selected_text;
  }

  if (contexts & MenuItem::LINK) {
    rv.link_url = GURL("http://test.link/");
  }

  if (contexts & MenuItem::EDITABLE) {
    rv.is_editable = true;
  }

  if (contexts & MenuItem::IMAGE) {
    rv.src_url = GURL("http://test.image/");
    rv.media_type = blink::mojom::ContextMenuDataMediaType::kImage;
  }

  if (contexts & MenuItem::VIDEO) {
    rv.src_url = GURL("http://test.video/");
    rv.media_type = blink::mojom::ContextMenuDataMediaType::kVideo;
  }

  if (contexts & MenuItem::AUDIO) {
    rv.src_url = GURL("http://test.audio/");
    rv.media_type = blink::mojom::ContextMenuDataMediaType::kAudio;
  }

  if (contexts & MenuItem::FRAME) {
    rv.frame_url = GURL("http://test.frame/");
  }

  return rv;
}

// Returns a test context menu.
std::unique_ptr<TestRenderViewContextMenu> CreateContextMenu(
    content::WebContents* web_contents,
    custom_handlers::ProtocolHandlerRegistry* registry) {
  content::ContextMenuParams params = CreateParams(MenuItem::LINK);
  params.unfiltered_link_url = params.link_url;
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents->GetPrimaryMainFrame(), params);
  menu->set_protocol_handler_registry(registry);
  menu->Init();
  return menu;
}

class TestNavigationDelegate : public content::WebContentsDelegate {
 public:
  TestNavigationDelegate() = default;
  ~TestNavigationDelegate() override = default;

  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params) override {
    last_navigation_params_ = params;
    return nullptr;
  }

  const absl::optional<content::OpenURLParams>& last_navigation_params() {
    return last_navigation_params_;
  }

 private:
  absl::optional<content::OpenURLParams> last_navigation_params_;
};

#if BUILDFLAG(IS_CHROMEOS)
class MockDlpRulesManager : public policy::DlpRulesManagerImpl {
 public:
  explicit MockDlpRulesManager(PrefService* local_state)
      : DlpRulesManagerImpl(local_state) {}
};
#endif

}  // namespace

class RenderViewContextMenuTest : public testing::Test {
 protected:
  RenderViewContextMenuTest()
      : RenderViewContextMenuTest(
            std::unique_ptr<extensions::TestExtensionEnvironment>()) {}

  // If the test uses a TestExtensionEnvironment, which provides a MessageLoop,
  // it needs to be passed to the constructor so that it exists before the
  // RenderViewHostTestEnabler which needs to use the MessageLoop.
  explicit RenderViewContextMenuTest(
      std::unique_ptr<extensions::TestExtensionEnvironment> env)
      : environment_(std::move(env)) {
    // TODO(mgiuca): Add tests with DesktopPWAs enabled.
  }

  RenderViewContextMenuTest(const RenderViewContextMenuTest&) = delete;
  RenderViewContextMenuTest& operator=(const RenderViewContextMenuTest&) =
      delete;

  // Proxy defined here to minimize friend classes in RenderViewContextMenu
  static bool ExtensionContextAndPatternMatch(
      const content::ContextMenuParams& params,
      MenuItem::ContextList contexts,
      const URLPatternSet& patterns) {
    return RenderViewContextMenu::ExtensionContextAndPatternMatch(
        params, contexts, patterns);
  }

  // Returns a test item.
  std::unique_ptr<MenuItem> CreateTestItem(const Extension* extension,
                                           int uid) {
    MenuItem::Type type = MenuItem::NORMAL;
    MenuItem::ContextList contexts(MenuItem::ALL);
    const MenuItem::ExtensionKey key(extension->id());
    bool incognito = false;
    MenuItem::Id id(incognito, key);
    id.uid = uid;
    return std::make_unique<MenuItem>(id, "Added by an extension", false, true,
                                      true, type, contexts);
  }

 protected:
  std::unique_ptr<extensions::TestExtensionEnvironment> environment_;

 private:
  content::RenderViewHostTestEnabler rvh_test_enabler_;
};

// Generates a URLPatternSet with a single pattern
static URLPatternSet CreatePatternSet(const std::string& pattern) {
  URLPattern target(URLPattern::SCHEME_HTTP);
  target.Parse(pattern);

  URLPatternSet rv;
  rv.AddPattern(target);

  return rv;
}

TEST_F(RenderViewContextMenuTest, TargetIgnoredForPage) {
  content::ContextMenuParams params = CreateParams(0);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::PAGE);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetCheckedForLink) {
  content::ContextMenuParams params = CreateParams(MenuItem::LINK);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::PAGE);
  contexts.Add(MenuItem::LINK);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_FALSE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetCheckedForImage) {
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::PAGE);
  contexts.Add(MenuItem::IMAGE);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_FALSE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetCheckedForVideo) {
  content::ContextMenuParams params = CreateParams(MenuItem::VIDEO);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::PAGE);
  contexts.Add(MenuItem::VIDEO);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_FALSE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetCheckedForAudio) {
  content::ContextMenuParams params = CreateParams(MenuItem::AUDIO);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::PAGE);
  contexts.Add(MenuItem::AUDIO);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_FALSE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, MatchWhenLinkedImageMatchesTarget) {
  content::ContextMenuParams params =
      CreateParams(MenuItem::IMAGE | MenuItem::LINK);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::LINK);
  contexts.Add(MenuItem::IMAGE);

  URLPatternSet patterns = CreatePatternSet("*://test.link/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, MatchWhenLinkedImageMatchesSource) {
  content::ContextMenuParams params =
      CreateParams(MenuItem::IMAGE | MenuItem::LINK);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::LINK);
  contexts.Add(MenuItem::IMAGE);

  URLPatternSet patterns = CreatePatternSet("*://test.image/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, NoMatchWhenLinkedImageMatchesNeither) {
  content::ContextMenuParams params =
      CreateParams(MenuItem::IMAGE | MenuItem::LINK);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::LINK);
  contexts.Add(MenuItem::IMAGE);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_FALSE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetIgnoredForFrame) {
  content::ContextMenuParams params = CreateParams(MenuItem::FRAME);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::FRAME);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetIgnoredForEditable) {
  content::ContextMenuParams params = CreateParams(MenuItem::EDITABLE);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::EDITABLE);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetIgnoredForSelection) {
  content::ContextMenuParams params = CreateParams(MenuItem::SELECTION);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::SELECTION);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetIgnoredForSelectionOnLink) {
  content::ContextMenuParams params =
      CreateParams(MenuItem::SELECTION | MenuItem::LINK);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::SELECTION);
  contexts.Add(MenuItem::LINK);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

TEST_F(RenderViewContextMenuTest, TargetIgnoredForSelectionOnImage) {
  content::ContextMenuParams params =
      CreateParams(MenuItem::SELECTION | MenuItem::IMAGE);

  MenuItem::ContextList contexts;
  contexts.Add(MenuItem::SELECTION);
  contexts.Add(MenuItem::IMAGE);

  URLPatternSet patterns = CreatePatternSet("*://test.none/*");

  EXPECT_TRUE(ExtensionContextAndPatternMatch(params, contexts, patterns));
}

class RenderViewContextMenuExtensionsTest : public RenderViewContextMenuTest {
 protected:
  RenderViewContextMenuExtensionsTest()
      : RenderViewContextMenuTest(
            std::make_unique<extensions::TestExtensionEnvironment>()) {}

  RenderViewContextMenuExtensionsTest(
      const RenderViewContextMenuExtensionsTest&) = delete;
  RenderViewContextMenuExtensionsTest& operator=(
      const RenderViewContextMenuExtensionsTest&) = delete;

  void SetUp() override {
    RenderViewContextMenuTest::SetUp();
    // TestingProfile does not provide a protocol registry.
    registry_ = std::make_unique<custom_handlers::ProtocolHandlerRegistry>(
        profile()->GetPrefs(), nullptr);
  }

  void TearDown() override {
    registry_.reset();
    RenderViewContextMenuTest::TearDown();
  }

  TestingProfile* profile() const { return environment_->profile(); }

  extensions::TestExtensionEnvironment& environment() { return *environment_; }

 protected:
  std::unique_ptr<custom_handlers::ProtocolHandlerRegistry> registry_;
};

TEST_F(RenderViewContextMenuExtensionsTest,
       ItemWithSameTitleFromTwoExtensions) {
  MenuManager* menu_manager =  // Owned by profile().
      static_cast<MenuManager*>(
          (MenuManagerFactory::GetInstance()->SetTestingFactoryAndUse(
              profile(),
              base::BindRepeating(
                  &MenuManagerFactory::BuildServiceInstanceForTesting))));

  const Extension* extension1 = environment().MakeExtension(
      base::Value::Dict(), "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  const Extension* extension2 = environment().MakeExtension(
      base::Value::Dict(), "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");

  // Create two items in two extensions with same title.
  ASSERT_TRUE(
      menu_manager->AddContextItem(extension1, CreateTestItem(extension1, 1)));
  ASSERT_TRUE(
      menu_manager->AddContextItem(extension2, CreateTestItem(extension2, 2)));

  std::unique_ptr<content::WebContents> web_contents = environment().MakeTab();
  std::unique_ptr<TestRenderViewContextMenu> menu(
      CreateContextMenu(web_contents.get(), registry_.get()));

  const ui::MenuModel& model = menu->menu_model();
  std::u16string expected_title = u"Added by an extension";
  int num_items_found = 0;
  for (size_t i = 0; i < model.GetItemCount(); ++i) {
    if (expected_title == model.GetLabelAt(i)) {
      ++num_items_found;
    }
  }

  // Expect both items to be found.
  ASSERT_EQ(2, num_items_found);
}

class RenderViewContextMenuPrefsTest : public ChromeRenderViewHostTestHarness {
 public:
  RenderViewContextMenuPrefsTest() = default;

  RenderViewContextMenuPrefsTest(const RenderViewContextMenuPrefsTest&) =
      delete;
  RenderViewContextMenuPrefsTest& operator=(
      const RenderViewContextMenuPrefsTest&) = delete;

  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    registry_ = std::make_unique<custom_handlers::ProtocolHandlerRegistry>(
        profile()->GetPrefs(), nullptr);

    TemplateURLServiceFactory::GetInstance()->SetTestingFactoryAndUse(
        profile(),
        base::BindRepeating(&TemplateURLServiceFactory::BuildInstanceFor));
    template_url_service_ = TemplateURLServiceFactory::GetForProfile(profile());
    search_test_utils::WaitForTemplateURLServiceToLoad(template_url_service_);

    // Set up policies.
    testing_local_state_ = std::make_unique<ScopedTestingLocalState>(
        TestingBrowserProcess::GetGlobal());
    local_state()->SetBoolean(prefs::kAllowFileSelectionDialogs, true);
    DownloadCoreServiceFactory::GetForBrowserContext(profile())
        ->SetDownloadManagerDelegateForTesting(
            std::make_unique<ChromeDownloadManagerDelegate>(profile()));
  }

  void TearDown() override {
    browser_.reset();
    registry_.reset();
    ChromeRenderViewHostTestHarness::TearDown();
    testing_local_state_.reset();
  }

  std::unique_ptr<TestRenderViewContextMenu> CreateContextMenu() {
    return ::CreateContextMenu(web_contents(), registry_.get());
  }

  // Returns a test context menu for a chrome:// url not permitted to open in
  // incognito mode.
  std::unique_ptr<TestRenderViewContextMenu> CreateContextMenuOnChromeLink() {
    content::ContextMenuParams params = CreateParams(MenuItem::LINK);
    params.unfiltered_link_url = params.link_url =
        GURL(chrome::kChromeUISettingsURL);
    auto menu = std::make_unique<TestRenderViewContextMenu>(
        *web_contents()->GetPrimaryMainFrame(), params);
    menu->set_protocol_handler_registry(registry_.get());
    menu->Init();
    return menu;
  }

  void AppendImageItems(TestRenderViewContextMenu* menu) {
    menu->AppendImageItems();
  }

  void SetUserSelectedDefaultSearchProvider(
      const std::string& base_url,
      bool supports_image_search,
      bool supports_image_translate = true) {
    TemplateURLData data;
    data.SetShortName(u"t");
    data.SetURL(base_url + "?q={searchTerms}");
    if (supports_image_search) {
      data.image_url = base_url;
    }
    if (supports_image_translate) {
      data.image_translate_url = base_url;
    }
    TemplateURL* template_url =
        template_url_service_->Add(std::make_unique<TemplateURL>(data));
    template_url_service_->SetUserSelectedDefaultSearchProvider(template_url);
  }

  PrefService* local_state() { return testing_local_state_->Get(); }

  Browser* GetBrowser() {
    if (!browser_) {
      Browser::CreateParams create_params(profile(), true);
      auto test_window = std::make_unique<TestBrowserWindow>();
      create_params.window = test_window.get();
      browser_.reset(Browser::Create(create_params));
    }
    return browser_.get();
  }

  Browser* GetPwaBrowser() {
    if (!browser_) {
      Browser::CreateParams create_params(Browser::Type::TYPE_APP, profile(),
                                          true);
      auto test_window = std::make_unique<TestBrowserWindow>();
      create_params.window = test_window.get();
      browser_.reset(Browser::Create(create_params));
    }
    return browser_.get();
  }

 private:
  std::unique_ptr<custom_handlers::ProtocolHandlerRegistry> registry_;
  std::unique_ptr<ScopedTestingLocalState> testing_local_state_;
  raw_ptr<TemplateURLService> template_url_service_;
  std::unique_ptr<Browser> browser_;
};

// Verifies when Incognito Mode is not available (disabled by policy),
// Open Link in Incognito Window link in the context menu is disabled.
TEST_F(RenderViewContextMenuPrefsTest,
       DisableOpenInIncognitoWindowWhenIncognitoIsDisabled) {
  std::unique_ptr<TestRenderViewContextMenu> menu(CreateContextMenu());

  // Initially the Incognito mode is be enabled. So is the Open Link in
  // Incognito Window link.
  ASSERT_TRUE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD));
  EXPECT_TRUE(
      menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD));

  // Disable Incognito mode.
  IncognitoModePrefs::SetAvailability(
      profile()->GetPrefs(), policy::IncognitoModeAvailability::kDisabled);
  menu = CreateContextMenu();
  ASSERT_TRUE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD));
  EXPECT_FALSE(
      menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD));
}

#if BUILDFLAG(IS_CHROMEOS)
class RenderViewContextMenuDlpPrefsTest
    : public RenderViewContextMenuPrefsTest {
 public:
  RenderViewContextMenuDlpPrefsTest() = default;

  RenderViewContextMenuDlpPrefsTest(const RenderViewContextMenuDlpPrefsTest&) =
      delete;
  RenderViewContextMenuDlpPrefsTest& operator=(
      const RenderViewContextMenuDlpPrefsTest&) = delete;

  void SetDlpClipboardRestriction() {
    policy::dlp_test_util::DlpRule rule("Rule #1", "Block", "testid1");
    rule.AddSrcUrl(PAGE_URL)
        .AddDstUrl(RESTRICTED_URL)
        .AddRestriction(policy::dlp::kClipboardRestriction,
                        policy::dlp::kBlockLevel);

    base::Value::List rules;
    rules.Append(rule.Create());
    local_state()->SetList(policy::policy_prefs::kDlpRulesList,
                           std::move(rules));
  }

  static constexpr char PAGE_URL[] = "http://www.foo.com/";
  static constexpr char RESTRICTED_URL[] = "http://www.bar.com/";
  static constexpr char NOT_RESTRICTED_URL[] = "http://www.site.com/";
};

// Verifies that OpenLinkInNewTab field is enabled/disabled based on DLP rules.
TEST_F(RenderViewContextMenuDlpPrefsTest,
       DisableOpenLinkInNewTabWhenClipboardIsBlocked) {
  content::ContextMenuParams params = CreateParams(MenuItem::LINK);
  params.page_url = GURL(PAGE_URL);
  params.link_url = GURL(RESTRICTED_URL);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(nullptr);

  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWTAB));

  MockDlpRulesManager mock_dlp_rules_manager(local_state());
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWTAB));

  SetDlpClipboardRestriction();
  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWTAB));

  params.link_url = GURL(NOT_RESTRICTED_URL);
  menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWTAB));
}

// Verifies that OpenLinkInNewWindow field is enabled/disabled based on DLP
// rules.
TEST_F(RenderViewContextMenuDlpPrefsTest,
       DisableOpenLinkInNewWindowWhenClipboardIsBlocked) {
  content::ContextMenuParams params = CreateParams(MenuItem::LINK);
  params.page_url = GURL(PAGE_URL);
  params.link_url = GURL(RESTRICTED_URL);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(nullptr);

  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWWINDOW));

  MockDlpRulesManager mock_dlp_rules_manager(local_state());
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWWINDOW));

  SetDlpClipboardRestriction();
  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWWINDOW));

  params.link_url = GURL(NOT_RESTRICTED_URL);
  menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKNEWWINDOW));
}

// Verifies that OpenLinkInProfileTab field is enabled/disabled based on DLP
// rules.
TEST_F(RenderViewContextMenuDlpPrefsTest,
       DisableOpenLinkInProfileTabWhenClipboardIsBlocked) {
  content::ContextMenuParams params = CreateParams(MenuItem::LINK);
  params.page_url = GURL(PAGE_URL);
  params.link_url = GURL(RESTRICTED_URL);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(nullptr);

  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKINPROFILE));

  MockDlpRulesManager mock_dlp_rules_manager(local_state());
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKINPROFILE));

  SetDlpClipboardRestriction();
  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKINPROFILE));

  params.link_url = GURL(NOT_RESTRICTED_URL);
  menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKINPROFILE));
}

// Verifies that OpenLinkInWebApp field is enabled/disabled based on DLP rules.
TEST_F(RenderViewContextMenuDlpPrefsTest,
       DisableOpenLinkInWebAppWhenClipboardIsBlocked) {
  content::ContextMenuParams params = CreateParams(MenuItem::LINK);
  params.page_url = GURL(PAGE_URL);
  params.link_url = GURL(RESTRICTED_URL);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(nullptr);

  EXPECT_TRUE(
      menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKBOOKMARKAPP));

  MockDlpRulesManager mock_dlp_rules_manager(local_state());
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(
      menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKBOOKMARKAPP));

  SetDlpClipboardRestriction();
  EXPECT_FALSE(
      menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKBOOKMARKAPP));

  params.link_url = GURL(NOT_RESTRICTED_URL);
  menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(
      menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKBOOKMARKAPP));
}

// Verifies that GoToURL field is enabled/disabled based on DLP rules.
TEST_F(RenderViewContextMenuDlpPrefsTest,
       DisableGoToURLWhenClipboardIsBlocked) {
  content::ContextMenuParams params = CreateParams(MenuItem::SELECTION);
  params.page_url = GURL(PAGE_URL);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(nullptr);
  menu->set_selection_navigation_url(GURL(RESTRICTED_URL));

  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_GOTOURL));

  MockDlpRulesManager mock_dlp_rules_manager(local_state());
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_GOTOURL));

  SetDlpClipboardRestriction();
  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_GOTOURL));

  menu->set_selection_navigation_url(GURL(NOT_RESTRICTED_URL));
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_GOTOURL));
}

// Verifies that SearchWebFor field is enabled/disabled based on DLP rules.
TEST_F(RenderViewContextMenuDlpPrefsTest,
       DisableSearchWebForWhenClipboardIsBlocked) {
  content::ContextMenuParams params = CreateParams(MenuItem::SELECTION);
  params.page_url = GURL(PAGE_URL);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_dlp_rules_manager(nullptr);
  menu->set_selection_navigation_url(GURL(RESTRICTED_URL));

  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SEARCHWEBFOR));

  MockDlpRulesManager mock_dlp_rules_manager(local_state());
  menu->set_dlp_rules_manager(&mock_dlp_rules_manager);
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SEARCHWEBFOR));

  SetDlpClipboardRestriction();
  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SEARCHWEBFOR));

  menu->set_selection_navigation_url(GURL(NOT_RESTRICTED_URL));
  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SEARCHWEBFOR));
}
#endif

// Verifies Incognito Mode is not enabled for links disallowed in Incognito.
TEST_F(RenderViewContextMenuPrefsTest,
       DisableOpenInIncognitoWindowForDisallowedUrls) {
  std::unique_ptr<TestRenderViewContextMenu> menu(
      CreateContextMenuOnChromeLink());

  ASSERT_TRUE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD));
  EXPECT_FALSE(
      menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_OPENLINKOFFTHERECORD));
}

// Make sure the checking custom command id that is not enabled will not
// cause DCHECK failure.
TEST_F(RenderViewContextMenuPrefsTest, IsCustomCommandIdEnabled) {
  std::unique_ptr<TestRenderViewContextMenu> menu(CreateContextMenu());

  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_CUSTOM_FIRST));
}

// Check that if image is broken "Load image" menu item is present.
TEST_F(RenderViewContextMenuPrefsTest, LoadBrokenImage) {
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.unfiltered_link_url = params.link_url;
  params.has_image_contents = false;
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  AppendImageItems(menu.get());

  ASSERT_TRUE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_LOAD_IMAGE));
}

// Verify that the suggested file name is propagated to web contents when save a
// media file in context menu.
TEST_F(RenderViewContextMenuPrefsTest, SaveMediaSuggestedFileName) {
  const std::u16string kTestSuggestedFileName = u"test_file";
  content::ContextMenuParams params = CreateParams(MenuItem::VIDEO);
  params.suggested_filename = kTestSuggestedFileName;
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->ExecuteCommand(IDC_CONTENT_CONTEXT_SAVEAVAS, 0 /* event_flags */);

  // Video item should have suggested file name.
  std::u16string suggested_filename =
      content::WebContentsTester::For(web_contents())->GetSuggestedFileName();
  EXPECT_EQ(kTestSuggestedFileName, suggested_filename);

  params = CreateParams(MenuItem::AUDIO);
  params.suggested_filename = kTestSuggestedFileName;
  menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->ExecuteCommand(IDC_CONTENT_CONTEXT_SAVEAVAS, 0 /* event_flags */);

  // Audio item should have suggested file name.
  suggested_filename =
      content::WebContentsTester::For(web_contents())->GetSuggestedFileName();
  EXPECT_EQ(kTestSuggestedFileName, suggested_filename);
}

// Verify ContextMenu navigations properly set the initiator frame token for a
// frame.
TEST_F(RenderViewContextMenuPrefsTest, OpenLinkNavigationParamsSet) {
  TestNavigationDelegate delegate;
  web_contents()->SetDelegate(&delegate);
  content::RenderFrameHost& main_frame = *web_contents()->GetPrimaryMainFrame();

  content::ContextMenuParams params = CreateParams(MenuItem::LINK);
  params.unfiltered_link_url = params.link_url;
  params.link_url = params.link_url;
  params.impression = blink::Impression();
  auto menu = std::make_unique<TestRenderViewContextMenu>(main_frame, params);
  menu->ExecuteCommand(IDC_CONTENT_CONTEXT_OPENLINKNEWTAB, 0);
  EXPECT_TRUE(delegate.last_navigation_params());

  // Verify that the ContextMenu source frame is set as the navigation
  // initiator.
  EXPECT_EQ(main_frame.GetFrameToken(),
            delegate.last_navigation_params()->initiator_frame_token);
  EXPECT_EQ(main_frame.GetProcess()->GetID(),
            delegate.last_navigation_params()->initiator_process_id);

  // Verify that the impression is attached to the navigation.
  EXPECT_TRUE(delegate.last_navigation_params()->impression);
}

// Verify ContextMenu navigations properly set the initiating origin.
TEST_F(RenderViewContextMenuPrefsTest, OpenLinkNavigationInitiatorSet) {
  TestNavigationDelegate delegate;
  web_contents()->SetDelegate(&delegate);
  content::RenderFrameHost& main_frame = *web_contents()->GetPrimaryMainFrame();

  content::ContextMenuParams params = CreateParams(MenuItem::LINK);
  params.unfiltered_link_url = params.link_url;
  params.link_url = params.link_url;
  params.impression = blink::Impression();
  auto menu = std::make_unique<TestRenderViewContextMenu>(main_frame, params);
  menu->ExecuteCommand(IDC_CONTENT_CONTEXT_OPENLINKNEWTAB, 0);
  EXPECT_TRUE(delegate.last_navigation_params());

  // Verify that the initiator is set, and set expectedly.
  EXPECT_TRUE(delegate.last_navigation_params()->initiator_origin.has_value());
  EXPECT_EQ(delegate.last_navigation_params()->initiator_origin->GetURL(),
            params.page_url.DeprecatedGetOriginAsURL());
}

// Verify that "Show all passwords" is displayed on a password field.
TEST_F(RenderViewContextMenuPrefsTest, ShowAllPasswords) {
  // Set up password manager stuff.
  ChromePasswordManagerClient::CreateForWebContentsWithAutofillClient(
      web_contents(), nullptr);

  NavigateAndCommit(GURL("http://www.foo.com/"));
  content::ContextMenuParams params = CreateParams(MenuItem::EDITABLE);
  params.input_field_type =
      blink::mojom::ContextMenuDataInputFieldType::kPassword;
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->Init();

  EXPECT_TRUE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_SHOWALLSAVEDPASSWORDS));
}

// Verify that "Show all passwords" is displayed on a password field in
// Incognito.
TEST_F(RenderViewContextMenuPrefsTest, ShowAllPasswordsIncognito) {
  std::unique_ptr<content::WebContents> incognito_web_contents(
      content::WebContentsTester::CreateTestWebContents(
          profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true), nullptr));

  // Set up password manager stuff.
  ChromePasswordManagerClient::CreateForWebContentsWithAutofillClient(
      incognito_web_contents.get(), nullptr);

  content::WebContentsTester::For(incognito_web_contents.get())
      ->NavigateAndCommit(GURL("http://www.foo.com/"));
  content::ContextMenuParams params = CreateParams(MenuItem::EDITABLE);
  params.input_field_type =
      blink::mojom::ContextMenuDataInputFieldType::kPassword;
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *incognito_web_contents->GetPrimaryMainFrame(), params);
  menu->Init();

  EXPECT_TRUE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_SHOWALLSAVEDPASSWORDS));
}

TEST_F(RenderViewContextMenuPrefsTest,
       SaveAsDisabledByDownloadRestrictionsPolicy) {
  std::unique_ptr<TestRenderViewContextMenu> menu(CreateContextMenu());

  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SAVELINKAS));

  profile()->GetPrefs()->SetInteger(prefs::kDownloadRestrictions,
                                    3 /*ALL_FILES*/);

  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SAVELINKAS));
}

TEST_F(RenderViewContextMenuPrefsTest,
       SaveAsDisabledByAllowFileSelectionDialogsPolicy) {
  std::unique_ptr<TestRenderViewContextMenu> menu(CreateContextMenu());

  EXPECT_TRUE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SAVELINKAS));

  local_state()->SetBoolean(prefs::kAllowFileSelectionDialogs, false);

  EXPECT_FALSE(menu->IsCommandIdEnabled(IDC_CONTENT_CONTEXT_SAVELINKAS));
}

// Verify that item "Search web for" on password Manager is not present
TEST_F(RenderViewContextMenuPrefsTest,
       SearchWebForOptionOnPasswordsManagerIsDisabled) {
  content::ContextMenuParams params =
      CreateParams(MenuItem::SELECTION | MenuItem::EDITABLE);
  params.page_url = chrome::GetSettingsUrl(chrome::kPasswordManagerSubPage);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_selection_navigation_url(GURL("https://www.foo.com/"));
  menu->Init();

  EXPECT_FALSE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFOR));
}

// Verify that item "Search web for" on password check is not present
TEST_F(RenderViewContextMenuPrefsTest,
       SearchWebForOptionOnPasswordCheckIsDisabled) {
  content::ContextMenuParams params =
      CreateParams(MenuItem::SELECTION | MenuItem::EDITABLE);
  params.page_url = chrome::GetSettingsUrl(chrome::kPasswordCheckSubPage);
  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->set_selection_navigation_url(GURL("https://www.foo.com/"));
  menu->Init();

  EXPECT_FALSE(menu->IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFOR));
}

TEST_F(RenderViewContextMenuPrefsTest, FollowOrUnfollow) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(feed::kWebUiFeed);

  feed::WebFeedTabHelper::CreateForWebContents(web_contents());
  feed::WebFeedTabHelper* web_feed_tab_helper =
      feed::WebFeedTabHelper::FromWebContents(web_contents());

  // "Follow site" should be added when the site for the page is not followed.
  {
    web_feed_tab_helper->SetWebFeedInfoForTesting(
        web_contents()->GetLastCommittedURL(),
        TabWebFeedFollowState::kNotFollowed, std::string());
    content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
    TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                   params);
    menu.Init();
    EXPECT_TRUE(menu.IsItemPresent(IDC_FOLLOW));
    EXPECT_FALSE(menu.IsItemPresent(IDC_UNFOLLOW));
  }

  // "Unfollow site" should be added when the site for the page is followed.
  {
    web_feed_tab_helper->SetWebFeedInfoForTesting(
        web_contents()->GetLastCommittedURL(), TabWebFeedFollowState::kFollowed,
        std::string());
    content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
    TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                   params);
    menu.Init();
    EXPECT_FALSE(menu.IsItemPresent(IDC_FOLLOW));
    EXPECT_TRUE(menu.IsItemPresent(IDC_UNFOLLOW));
  }

  // Neither "Follow site" nor "Unfollow site" should be added when the follow
  // state of the site for the page is unknown.
  {
    web_feed_tab_helper->SetWebFeedInfoForTesting(
        web_contents()->GetLastCommittedURL(), TabWebFeedFollowState::kUnknown,
        std::string());
    content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
    TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                   params);
    menu.Init();
    EXPECT_FALSE(menu.IsItemPresent(IDC_FOLLOW));
    EXPECT_FALSE(menu.IsItemPresent(IDC_FOLLOW));
  }
}

class RenderViewContestMenuAutofillTest
    : public RenderViewContextMenuPrefsTest,
      public testing::WithParamInterface<bool> {
 public:
  RenderViewContestMenuAutofillTest() {
    feature_list_.InitAndEnableFeature(
        autofill::features::kAutofillShowManualFallbackInContextMenu);
  }

  void SetUp() override {
    RenderViewContextMenuPrefsTest::SetUp();
    if (IsIncognito()) {
      SetContents(content::WebContentsTester::CreateTestWebContents(
          profile()->GetPrimaryOTRProfile(/*create_if_needed=*/true), nullptr));
    }
  }

 protected:
  // Returns true if the test needs to run in incognito mode.
  bool IsIncognito() const { return GetParam(); }

  autofill::TestContentAutofillClient* autofill_client() {
    return autofill_client_injector_[web_contents()];
  }

 private:
  base::test::ScopedFeatureList feature_list_;
  autofill::TestAutofillClientInjector<autofill::TestContentAutofillClient>
      autofill_client_injector_;
};

INSTANTIATE_TEST_SUITE_P(AutofillContextMenuTest,
                         RenderViewContestMenuAutofillTest,
                         testing::Bool());

// Verify that Autofill context menu items are displayed on a plain text field.
TEST_P(RenderViewContestMenuAutofillTest, ShowAutofillOptions) {
  autofill::PersonalDataManager* pdm =
      autofill::PersonalDataManagerFactory::GetForProfile(
          profile()->GetOriginalProfile());
  DCHECK(pdm);
  pdm->AddServerCreditCardForTest(
      std::make_unique<autofill::CreditCard>(autofill::test::GetCreditCard()));

  NavigateAndCommit(GURL("http://www.foo.com/"));
  content::ContextMenuParams params = CreateParams(MenuItem::EDITABLE);
  params.input_field_type =
      blink::mojom::ContextMenuDataInputFieldType::kPlainText;

  auto menu = std::make_unique<TestRenderViewContextMenu>(
      *web_contents()->GetPrimaryMainFrame(), params);
  menu->Init();

  EXPECT_TRUE(
      menu->IsItemInRangePresent(IDC_CONTENT_CONTEXT_AUTOFILL_CUSTOM_FIRST,
                                 IDC_CONTENT_CONTEXT_AUTOFILL_CUSTOM_LAST));
}

// Verify that the Lens Image Search menu item is disabled on non-image content
TEST_F(RenderViewContextMenuPrefsTest, LensImageSearchNonImage) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

// Verify that the Lens Image Search menu item is disabled when there is the
// Browser is NULL (b/266624865).
TEST_F(RenderViewContextMenuPrefsTest, LensImageSearchNoBrowser) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(nullptr);
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
}

// Verify that the Lens Image Search menu item is enabled on image content
TEST_F(RenderViewContextMenuPrefsTest, LensImageSearchEnabled) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

// Verify that the Lens Image Search menu item is enabled for Progressive Web
// Apps
TEST_F(RenderViewContextMenuPrefsTest, LensImageSearchForProgressiveWebApp) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetPwaBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

// Verify that the Lens Image Search menu item is enabled for third-party
// default search engines that support image search.
TEST_F(RenderViewContextMenuPrefsTest, LensImageSearchEnabledFor3pDse) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.bing.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

// Verify that the Lens Image Search menu item is disabled for third-part
// default search engines that do not support image search.
TEST_F(RenderViewContextMenuPrefsTest, LensImageSearchDisabledFor3pDse) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.yahoo.com",
                                       /*supports_image_search=*/false);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

// Verify that the Translate image menu item is enabled on image content when
// the page is translated
TEST_F(RenderViewContextMenuPrefsTest, LensTranslateImageEnabled) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true,
                                       /*supports_image_translate=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  ChromeTranslateClient::CreateForWebContents(web_contents());
  translate::LanguageState* language_state =
      ChromeTranslateClient::FromWebContents(web_contents())
          ->GetTranslateManager()
          ->GetLanguageState();
  language_state->SetSourceLanguage("zh-CN");
  language_state->SetCurrentLanguage("fr");
  language_state->SetTranslateEnabled(true);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

// Verify that the Translate image menu item is enabled for third-party
// default search engines that support image translate.
TEST_F(RenderViewContextMenuPrefsTest, LensTranslateImageEnabledFor3pDse) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.bing.com",
                                       /*supports_image_search=*/true,
                                       /*supports_image_translate=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  ChromeTranslateClient::CreateForWebContents(web_contents());
  translate::LanguageState* language_state =
      ChromeTranslateClient::FromWebContents(web_contents())
          ->GetTranslateManager()
          ->GetLanguageState();
  language_state->SetSourceLanguage("zh-CN");
  language_state->SetCurrentLanguage("fr");
  language_state->SetTranslateEnabled(true);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

// Verify that the Translate image menu item is disabled for third-party
// default search engines that do not support image translate.
TEST_F(RenderViewContextMenuPrefsTest, LensTranslateImageDisabledFor3pDse) {
  base::test::ScopedFeatureList features;
  features.InitWithFeatures(
      {lens::features::kLensStandalone, lens::features::kEnableImageTranslate},
      {});
  SetUserSelectedDefaultSearchProvider("https://www.bing.com",
                                       /*supports_image_search=*/true,
                                       /*supports_image_translate=*/false);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  ChromeTranslateClient::CreateForWebContents(web_contents());
  translate::LanguageState* language_state =
      ChromeTranslateClient::FromWebContents(web_contents())
          ->GetTranslateManager()
          ->GetLanguageState();
  language_state->SetSourceLanguage("zh-CN");
  language_state->SetCurrentLanguage("fr");
  language_state->SetTranslateEnabled(true);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHWEBFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_SEARCHLENSFORIMAGE));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHWEB));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_TRANSLATEIMAGEWITHLENS));
}

#if BUILDFLAG(ENABLE_LENS_DESKTOP_GOOGLE_BRANDED_FEATURES)
// Verify that the Lens Region Search menu item is displayed when the feature
// is enabled.
TEST_F(RenderViewContextMenuPrefsTest, LensRegionSearch) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

TEST_F(RenderViewContextMenuPrefsTest, LensRegionSearchPdfEnabled) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::RenderFrameHost* render_frame_host =
      web_contents()->GetPrimaryMainFrame();
  OverrideLastCommittedOrigin(
      render_frame_host,
      url::Origin::Create(
          GURL("chrome-extension://mhjfbmdgcfjbbpaeojofohoefgiehjai")));
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*render_frame_host, params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

// Verify that the Lens Region Search menu item is disabled when the user's
// enterprise policy for Lens Region Search is disabled.
TEST_F(RenderViewContextMenuPrefsTest,
       LensRegionSearchEnterprisePoicyDisabled) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  // Set enterprise policy to false.
  profile()->GetPrefs()->SetBoolean(prefs::kLensRegionSearchEnabled, false);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

// Verify that the Lens Region Search menu item is disabled when the user
// clicks on an image.
TEST_F(RenderViewContextMenuPrefsTest, LensRegionSearchDisabledOnImage) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::IMAGE);
  params.has_image_contents = true;
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  AppendImageItems(&menu);

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_WEB_REGION_SEARCH));
}

// Verify that the Lens Region Search menu item is disabled when there is no
// browser.
TEST_F(RenderViewContextMenuPrefsTest, LensRegionSearchPdfDisabledNoBrowser) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::RenderFrameHost* render_frame_host =
      web_contents()->GetPrimaryMainFrame();
  OverrideLastCommittedOrigin(
      render_frame_host,
      url::Origin::Create(
          GURL("chrome-extension://mhjfbmdgcfjbbpaeojofohoefgiehjai")));
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*render_frame_host, params);
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_WEB_REGION_SEARCH));
}

// Verify that the web region search menu item is enabled for a non-Google
// search engine that supports visual search.
TEST_F(RenderViewContextMenuPrefsTest,
       LensRegionSearchNonGoogleDefaultSearchEngineSupportsImageSearch) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.search.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_WEB_REGION_SEARCH));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

// Verify that region search menu items are disabled for a search engine that
// does not support visual search.
TEST_F(RenderViewContextMenuPrefsTest,
       LensRegionSearchDefaultSearchEngineDoesNotSupportImageSearch) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.search.com",
                                       /*supports_image_search=*/false);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_WEB_REGION_SEARCH));
  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

// Verify that the Lens Region Search menu item is disabled when the feature
// is disabled.
TEST_F(RenderViewContextMenuPrefsTest, LensRegionSearchExperimentDisabled) {
  base::test::ScopedFeatureList features;
  features.InitAndDisableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

// Verify that the Lens Region Search menu item is disabled for any page with a
// Chrome UI Scheme.
TEST_F(RenderViewContextMenuPrefsTest, LensRegionSearchChromeUIScheme) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  params.page_url = GURL(chrome::kChromeUISettingsURL);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetBrowser());
  menu.Init();

  EXPECT_FALSE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

// Verify that the Lens Region Search menu item is enabled for Progressive Web
// Apps. Region Search on PWAs is currently broken and therefore disabled on
// Mac. b/250074889
#if BUILDFLAG(IS_MAC)
#define MAYBE_LensRegionSearchProgressiveWebApp \
  DISABLED_LensRegionSearchProgressiveWebApp
#else
#define MAYBE_LensRegionSearchProgressiveWebApp \
  LensRegionSearchProgressiveWebApp
#endif
TEST_F(RenderViewContextMenuPrefsTest,
       MAYBE_LensRegionSearchProgressiveWebApp) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(lens::features::kLensStandalone);
  SetUserSelectedDefaultSearchProvider("https://www.google.com",
                                       /*supports_image_search=*/true);
  content::ContextMenuParams params = CreateParams(MenuItem::PAGE);
  TestRenderViewContextMenu menu(*web_contents()->GetPrimaryMainFrame(),
                                 params);
  menu.SetBrowser(GetPwaBrowser());
  menu.Init();

  EXPECT_TRUE(menu.IsItemPresent(IDC_CONTENT_CONTEXT_LENS_REGION_SEARCH));
}

#endif  // BUILDFLAG(ENABLE_LENS_DESKTOP_GOOGLE_BRANDED_FEATURES)

// Test FormatUrlForClipboard behavior
// -------------------------------------------

struct FormatUrlForClipboardTestData {
  const char* const input;
  const char* const output;
  const char* const name;
};

class FormatUrlForClipboardTest
    : public testing::TestWithParam<FormatUrlForClipboardTestData> {
 public:
  static std::u16string FormatUrl(const GURL& url) {
    return RenderViewContextMenu::FormatURLForClipboard(url);
  }
};

const FormatUrlForClipboardTestData kFormatUrlForClipboardTestData[]{
    {"http://www.foo.com/", "http://www.foo.com/", "HttpNoEscapes"},
    {"http://www.foo.com/%61%62%63", "http://www.foo.com/abc",
     "HttpSafeUnescapes"},
    {"https://www.foo.com/abc%20def", "https://www.foo.com/abc%20def",
     "HttpsEscapedSpecialCharacters"},
    {"https://www.foo.com/%CE%B1%CE%B2%CE%B3",
     "https://www.foo.com/%CE%B1%CE%B2%CE%B3", "HttpsEscapedUnicodeCharacters"},
    {"file:///etc/%CE%B1%CE%B2%CE%B3", "file:///etc/%CE%B1%CE%B2%CE%B3",
     "FileEscapedUnicodeCharacters"},
    {"file://stuff.host.co/my%2Bshare/foo.txt",
     "file://stuff.host.co/my%2Bshare/foo.txt", "FileEscapedSpecialCharacters"},
    {"file://stuff.host.co/my%2Dshare/foo.txt",
     "file://stuff.host.co/my-share/foo.txt", "FileSafeUnescapes"},
    {"mailto:me@foo.com", "me@foo.com", "MailToNoEscapes"},
    {"mailto:me@foo.com,you@bar.com?subject=Hello%20world",
     "me@foo.com,you@bar.com", "MailToWithQuery"},
    {"mailto:me@%66%6F%6F.com", "me@foo.com", "MailToSafeEscapes"},
    {"mailto:me%2Bsorting-tag@foo.com", "me+sorting-tag@foo.com",
     "MailToEscapedSpecialCharacters"},
    {"mailto:%CE%B1%CE%B2%CE%B3@foo.gr", "αβγ@foo.gr",
     "MailToEscapedUnicodeCharacters"},
};

INSTANTIATE_TEST_SUITE_P(
    All,
    FormatUrlForClipboardTest,
    testing::ValuesIn(kFormatUrlForClipboardTestData),
    [](const testing::TestParamInfo<FormatUrlForClipboardTestData>&
           param_info) { return param_info.param.name; });

TEST_P(FormatUrlForClipboardTest, FormatUrlForClipboard) {
  auto param = GetParam();
  GURL url(param.input);
  const std::u16string result = FormatUrl(url);
  DCHECK_EQ(base::UTF8ToUTF16(param.output), result);
}
