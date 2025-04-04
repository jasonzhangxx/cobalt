// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ASH_CLIPBOARD_VIEWS_CLIPBOARD_HISTORY_BITMAP_ITEM_VIEW_H_
#define ASH_CLIPBOARD_VIEWS_CLIPBOARD_HISTORY_BITMAP_ITEM_VIEW_H_

#include "ash/clipboard/views/clipboard_history_item_view.h"
#include "base/memory/raw_ptr.h"
#include "base/unguessable_token.h"
#include "ui/base/clipboard/clipboard_data.h"
#include "ui/base/metadata/metadata_header_macros.h"

namespace ash {
class ClipboardHistory;
class ClipboardHistoryResourceManager;

// The menu item showing a bitmap.
class ClipboardHistoryBitmapItemView : public ClipboardHistoryItemView {
 public:
  METADATA_HEADER(ClipboardHistoryBitmapItemView);
  ClipboardHistoryBitmapItemView(
      const base::UnguessableToken& item_id,
      const ClipboardHistory* clipboard_history,
      const ClipboardHistoryResourceManager* resource_manager,
      views::MenuItemView* container);
  ClipboardHistoryBitmapItemView(const ClipboardHistoryBitmapItemView& rhs) =
      delete;
  ClipboardHistoryBitmapItemView& operator=(
      const ClipboardHistoryBitmapItemView& rhs) = delete;
  ~ClipboardHistoryBitmapItemView() override;

 private:
  class BitmapContentsView;

  // ClipboardHistoryItemView:
  std::unique_ptr<ContentsView> CreateContentsView() override;

  // Owned by ClipboardHistoryController.
  const raw_ptr<const ClipboardHistoryResourceManager, ExperimentalAsh>
      resource_manager_;

  // The format of the associated `ClipboardData`.
  const ui::ClipboardInternalFormat data_format_;
};

}  // namespace ash

#endif  // ASH_CLIPBOARD_VIEWS_CLIPBOARD_HISTORY_BITMAP_ITEM_VIEW_H_
