// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_OMNIBOX_CHROME_OMNIBOX_CLIENT_H_
#define CHROME_BROWSER_UI_OMNIBOX_CHROME_OMNIBOX_CLIENT_H_

#include <memory>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "chrome/browser/autocomplete/chrome_autocomplete_scheme_classifier.h"
#include "chrome/browser/bitmap_fetcher/bitmap_fetcher_service.h"
#include "chrome/common/search/instant_types.h"
#include "components/omnibox/browser/favicon_cache.h"
#include "components/omnibox/browser/omnibox.mojom-shared.h"
#include "components/omnibox/browser/omnibox_client.h"
#include "components/omnibox/browser/on_device_tail_model_observer.h"

class ChromeOmniboxEditModelDelegate;
class GURL;
class Profile;

class ChromeOmniboxClient : public OmniboxClient {
 public:
  ChromeOmniboxClient(ChromeOmniboxEditModelDelegate* edit_model_delegate,
                      Profile* profile);

  ChromeOmniboxClient(const ChromeOmniboxClient&) = delete;
  ChromeOmniboxClient& operator=(const ChromeOmniboxClient&) = delete;

  ~ChromeOmniboxClient() override;

  // OmniboxClient.
  std::unique_ptr<AutocompleteProviderClient> CreateAutocompleteProviderClient()
      override;
  bool CurrentPageExists() const override;
  const GURL& GetURL() const override;
  const std::u16string& GetTitle() const override;
  gfx::Image GetFavicon() const override;
  bool IsLoading() const override;
  bool IsPasteAndGoEnabled() const override;
  bool IsDefaultSearchProviderEnabled() const override;
  SessionID GetSessionID() const override;
  bookmarks::BookmarkModel* GetBookmarkModel() override;
  AutocompleteControllerEmitter* GetAutocompleteControllerEmitter() override;
  TemplateURLService* GetTemplateURLService() override;
  const AutocompleteSchemeClassifier& GetSchemeClassifier() const override;
  AutocompleteClassifier* GetAutocompleteClassifier() override;
  bool ShouldDefaultTypedNavigationsToHttps() const override;
  int GetHttpsPortForTesting() const override;
  bool IsUsingFakeHttpsForHttpsUpgradeTesting() const override;
  gfx::Image GetIconIfExtensionMatch(
      const AutocompleteMatch& match) const override;
  gfx::Image GetSizedIcon(const gfx::VectorIcon& vector_icon_type,
                          SkColor vector_icon_color) const override;
  gfx::Image GetSizedIcon(const gfx::Image& icon) const override;
  bool ProcessExtensionKeyword(const std::u16string& text,
                               const TemplateURL* template_url,
                               const AutocompleteMatch& match,
                               WindowOpenDisposition disposition) override;
  void OnInputStateChanged() override;
  void OnFocusChanged(OmniboxFocusState state,
                      OmniboxFocusChangeReason reason) override;
  void OnResultChanged(const AutocompleteResult& result,
                       bool default_match_changed,
                       bool should_prerender,
                       const BitmapFetchedCallback& on_bitmap_fetched) override;
  gfx::Image GetFaviconForPageUrl(
      const GURL& page_url,
      FaviconFetchedCallback on_favicon_fetched) override;
  gfx::Image GetFaviconForDefaultSearchProvider(
      FaviconFetchedCallback on_favicon_fetched) override;
  gfx::Image GetFaviconForKeywordSearchProvider(
      const TemplateURL* template_url,
      FaviconFetchedCallback on_favicon_fetched) override;
  void OnTextChanged(const AutocompleteMatch& current_match,
                     bool user_input_in_progress,
                     const std::u16string& user_text,
                     const AutocompleteResult& result,
                     bool has_focus) override;
  void OnRevert() override;
  void OnURLOpenedFromOmnibox(OmniboxLog* log) override;
  void OnBookmarkLaunched() override;
  void DiscardNonCommittedNavigations() override;
  void OpenUpdateChromeDialog() override;
  void FocusWebContents() override;
  void OnNavigationLikely(
      size_t index,
      const AutocompleteMatch& match,
      omnibox::mojom::NavigationPredictor navigation_predictor) override;

  // Update shortcuts when a navigation succeeds.
  static void OnSuccessfulNavigation(Profile* profile,
                                     const std::u16string& text,
                                     const AutocompleteMatch& match);

 private:
  // Performs prerendering for |match|.
  void DoPrerender(const AutocompleteMatch& match);

  // Performs preconnection for |match|.
  void DoPreconnect(const AutocompleteMatch& match);

  void OnBitmapFetched(const BitmapFetchedCallback& callback,
                       int result_index,
                       const SkBitmap& bitmap);

  raw_ptr<ChromeOmniboxEditModelDelegate> edit_model_delegate_;
  raw_ptr<Profile> profile_;
  ChromeAutocompleteSchemeClassifier scheme_classifier_;
  std::vector<BitmapFetcherService::RequestId> request_ids_;
  FaviconCache favicon_cache_;
  std::unique_ptr<OnDeviceTailModelObserver> tail_model_observer_;

  base::WeakPtrFactory<ChromeOmniboxClient> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_OMNIBOX_CHROME_OMNIBOX_CLIENT_H_
