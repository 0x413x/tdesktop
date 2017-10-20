/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include "info/media/info_media_list_widget.h"

#include "overview/overview_layout.h"
#include "history/history_media_types.h"
#include "history/history_item.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "storage/file_download.h"
#include "lang/lang_keys.h"
#include "auth_session.h"
#include "window/main_window.h"
#include "styles/style_overview.h"
#include "styles/style_info.h"

namespace Layout = Overview::Layout;

namespace Info {
namespace Media {
namespace {

constexpr auto kPreloadedScreensCount = 4;
constexpr auto kPreloadIfLessThanScreens = 2;
constexpr auto kPreloadedScreensCountFull
	= kPreloadedScreensCount + 1 + kPreloadedScreensCount;

UniversalMsgId GetUniversalId(FullMsgId itemId) {
	return (itemId.channel != 0)
		? UniversalMsgId(itemId.msg)
		: UniversalMsgId(itemId.msg - ServerMaxMsgId);
}

UniversalMsgId GetUniversalId(not_null<const HistoryItem*> item) {
	return GetUniversalId(item->fullId());
}

UniversalMsgId GetUniversalId(not_null<const BaseLayout*> layout) {
	return GetUniversalId(layout->getItem()->fullId());
}

} // namespace

struct ListWidget::Context {
	Layout::PaintContext layoutContext;
	not_null<SelectedMap*> selected;
	not_null<SelectedMap*> dragSelected;
	DragSelectAction dragSelectAction;
};

class ListWidget::Section {
public:
	Section(Type type) : _type(type) {
	}

	bool addItem(not_null<BaseLayout*> item);
	bool empty() const {
		return _items.empty();
	}

	UniversalMsgId minId() const {
		Expects(!empty());
		return _items.back().first;
	}
	UniversalMsgId maxId() const {
		Expects(!empty());
		return _items.front().first;
	}

	void setTop(int top) {
		_top = top;
	}
	int top() const {
		return _top;
	}
	void resizeToWidth(int newWidth);
	int height() const {
		return _height;
	}

	bool removeItem(UniversalMsgId universalId);
	FoundItem findItemNearId(UniversalMsgId universalId) const;
	FoundItem findItemByPoint(QPoint point) const;

	void paint(
		Painter &p,
		const Context &context,
		QRect clip,
		int outerWidth) const;

	static int MinItemHeight(Type type, int width);

private:
	using Items = base::flat_map<
		UniversalMsgId,
		not_null<BaseLayout*>,
		std::greater<>>;
	int headerHeight() const;
	void appendItem(not_null<BaseLayout*> item);
	void setHeader(not_null<BaseLayout*> item);
	bool belongsHere(not_null<BaseLayout*> item) const;
	Items::iterator findItemAfterTop(int top);
	Items::const_iterator findItemAfterTop(int top) const;
	Items::const_iterator findItemAfterBottom(
		Items::const_iterator from,
		int bottom) const;
	QRect findItemRect(not_null<const BaseLayout*> item) const;
	FoundItem completeResult(
		not_null<BaseLayout*> item,
		bool exact) const;
	TextSelection itemSelection(
		not_null<const BaseLayout*> item,
		const Context &context) const;

	int recountHeight() const;
	void refreshHeight();

	Type _type = Type::Photo;
	Text _header;
	Items _items;
	int _itemsLeft = 0;
	int _itemsTop = 0;
	int _itemWidth = 0;
	int _itemsInRow = 1;
	mutable int _rowsCount = 0;
	int _top = 0;
	int _height = 0;

};

bool ListWidget::IsAfter(
		const CursorState &a,
		const CursorState &b) {
	if (a.itemId != b.itemId) {
		return (a.itemId < b.itemId);
	}
	auto xAfter = a.cursor.x() - b.cursor.x();
	auto yAfter = a.cursor.y() - b.cursor.y();
	return (xAfter + yAfter >= 0);
}

bool ListWidget::SkipSelectFromItem(const CursorState &state) {
	if (state.cursor.y() >= state.size.height()
		&& state.cursor.x() >= 0) {
		return true;
	} else if (state.cursor.x() >= state.size.width()
		&& state.cursor.y() >= 0) {
		return true;
	}
	return false;
}

bool ListWidget::SkipSelectTillItem(const CursorState &state) {
	if (state.cursor.y() < state.size.height()
		&& state.cursor.x() < 0) {
		return true;
	} else if (state.cursor.x() < state.size.width()
		&& state.cursor.y() < 0) {
		return true;
	}
	return false;
}

ListWidget::CachedItem::CachedItem(std::unique_ptr<BaseLayout> item)
: item(std::move(item)) {
}

ListWidget::CachedItem::~CachedItem() = default;

bool ListWidget::Section::addItem(not_null<BaseLayout*> item) {
	if (_items.empty() || belongsHere(item)) {
		if (_items.empty()) setHeader(item);
		appendItem(item);
		return true;
	}
	return false;
}

void ListWidget::Section::setHeader(not_null<BaseLayout*> item) {
	auto text = [&] {
		auto date = item->getItem()->date.date();
		switch (_type) {
		case Type::Photo:
		case Type::Video:
		case Type::RoundFile:
		case Type::VoiceFile:
		case Type::File:
			return langMonthFull(date);

		case Type::Link:
			return langDayOfMonthFull(date);

		case Type::MusicFile:
			return QString();
		}
		Unexpected("Type in ListWidget::Section::setHeader()");
	}();
	_header.setText(st::infoMediaHeaderStyle, text);
}

bool ListWidget::Section::belongsHere(
		not_null<BaseLayout*> item) const {
	Expects(!_items.empty());
	auto date = item->getItem()->date.date();
	auto myDate = _items.back().second->getItem()->date.date();

	switch (_type) {
	case Type::Photo:
	case Type::Video:
	case Type::RoundFile:
	case Type::VoiceFile:
	case Type::File:
		return date.year() == myDate.year()
			&& date.month() == myDate.month();

	case Type::Link:
		return date.year() == myDate.year()
			&& date.month() == myDate.month()
			&& date.day() == myDate.day();

	case Type::MusicFile:
		return true;
	}
	Unexpected("Type in ListWidget::Section::belongsHere()");
}

void ListWidget::Section::appendItem(not_null<BaseLayout*> item) {
	_items.emplace(GetUniversalId(item), item);
}

bool ListWidget::Section::removeItem(UniversalMsgId universalId) {
	if (auto it = _items.find(universalId); it != _items.end()) {
		it = _items.erase(it);
		refreshHeight();
		return true;
	}
	return false;
}

QRect ListWidget::Section::findItemRect(
		not_null<const BaseLayout*> item) const {
	auto position = item->position();
	auto top = position / _itemsInRow;
	auto indexInRow = position % _itemsInRow;
	auto left = _itemsLeft
		+ indexInRow * (_itemWidth + st::infoMediaSkip);
	return QRect(left, top, _itemWidth, item->height());
}

auto ListWidget::Section::completeResult(
		not_null<BaseLayout*> item,
		bool exact) const -> FoundItem {
	return { item, findItemRect(item), exact };
}

auto ListWidget::Section::findItemByPoint(
		QPoint point) const -> FoundItem {
	Expects(!_items.empty());
	auto itemIt = findItemAfterTop(point.y());
	if (itemIt == _items.end()) {
		--itemIt;
	}
	auto item = itemIt->second;
	auto rect = findItemRect(item);
	if (point.y() >= rect.top()) {
		auto shift = floorclamp(
			point.x(),
			(_itemWidth + st::infoMediaSkip),
			0,
			_itemsInRow);
		while (shift-- && itemIt != _items.end()) {
			++itemIt;
		}
		if (itemIt == _items.end()) {
			--itemIt;
		}
		item = itemIt->second;
		rect = findItemRect(item);
	}
	return { item, rect, rect.contains(point) };
}

auto ListWidget::Section::findItemNearId(
		UniversalMsgId universalId) const -> FoundItem {
	Expects(!_items.empty());
	auto itemIt = base::lower_bound(
		_items,
		universalId,
		[this](const auto &item, UniversalMsgId universalId) {
			return (item.first > universalId);
		});
	if (itemIt == _items.end()) {
		--itemIt;
	}
	auto item = itemIt->second;
	auto exact = (GetUniversalId(item) == universalId);
	return { item, findItemRect(item), exact };
}

auto ListWidget::Section::findItemAfterTop(
		int top) -> Items::iterator {
	return base::lower_bound(
		_items,
		top,
		[this](const auto &item, int top) {
			auto itemTop = item.second->position() / _itemsInRow;
			return (itemTop + item.second->height()) <= top;
		});
}

auto ListWidget::Section::findItemAfterTop(
		int top) const -> Items::const_iterator {
	return base::lower_bound(
		_items,
		top,
		[this](const auto &item, int top) {
		auto itemTop = item.second->position() / _itemsInRow;
		return (itemTop + item.second->height()) <= top;
	});
}

auto ListWidget::Section::findItemAfterBottom(
		Items::const_iterator from,
		int bottom) const -> Items::const_iterator {
	return std::lower_bound(
		from,
		_items.end(),
		bottom,
		[this](const auto &item, int bottom) {
			auto itemTop = item.second->position() / _itemsInRow;
			return itemTop < bottom;
		});
}

void ListWidget::Section::paint(
		Painter &p,
		const Context &context,
		QRect clip,
		int outerWidth) const {
	auto baseIndex = 0;
	auto header = headerHeight();
	if (QRect(0, 0, outerWidth, header).intersects(clip)) {
		p.setPen(st::infoMediaHeaderFg);
		_header.drawLeftElided(
			p,
			st::infoMediaHeaderPosition.x(),
			st::infoMediaHeaderPosition.y(),
			outerWidth - 2 * st::infoMediaHeaderPosition.x(),
			outerWidth);
	}
	auto top = header + _itemsTop;
	auto fromcol = floorclamp(
		clip.x() - _itemsLeft,
		_itemWidth,
		0,
		_itemsInRow);
	auto tillcol = ceilclamp(
		clip.x() + clip.width() - _itemsLeft,
		_itemWidth,
		0,
		_itemsInRow);
	auto localContext = context.layoutContext;
	localContext.isAfterDate = (header > 0);

	auto fromIt = findItemAfterTop(clip.y());
	auto tillIt = findItemAfterBottom(
		fromIt,
		clip.y() + clip.height());
	for (auto it = fromIt; it != tillIt; ++it) {
		auto item = it->second;
		auto rect = findItemRect(item);
		localContext.isAfterDate = (header > 0)
			&& (rect.y() <= header + _itemsTop);
		if (rect.intersects(clip)) {
			p.translate(rect.topLeft());
			item->paint(
				p,
				clip.translated(-rect.topLeft()),
				itemSelection(item, context),
				&localContext);
			p.translate(-rect.topLeft());
		}
	}
}

TextSelection ListWidget::Section::itemSelection(
		not_null<const BaseLayout*> item,
		const Context &context) const {
	auto universalId = GetUniversalId(item);
	auto dragSelectAction = context.dragSelectAction;
	if (dragSelectAction != DragSelectAction::None) {
		auto i = context.dragSelected->find(universalId);
		if (i != context.dragSelected->end()) {
			return (dragSelectAction == DragSelectAction::Selecting)
				? FullSelection
				: TextSelection();
		}
	}
	auto i = context.selected->find(universalId);
	return (i == context.selected->cend())
		? TextSelection()
		: i->second.text;
}

int ListWidget::Section::headerHeight() const {
	return _header.isEmpty() ? 0 : st::infoMediaHeaderHeight;
}

void ListWidget::Section::resizeToWidth(int newWidth) {
	auto minWidth = st::infoMediaMinGridSize + st::infoMediaSkip * 2;
	if (newWidth < minWidth) {
		return;
	}

	auto resizeOneColumn = [&](int itemsLeft, int itemWidth) {
		_itemsLeft = itemsLeft;
		_itemsTop = 0;
		_itemsInRow = 1;
		_itemWidth = itemWidth;
		for (auto &item : _items) {
			item.second->resizeGetHeight(_itemWidth);
		}
	};
	switch (_type) {
	case Type::Photo:
	case Type::Video:
	case Type::RoundFile: {
		_itemsLeft = st::infoMediaSkip;
		_itemsTop = st::infoMediaSkip;
		_itemsInRow = (newWidth - _itemsLeft)
			/ (st::infoMediaMinGridSize + st::infoMediaSkip);
		_itemWidth = ((newWidth - _itemsLeft) / _itemsInRow)
			- st::infoMediaSkip;
		for (auto &item : _items) {
			item.second->resizeGetHeight(_itemWidth);
		}
	} break;

	case Type::VoiceFile:
	case Type::MusicFile:
		resizeOneColumn(0, newWidth);
		break;
	case Type::File:
	case Type::Link: {
		auto itemsLeft = st::infoMediaHeaderPosition.x();
		auto itemWidth = newWidth - 2 * itemsLeft;
		resizeOneColumn(itemsLeft, itemWidth);
	} break;
	}

	refreshHeight();
}

int ListWidget::Section::MinItemHeight(Type type, int width) {
	auto &songSt = st::overviewFileLayout;
	switch (type) {
	case Type::Photo:
	case Type::Video:
	case Type::RoundFile: {
		auto itemsLeft = st::infoMediaSkip;
		auto itemsInRow = (width - itemsLeft)
			/ (st::infoMediaMinGridSize + st::infoMediaSkip);
		return (st::infoMediaMinGridSize + st::infoMediaSkip) / itemsInRow;
	} break;

	case Type::VoiceFile:
		return songSt.songPadding.top() + songSt.songThumbSize + songSt.songPadding.bottom() + st::lineWidth;
	case Type::File:
		return songSt.filePadding.top() + songSt.fileThumbSize + songSt.filePadding.bottom() + st::lineWidth;
	case Type::MusicFile:
		return songSt.songPadding.top() + songSt.songThumbSize + songSt.songPadding.bottom();
	case Type::Link:
		return st::linksPhotoSize + st::linksMargin.top() + st::linksMargin.bottom() + st::linksBorder;
	}
	Unexpected("Type in ListWidget::Section::MinItemHeight()");
}

int ListWidget::Section::recountHeight() const {
	auto result = headerHeight();

	switch (_type) {
	case Type::Photo:
	case Type::Video:
	case Type::RoundFile: {
		auto itemHeight = _itemWidth + st::infoMediaSkip;
		auto index = 0;
		result += _itemsTop;
		for (auto &item : _items) {
			item.second->setPosition(_itemsInRow * result + index);
			if (++index == _itemsInRow) {
				result += itemHeight;
				index = 0;
			}
		}
		if (_items.size() % _itemsInRow) {
			_rowsCount = int(_items.size()) / _itemsInRow + 1;
			result += itemHeight;
		} else {
			_rowsCount = int(_items.size()) / _itemsInRow;
		}
	} break;

	case Type::VoiceFile:
	case Type::File:
	case Type::MusicFile:
	case Type::Link:
		for (auto &item : _items) {
			item.second->setPosition(result);
			result += item.second->height();
		}
		_rowsCount = _items.size();
		break;
	}

	return result;
}

void ListWidget::Section::refreshHeight() {
	_height = recountHeight();
}

ListWidget::ListWidget(
	QWidget *parent,
	not_null<Window::Controller*> controller,
	not_null<PeerData*> peer,
	Type type)
: RpWidget(parent)
, _controller(controller)
, _peer(peer)
, _type(type)
, _slice(sliceKey(_universalAroundId)) {
	setAttribute(Qt::WA_MouseTracking);
	start();
	refreshViewer();
}

void ListWidget::start() {
	ObservableViewer(*Window::Theme::Background())
		| rpl::start_with_next([this](const auto &update) {
			if (update.paletteChanged()) {
				invalidatePaletteCache();
			}
		}, lifetime());
	ObservableViewer(Auth().downloader().taskFinished())
		| rpl::start_with_next([this] { update(); }, lifetime());
	Auth().data().itemLayoutChanged()
		| rpl::start_with_next([this](auto item) {
			itemLayoutChanged(item);
		}, lifetime());
	Auth().data().itemRemoved()
		| rpl::start_with_next([this](auto item) {
			itemRemoved(item);
		}, lifetime());
	Auth().data().itemRepaintRequest()
		| rpl::start_with_next([this](auto item) {
			repaintItem(item);
		}, lifetime());
}

void ListWidget::itemRemoved(not_null<const HistoryItem*> item) {
	if (isMyItem(item)) {
		auto universalId = GetUniversalId(item);

		auto sectionIt = findSectionByItem(universalId);
		if (sectionIt != _sections.end()) {
			if (sectionIt->removeItem(universalId)) {
				auto top = sectionIt->top();
				if (sectionIt->empty()) {
					_sections.erase(sectionIt);
				}
				refreshHeight();
			}
		}

		if (isItemLayout(item, _overLayout)) {
			_overLayout = nullptr;
		}

		_layouts.erase(universalId);
		_dragSelected.remove(universalId);

		auto i = _selected.find(universalId);
		if (i != _selected.cend()) {
			removeItemSelection(i);
			pushSelectedItems();
		}

		mouseActionUpdate(_mousePosition);
	}
}

FullMsgId ListWidget::computeFullId(
		UniversalMsgId universalId) const {
	Expects(universalId != 0);
	auto peerChannel = [&] {
		return _peer->isChannel() ? _peer->bareId() : NoChannel;
	};
	return (universalId > 0)
		? FullMsgId(peerChannel(), universalId)
		: FullMsgId(NoChannel, ServerMaxMsgId + universalId);
}

auto ListWidget::collectSelectedItems() const -> SelectedItems {
	auto convert = [&](
			UniversalMsgId universalId,
			const SelectionData &selection) {
		auto result = SelectedItem(computeFullId(universalId));
		result.canDelete = selection.canDelete;
		result.canForward = selection.canForward;
		return result;
	};
	auto transformation = [&](const auto &item) {
		return convert(item.first, item.second);
	};
	auto items = SelectedItems();
	items.reserve(_selected.size());
	std::transform(
		_selected.begin(),
		_selected.end(),
		std::back_inserter(items),
		transformation);
	return items;
}

void ListWidget::pushSelectedItems() {
	_selectedItemsStream.fire(collectSelectedItems());
}

bool ListWidget::hasSelected() const {
	return !_selected.empty();
}

bool ListWidget::isSelectedItem(
		const SelectedMap::const_iterator &i) const {
	return (i != _selected.end())
		&& (i->second.text == FullSelection);
}

void ListWidget::removeItemSelection(
		const SelectedMap::const_iterator &i) {
	Expects(i != _selected.cend());
	_selected.erase(i);
	if (_selected.empty()) {
		update();
	}
}

bool ListWidget::hasSelectedText() const {
	return hasSelected()
		&& !hasSelectedItems();
}

bool ListWidget::hasSelectedItems() const {
	return isSelectedItem(_selected.cbegin());
}

void ListWidget::itemLayoutChanged(
		not_null<const HistoryItem*> item) {
	if (isItemLayout(item, _overLayout)) {
		mouseActionUpdate();
	}
}

void ListWidget::repaintItem(const HistoryItem *item) {
	if (item && isMyItem(item)) {
		repaintItem(GetUniversalId(item));
	}
}

void ListWidget::repaintItem(UniversalMsgId universalId) {
	if (auto item = findItemById(universalId)) {
		repaintItem(item->geometry);
	}
}

void ListWidget::repaintItem(const BaseLayout *item) {
	if (item) {
		repaintItem(GetUniversalId(item));
	}
}

void ListWidget::repaintItem(QRect itemGeometry) {
	rtlupdate(itemGeometry);
}

bool ListWidget::isMyItem(not_null<const HistoryItem*> item) const {
	auto peer = item->history()->peer;
	return (_peer == peer || _peer == peer->migrateTo());
}

bool ListWidget::isItemLayout(
		not_null<const HistoryItem*> item,
		BaseLayout *layout) const {
	return layout && (layout->getItem() == item);
}

void ListWidget::invalidatePaletteCache() {
	for (auto &layout : _layouts) {
		layout.second.item->invalidateCache();
	}
}

SharedMediaMergedSlice::Key ListWidget::sliceKey(
		UniversalMsgId universalId) const {
	using Key = SharedMediaMergedSlice::Key;
	if (auto migrateFrom = _peer->migrateFrom()) {
		return Key(_peer->id, migrateFrom->id, _type, universalId);
	}
	if (universalId < 0) {
		// Convert back to plain id for non-migrated histories.
		universalId += ServerMaxMsgId;
	}
	return Key(_peer->id, 0, _type, universalId);
}

void ListWidget::refreshViewer() {
	_viewerLifetime.destroy();
	SharedMediaMergedViewer(
		sliceKey(_universalAroundId),
		_idsLimit,
		_idsLimit)
		| rpl::start_with_next([this](
				SharedMediaMergedSlice &&slice) {
			_slice = std::move(slice);
			if (auto nearest = _slice.nearest(_universalAroundId)) {
				_universalAroundId = *nearest;
			}
			refreshRows();
		}, _viewerLifetime);
}

BaseLayout *ListWidget::getLayout(UniversalMsgId universalId) {
	auto it = _layouts.find(universalId);
	if (it == _layouts.end()) {
		if (auto layout = createLayout(universalId, _type)) {
			layout->initDimensions();
			it = _layouts.emplace(
				universalId,
				std::move(layout)).first;
		} else {
			return nullptr;
		}
	}
	it->second.stale = false;
	return it->second.item.get();
}

BaseLayout *ListWidget::getExistingLayout(
		UniversalMsgId universalId) const {
	auto it = _layouts.find(universalId);
	return (it != _layouts.end())
		? it->second.item.get()
		: nullptr;
}

std::unique_ptr<BaseLayout> ListWidget::createLayout(
		UniversalMsgId universalId,
		Type type) {
	auto item = App::histItemById(computeFullId(universalId));
	if (!item) {
		return nullptr;
	}
	auto getPhoto = [&]() -> PhotoData* {
		if (auto media = item->getMedia()) {
			if (media->type() == MediaTypePhoto) {
				return static_cast<HistoryPhoto*>(media)->photo();
			}
		}
		return nullptr;
	};
	auto getFile = [&]() -> DocumentData* {
		if (auto media = item->getMedia()) {
			return media->getDocument();
		}
		return nullptr;
	};

	auto &songSt = st::overviewFileLayout;
	using namespace Layout;
	switch (type) {
	case Type::Photo:
		if (auto photo = getPhoto()) {
			return std::make_unique<Photo>(item, photo);
		}
		return nullptr;
	case Type::Video:
		if (auto file = getFile()) {
			return std::make_unique<Video>(item, file);
		}
		return nullptr;
	case Type::File:
		if (auto file = getFile()) {
			return std::make_unique<Document>(item, file, songSt);
		}
		return nullptr;
	case Type::MusicFile:
		if (auto file = getFile()) {
			return std::make_unique<Document>(item, file, songSt);
		}
		return nullptr;
	case Type::VoiceFile:
		if (auto file = getFile()) {
			return std::make_unique<Voice>(item, file, songSt);
		}
		return nullptr;
	case Type::Link:
		return std::make_unique<Link>(item, item->getMedia());
	case Type::RoundFile:
		return nullptr;
	}
	Unexpected("Type in ListWidget::createLayout()");
}

void ListWidget::refreshRows() {
	saveScrollState();

	markLayoutsStale();

	_sections.clear();
	auto section = Section(_type);
	auto count = _slice.size();
	for (auto i = count; i != 0;) {
		auto universalId = GetUniversalId(_slice[--i]);
		if (auto layout = getLayout(universalId)) {
			if (!section.addItem(layout)) {
				_sections.push_back(std::move(section));
				section = Section(_type);
				section.addItem(layout);
			}
		}
	}
	if (!section.empty()) {
		_sections.push_back(std::move(section));
	}

	clearStaleLayouts();

	resizeToWidth(width());

	restoreScrollState();
}

void ListWidget::markLayoutsStale() {
	for (auto &layout : _layouts) {
		layout.second.stale = true;
	}
}

int ListWidget::resizeGetHeight(int newWidth) {
	if (newWidth > 0) {
		for (auto &section : _sections) {
			section.resizeToWidth(newWidth);
		}
	}
	return recountHeight();
}

auto ListWidget::findItemByPoint(QPoint point) -> FoundItem {
	Expects(!_sections.empty());
	auto sectionIt = findSectionAfterTop(point.y());
	if (sectionIt == _sections.end()) {
		--sectionIt;
	}
	auto shift = QPoint(0, sectionIt->top());
	return foundItemInSection(
		sectionIt->findItemByPoint(point - shift),
		*sectionIt);
}

auto ListWidget::findItemById(
		UniversalMsgId universalId) -> base::optional<FoundItem> {
	auto sectionIt = findSectionByItem(universalId);
	if (sectionIt != _sections.end()) {
		auto item = sectionIt->findItemNearId(universalId);
		if (item.exact) {
			return foundItemInSection(item, *sectionIt);
		}
	}
	return base::none;
}

auto ListWidget::findItemDetails(
		BaseLayout *item) -> base::optional<FoundItem> {
	return item
		? findItemById(GetUniversalId(item))
		: base::none;
}

auto ListWidget::foundItemInSection(
		const FoundItem &item,
		const Section &section) -> FoundItem {
	return {
		item.layout,
		item.geometry.translated(0, section.top()),
		item.exact };
}

void ListWidget::visibleTopBottomUpdated(
		int visibleTop,
		int visibleBottom) {
	_visibleTop = visibleTop;
	_visibleBottom = visibleBottom;

	checkMoveToOtherViewer();
}

void ListWidget::checkMoveToOtherViewer() {
	auto visibleHeight = (_visibleBottom - _visibleTop);
	if (width() <= 0
		|| visibleHeight <= 0
		|| _sections.empty()
		|| _scrollTopId) {
		return;
	}

	auto topItem = findItemByPoint({ 0, _visibleTop });
	auto bottomItem = findItemByPoint({ 0, _visibleBottom });

	auto preloadedHeight = kPreloadedScreensCountFull * visibleHeight;
	auto minItemHeight = Section::MinItemHeight(_type, width());
	auto preloadedCount = preloadedHeight / minItemHeight;
	auto preloadIdsLimitMin = (preloadedCount / 2) + 1;
	auto preloadIdsLimit = preloadIdsLimitMin
		+ (visibleHeight / minItemHeight);

	auto preloadBefore = kPreloadIfLessThanScreens * visibleHeight;
	auto after = _slice.skippedAfter();
	auto preloadTop = (_visibleTop < preloadBefore);
	auto topLoaded = after && (*after == 0);
	auto before = _slice.skippedBefore();
	auto preloadBottom = (height() - _visibleBottom < preloadBefore);
	auto bottomLoaded = before && (*before == 0);

	auto minScreenDelta = kPreloadedScreensCount
		- kPreloadIfLessThanScreens;
	auto minUniversalIdDelta = (minScreenDelta * visibleHeight)
		/ minItemHeight;
	auto preloadAroundItem = [&](const FoundItem &item) {
		auto preloadRequired = false;
		auto universalId = GetUniversalId(item.layout);
		if (!preloadRequired) {
			preloadRequired = (_idsLimit < preloadIdsLimitMin);
		}
		if (!preloadRequired) {
			auto delta = _slice.distance(
				sliceKey(_universalAroundId),
				sliceKey(universalId));
			Assert(delta != base::none);
			preloadRequired = (qAbs(*delta) >= minUniversalIdDelta);
		}
		if (preloadRequired) {
			_idsLimit = preloadIdsLimit;
			_universalAroundId = universalId;
			refreshViewer();
		}
	};

	if (preloadTop && !topLoaded) {
		preloadAroundItem(topItem);
	} else if (preloadBottom && !bottomLoaded) {
		preloadAroundItem(bottomItem);
	}
}

void ListWidget::saveScrollState() {
	if (_sections.empty()) {
		_scrollTopId = 0;
		_scrollTopShift = 0;
		return;
	}
	auto topItem = findItemByPoint({ 0, _visibleTop });
	_scrollTopId = GetUniversalId(topItem.layout);
	_scrollTopShift = _visibleTop - topItem.geometry.y();
}

void ListWidget::restoreScrollState() {
	auto scrollTopId = base::take(_scrollTopId);
	auto scrollTopShift = base::take(_scrollTopShift);
	if (_sections.empty() || !scrollTopId) {
		return;
	}
	auto sectionIt = findSectionByItem(scrollTopId);
	if (sectionIt == _sections.end()) {
		--sectionIt;
	}
	auto item = foundItemInSection(
		sectionIt->findItemNearId(scrollTopId),
		*sectionIt);
	auto newVisibleTop = item.geometry.y() + scrollTopShift;
	if (_visibleTop != newVisibleTop) {
		_scrollToRequests.fire_copy(newVisibleTop);
	}
}

QMargins ListWidget::padding() const {
	return st::infoMediaMargin;
}

void ListWidget::paintEvent(QPaintEvent *e) {
	Painter p(this);

	auto outerWidth = width();
	auto clip = e->rect();
	auto ms = getms();
	auto fromSectionIt = findSectionAfterTop(clip.y());
	auto tillSectionIt = findSectionAfterBottom(
		fromSectionIt,
		clip.y() + clip.height());
	auto context = Context {
		Layout::PaintContext(ms, !_selected.empty()),
		&_selected,
		&_dragSelected,
		_dragSelectAction
	};
	for (auto it = fromSectionIt; it != tillSectionIt; ++it) {
		auto top = it->top();
		p.translate(0, top);
		it->paint(p, context, clip.translated(0, -top), outerWidth);
		p.translate(0, -top);
	}
}

void ListWidget::mousePressEvent(QMouseEvent *e) {
	if (_contextMenu) {
		e->accept();
		return; // ignore mouse press, that was hiding context menu
	}
	mouseActionStart(e->globalPos(), e->button());
}

void ListWidget::mouseMoveEvent(QMouseEvent *e) {
	auto buttonsPressed = (e->buttons() & (Qt::LeftButton | Qt::MiddleButton));
	if (!buttonsPressed && _mouseAction != MouseAction::None) {
		mouseReleaseEvent(e);
	}
	mouseActionUpdate(e->globalPos());
}

void ListWidget::mouseReleaseEvent(QMouseEvent *e) {
	mouseActionFinish(e->globalPos(), e->button());
	if (!rect().contains(e->pos())) {
		leaveEvent(e);
	}
}

void ListWidget::mouseDoubleClickEvent(QMouseEvent *e) {
	mouseActionStart(e->globalPos(), e->button());
	trySwitchToWordSelection();
}

void ListWidget::trySwitchToWordSelection() {
	auto selectingSome = (_mouseAction == MouseAction::Selecting)
		&& hasSelectedText();
	auto willSelectSome = (_mouseAction == MouseAction::None)
		&& !hasSelectedItems();
	auto checkSwitchToWordSelection = _overLayout
		&& (_mouseSelectType == TextSelectType::Letters)
		&& (selectingSome || willSelectSome);
	if (checkSwitchToWordSelection) {
		switchToWordSelection();
	}
}

void ListWidget::switchToWordSelection() {
	Expects(_overLayout != nullptr);

	HistoryStateRequest request;
	request.flags |= Text::StateRequest::Flag::LookupSymbol;
	auto dragState = _overLayout->getState(_pressState.cursor, request);
	if (dragState.cursor != HistoryInTextCursorState) {
		return;
	}
	_mouseTextSymbol = dragState.symbol;
	_mouseSelectType = TextSelectType::Words;
	if (_mouseAction == MouseAction::None) {
		_mouseAction = MouseAction::Selecting;
		clearSelected();
		auto selStatus = TextSelection {
			dragState.symbol,
			dragState.symbol
		};
		applyItemSelection(_overState.itemId, selStatus);
	}
	mouseActionUpdate();

	_trippleClickPoint = _mousePosition;
	_trippleClickStartTime = getms();
}

void ListWidget::applyItemSelection(
		UniversalMsgId universalId,
		TextSelection selection) {
	if (changeItemSelection(
			_selected,
			universalId,
			selection)) {
		repaintItem(universalId);
	}
}

void ListWidget::toggleItemSelection(UniversalMsgId universalId) {
	auto it = _selected.find(universalId);
	if (it == _selected.cend()) {
		applyItemSelection(universalId, FullSelection);
	} else {
		removeItemSelection(it);
	}
}

bool ListWidget::changeItemSelection(
		SelectedMap &selected,
		UniversalMsgId universalId,
		TextSelection selection) const {
	auto changeExisting = [&](auto it) {
		if (it == selected.cend()) {
			return false;
		} else if (it->second.text != selection) {
			it->second.text = selection;
			return true;
		}
		return false;
	};
	if (selected.size() < MaxSelectedItems) {
		auto [iterator, ok] = selected.try_emplace(
			universalId,
			selection);
		if (ok) {
			auto item = App::histItemById(computeFullId(universalId));
			if (!item) {
				selected.erase(iterator);
				return false;
			}
			iterator->second.canDelete = item->canDelete();
			iterator->second.canForward = item->canForward();
			return true;
		}
		return changeExisting(iterator);
	}
	return changeExisting(selected.find(universalId));
}

bool ListWidget::isItemUnderPressSelected() const {
	return itemUnderPressSelection() != _selected.end();
}

auto ListWidget::itemUnderPressSelection() -> SelectedMap::iterator {
	return (_pressState.itemId && _pressState.inside)
		? _selected.find(_pressState.itemId)
		: _selected.end();
}

auto ListWidget::itemUnderPressSelection() const
-> SelectedMap::const_iterator {
	return (_pressState.itemId && _pressState.inside)
		? _selected.find(_pressState.itemId)
		: _selected.end();
}

bool ListWidget::requiredToStartDragging(
		not_null<BaseLayout*> layout) const {
	if (_mouseCursorState == HistoryInDateCursorState) {
		return true;
	}
//	return dynamic_cast<HistorySticker*>(layout->getMedia());
	return false;
}

bool ListWidget::isPressInSelectedText(HistoryTextState state) const {
	if (state.cursor != HistoryInTextCursorState) {
		return false;
	}
	if (!hasSelectedText()
		|| !isItemUnderPressSelected()) {
		return false;
	}
	auto pressedSelection = itemUnderPressSelection();
	auto from = pressedSelection->second.text.from;
	auto to = pressedSelection->second.text.to;
	return (state.symbol >= from && state.symbol < to);
}

void ListWidget::clearSelected() {
	if (_selected.empty()) {
		return;
	}
	if (hasSelectedText()) {
		repaintItem(_selected.begin()->first);
	} else {
		update();
	}
	_selected.clear();
}

void ListWidget::validateTrippleClickStartTime() {
	if (_trippleClickStartTime) {
		auto elapsed = (getms() - _trippleClickStartTime);
		if (elapsed >= QApplication::doubleClickInterval()) {
			_trippleClickStartTime = 0;
		}
	}
}

void ListWidget::enterEventHook(QEvent *e) {
	mouseActionUpdate(QCursor::pos());
	return RpWidget::enterEventHook(e);
}

void ListWidget::leaveEventHook(QEvent *e) {
	if (auto item = _overLayout) {
		if (_overState.inside) {
			repaintItem(item);
			_overState.inside = false;
		}
	}
	ClickHandler::clearActive();
	if (!ClickHandler::getPressed() && _cursor != style::cur_default) {
		_cursor = style::cur_default;
		setCursor(_cursor);
	}
	return RpWidget::leaveEventHook(e);
}

QPoint ListWidget::clampMousePosition(QPoint position) const {
	return {
		std::clamp(position.x(), 0, qMax(0, width() - 1)),
		std::clamp(position.y(), _visibleTop, _visibleBottom - 1)
	};
}

void ListWidget::mouseActionUpdate(const QPoint &screenPos) {
	if (_sections.empty()) {
		return;
	}

	_mousePosition = screenPos;

	auto local = mapFromGlobal(_mousePosition);
	auto point = clampMousePosition(local);
	auto [layout, geometry, inside] = findItemByPoint(point);
	auto state = CursorState {
		GetUniversalId(layout),
		geometry.size(),
		point - geometry.topLeft(),
		inside
	};
	auto item = layout ? layout->getItem() : nullptr;
	if (_overLayout != layout) {
		repaintItem(_overLayout);
		_overLayout = layout;
		repaintItem(geometry);
	}
	_overState = state;

	HistoryTextState dragState;
	ClickHandlerHost *lnkhost = nullptr;
	auto inTextSelection = _overState.inside
		&& (_overState.itemId == _pressState.itemId)
		&& hasSelectedText();
	auto cursorDeltaLength = [&] {
		auto cursorDelta = (_overState.cursor - _pressState.cursor);
		return cursorDelta.manhattanLength();
	};
	auto dragStartLength = [] {
		return QApplication::startDragDistance();
	};
	if (_overLayout) {
		if (_overState.itemId != _pressState.itemId
			|| cursorDeltaLength() >= dragStartLength()) {
			if (_mouseAction == MouseAction::PrepareDrag) {
				_mouseAction = MouseAction::Dragging;
				InvokeQueued(this, [this] { performDrag(); });
			} else if (_mouseAction == MouseAction::PrepareSelect) {
				_mouseAction = MouseAction::Selecting;
			}
		}
		HistoryStateRequest request;
		if (_mouseAction == MouseAction::Selecting) {
			request.flags |= Text::StateRequest::Flag::LookupSymbol;
		} else {
			inTextSelection = false;
		}
		dragState = _overLayout->getState(_overState.cursor, request);
		lnkhost = _overLayout;
	}
	ClickHandler::setActive(dragState.link, lnkhost);

	if (_mouseAction == MouseAction::None) {
		_mouseCursorState = dragState.cursor;
		auto cursor = computeMouseCursor();
		if (_cursor != cursor) {
			setCursor(_cursor = cursor);
		}
	} else if (_mouseAction == MouseAction::Selecting) {
		if (inTextSelection) {
			auto second = dragState.symbol;
			if (dragState.afterSymbol && _mouseSelectType == TextSelectType::Letters) {
				++second;
			}
			auto selState = TextSelection {
				qMin(second, _mouseTextSymbol),
				qMax(second, _mouseTextSymbol)
			};
			if (_mouseSelectType != TextSelectType::Letters) {
				selState = _overLayout->adjustSelection(selState, _mouseSelectType);
			}
			applyItemSelection(_overState.itemId, selState);
			auto hasSelection = (selState == FullSelection)
				|| (selState.from != selState.to);
			if (!_wasSelectedText && hasSelection) {
				_wasSelectedText = true;
				setFocus();
			}
			clearDragSelection();
		} else if (_pressState.itemId) {
			updateDragSelection();
		}
	} else if (_mouseAction == MouseAction::Dragging) {
	}

	// #TODO scroll by drag
	//if (_mouseAction == MouseAction::Selecting) {
	//	_widget->checkSelectingScroll(mousePos);
	//} else {
	//	clearDragSelection();
	//	_widget->noSelectingScroll();
	//}
}

style::cursor ListWidget::computeMouseCursor() const {
	if (ClickHandler::getPressed() || ClickHandler::getActive()) {
		return style::cur_pointer;
	} else if (!hasSelectedItems()
		&& (_mouseCursorState == HistoryInTextCursorState)) {
		return style::cur_text;
	}
	return style::cur_default;
}

void ListWidget::updateDragSelection() {
	auto fromState = _pressState;
	auto tillState = _overState;
	auto swapStates = IsAfter(fromState, tillState);
	if (swapStates) {
		std::swap(fromState, tillState);
	}
	if (!fromState.itemId || !tillState.itemId) {
		clearDragSelection();
		return;
	}
	auto fromId = SkipSelectFromItem(fromState)
		? (fromState.itemId - 1)
		: fromState.itemId;
	auto tillId = SkipSelectTillItem(tillState)
		? tillState.itemId
		: (tillState.itemId - 1);
	for (auto i = _dragSelected.begin(); i != _dragSelected.end();) {
		auto itemId = i->first;
		if (itemId > fromId || itemId <= tillId) {
			i = _dragSelected.erase(i);
		} else {
			++i;
		}
	}
	for (auto &layoutItem : _layouts) {
		auto &&universalId = layoutItem.first;
		auto &&layout = layoutItem.second;
		if (universalId <= fromId && universalId > tillId) {
			changeItemSelection(
				_dragSelected,
				universalId,
				FullSelection);
		}
	}
	_dragSelectAction = [&] {
		if (_dragSelected.empty()) {
			return DragSelectAction::None;
		}
		auto &[firstDragItem, data] = swapStates
			? _dragSelected.front()
			: _dragSelected.back();
		if (isSelectedItem(_selected.find(firstDragItem))) {
			return DragSelectAction::Deselecting;
		} else {
			return DragSelectAction::Selecting;
		}
	}();
	if (!_wasSelectedText
		&& !_dragSelected.empty()
		&& _dragSelectAction == DragSelectAction::Selecting) {
		_wasSelectedText = true;
		setFocus();
	}
	update();
}

void ListWidget::clearDragSelection() {
	_dragSelectAction = DragSelectAction::None;
	if (!_dragSelected.empty()) {
		_dragSelected.clear();
		update();
	}
}

void ListWidget::mouseActionStart(const QPoint &screenPos, Qt::MouseButton button) {
	mouseActionUpdate(screenPos);
	if (button != Qt::LeftButton) return;

	ClickHandler::pressed();
	if (_pressState != _overState) {
		if (_pressState.itemId != _overState.itemId) {
			repaintItem(_pressState.itemId);
		}
		_pressState = _overState;
		repaintItem(_overLayout);
	}
	auto pressLayout = _overLayout;

	_mouseAction = MouseAction::None;
	_pressWasInactive = _controller->window()->wasInactivePress();
	if (_pressWasInactive) _controller->window()->setInactivePress(false);

	if (ClickHandler::getPressed()) {
		_mouseAction = MouseAction::PrepareDrag;
	} else if (hasSelectedItems()) {
		if (isItemUnderPressSelected()) {
			_mouseAction = MouseAction::PrepareDrag; // start items drag
		} else if (!_pressWasInactive) {
			_mouseAction = MouseAction::PrepareSelect; // start items select
		}
	}
	if (_mouseAction == MouseAction::None && pressLayout) {
		HistoryTextState dragState;
		validateTrippleClickStartTime();
		auto startDistance = (screenPos - _trippleClickPoint).manhattanLength();
		auto validStartPoint = startDistance < QApplication::startDragDistance();
		if (_trippleClickStartTime != 0 && validStartPoint) {
			HistoryStateRequest request;
			request.flags = Text::StateRequest::Flag::LookupSymbol;
			dragState = pressLayout->getState(_pressState.cursor, request);
			if (dragState.cursor == HistoryInTextCursorState) {
				TextSelection selStatus = { dragState.symbol, dragState.symbol };
				if (selStatus != FullSelection && !hasSelectedItems()) {
					clearSelected();
					applyItemSelection(_pressState.itemId, selStatus);
					_mouseTextSymbol = dragState.symbol;
					_mouseAction = MouseAction::Selecting;
					_mouseSelectType = TextSelectType::Paragraphs;
					mouseActionUpdate(_mousePosition);
					_trippleClickStartTime = getms();
				}
			}
		} else {
			HistoryStateRequest request;
			request.flags = Text::StateRequest::Flag::LookupSymbol;
			dragState = pressLayout->getState(_pressState.cursor, request);
		}
		if (_mouseSelectType != TextSelectType::Paragraphs) {
			if (_pressState.inside) {
				_mouseTextSymbol = dragState.symbol;
				if (isPressInSelectedText(dragState)) {
					_mouseAction = MouseAction::PrepareDrag; // start text drag
				} else if (!_pressWasInactive) {
					if (requiredToStartDragging(pressLayout)) {
						_mouseAction = MouseAction::PrepareDrag;
					} else {
						if (dragState.afterSymbol) ++_mouseTextSymbol;
						TextSelection selStatus = { _mouseTextSymbol, _mouseTextSymbol };
						if (selStatus != FullSelection && !hasSelectedItems()) {
							clearSelected();
							applyItemSelection(_pressState.itemId, selStatus);
							_mouseAction = MouseAction::Selecting;
							repaintItem(pressLayout);
						} else {
							_mouseAction = MouseAction::PrepareSelect;
						}
					}
				}
			} else if (!_pressWasInactive) {
				_mouseAction = MouseAction::PrepareSelect; // start items select
			}
		}
	}

	if (!pressLayout) {
		_mouseAction = MouseAction::None;
	} else if (_mouseAction == MouseAction::None) {
		mouseActionCancel();
	}
}

void ListWidget::mouseActionCancel() {
	_pressState = CursorState();
	_mouseAction = MouseAction::None;
	clearDragSelection();
	_wasSelectedText = false;
//	_widget->noSelectingScroll(); // #TODO scroll by drag
}

void ListWidget::performDrag() {
	if (_mouseAction != MouseAction::Dragging) return;

	auto uponSelected = false;
	if (_pressState.itemId && _pressState.inside) {
		if (hasSelectedItems()) {
			uponSelected = isItemUnderPressSelected();
		} else if (auto pressLayout = getExistingLayout(
				_pressState.itemId)) {
			HistoryStateRequest request;
			request.flags |= Text::StateRequest::Flag::LookupSymbol;
			auto dragState = pressLayout->getState(
				_pressState.cursor,
				request);
			uponSelected = isPressInSelectedText(dragState);
		}
	}
	auto pressedHandler = ClickHandler::getPressed();

	if (dynamic_cast<VoiceSeekClickHandler*>(pressedHandler.data())) {
		return;
	}

	TextWithEntities sel;
	QList<QUrl> urls;
	if (uponSelected) {
//		sel = getSelectedText();
	} else if (pressedHandler) {
		sel = { pressedHandler->dragText(), EntitiesInText() };
		//if (!sel.isEmpty() && sel.at(0) != '/' && sel.at(0) != '@' && sel.at(0) != '#') {
		//	urls.push_back(QUrl::fromEncoded(sel.toUtf8())); // Google Chrome crashes in Mac OS X O_o
		//}
	}
	//if (auto mimeData = MimeDataFromTextWithEntities(sel)) {
	//	clearDragSelection();
	//	_widget->noSelectingScroll();

	//	if (!urls.isEmpty()) mimeData->setUrls(urls);
	//	if (uponSelected && !Adaptive::OneColumn()) {
	//		auto selectedState = getSelectionState();
	//		if (selectedState.count > 0 && selectedState.count == selectedState.canForwardCount) {
	//			mimeData->setData(qsl("application/x-td-forward-selected"), "1");
	//		}
	//	}
	//	_controller->window()->launchDrag(std::move(mimeData));
	//	return;
	//} else {
	//	auto forwardMimeType = QString();
	//	auto pressedMedia = static_cast<HistoryMedia*>(nullptr);
	//	if (auto pressedItem = _pressState.layout) {
	//		pressedMedia = pressedItem->getMedia();
	//		if (_mouseCursorState == HistoryInDateCursorState || (pressedMedia && pressedMedia->dragItem())) {
	//			forwardMimeType = qsl("application/x-td-forward-pressed");
	//		}
	//	}
	//	if (auto pressedLnkItem = App::pressedLinkItem()) {
	//		if ((pressedMedia = pressedLnkItem->getMedia())) {
	//			if (forwardMimeType.isEmpty() && pressedMedia->dragItemByHandler(pressedHandler)) {
	//				forwardMimeType = qsl("application/x-td-forward-pressed-link");
	//			}
	//		}
	//	}
	//	if (!forwardMimeType.isEmpty()) {
	//		auto mimeData = std::make_unique<QMimeData>();
	//		mimeData->setData(forwardMimeType, "1");
	//		if (auto document = (pressedMedia ? pressedMedia->getDocument() : nullptr)) {
	//			auto filepath = document->filepath(DocumentData::FilePathResolveChecked);
	//			if (!filepath.isEmpty()) {
	//				QList<QUrl> urls;
	//				urls.push_back(QUrl::fromLocalFile(filepath));
	//				mimeData->setUrls(urls);
	//			}
	//		}

	//		// This call enters event loop and can destroy any QObject.
	//		_controller->window()->launchDrag(std::move(mimeData));
	//		return;
	//	}
	//}
}

void ListWidget::mouseActionFinish(const QPoint &screenPos, Qt::MouseButton button) {
	mouseActionUpdate(screenPos);

	auto pressState = base::take(_pressState);
	repaintItem(pressState.itemId);

	auto simpleSelectionChange = pressState.itemId
		&& pressState.inside
		&& !_pressWasInactive
		&& (button != Qt::RightButton)
		&& (_mouseAction == MouseAction::PrepareDrag
			|| _mouseAction == MouseAction::PrepareSelect);
	auto needSelectionToggle = simpleSelectionChange
		&& hasSelectedItems();
	auto needSelectionClear = simpleSelectionChange
		&& hasSelectedText();

	auto activated = ClickHandler::unpressed();
	if (_mouseAction == MouseAction::Dragging) {
		activated.clear();
	} else if (needSelectionToggle) {
		activated.clear();
	}

	_wasSelectedText = false;
	if (activated) {
		mouseActionCancel();
		App::activateClickHandler(activated, button);
		return;
	}

	if (needSelectionToggle) {
		toggleItemSelection(pressState.itemId);
	} else if (needSelectionClear) {
		clearSelected();
	} else if (_mouseAction == MouseAction::Selecting) {
		if (!_dragSelected.empty()) {
			applyDragSelection();
		} else if (!_selected.empty() && !_pressWasInactive) {
			auto selection = _selected.cbegin()->second;
			if (selection.text != FullSelection
				&& selection.text.from == selection.text.to) {
				clearSelected();
				//_controller->window()->setInnerFocus(); // #TODO focus
			}
		}
	}
	_mouseAction = MouseAction::None;
	_mouseSelectType = TextSelectType::Letters;
	//_widget->noSelectingScroll(); // #TODO scroll by drag
	//_widget->updateTopBarSelection();

#if defined Q_OS_LINUX32 || defined Q_OS_LINUX64
	//if (hasSelectedText()) { // #TODO linux clipboard
	//	setToClipboard(_selected.cbegin()->first->selectedText(_selected.cbegin()->second), QClipboard::Selection);
	//}
#endif // Q_OS_LINUX32 || Q_OS_LINUX64
}

void ListWidget::applyDragSelection() {
	applyDragSelection(_selected);
	clearDragSelection();
}

void ListWidget::applyDragSelection(SelectedMap &applyTo) const {
	if (_dragSelectAction == DragSelectAction::Selecting) {
		for (auto &[universalId,data] : _dragSelected) {
			changeItemSelection(applyTo, universalId, FullSelection);
		}
	} else if (_dragSelectAction == DragSelectAction::Deselecting) {
		for (auto &[universalId,data] : _dragSelected) {
			applyTo.remove(universalId);
		}
	}
}

void ListWidget::refreshHeight() {
	resize(width(), recountHeight());
}

int ListWidget::recountHeight() {
	auto cachedPadding = padding();
	auto result = cachedPadding.top();
	for (auto &section : _sections) {
		section.setTop(result);
		result += section.height();
	}
	return result
		+ cachedPadding.bottom();
}

void ListWidget::mouseActionUpdate() {
	mouseActionUpdate(_mousePosition);
}

void ListWidget::clearStaleLayouts() {
	for (auto i = _layouts.begin(); i != _layouts.end();) {
		if (i->second.stale) {
			i = _layouts.erase(i);
		} else {
			++i;
		}
	}
}

auto ListWidget::findSectionByItem(
		UniversalMsgId universalId) -> std::vector<Section>::iterator {
	return base::lower_bound(
		_sections,
		universalId,
		[](const Section &section, int universalId) {
			return section.minId() > universalId;
		});
}

auto ListWidget::findSectionAfterTop(
		int top) -> std::vector<Section>::iterator {
	return base::lower_bound(
		_sections,
		top,
		[](const Section &section, int top) {
			return (section.top() + section.height()) <= top;
		});
}

auto ListWidget::findSectionAfterTop(
		int top) const -> std::vector<Section>::const_iterator {
	return base::lower_bound(
		_sections,
		top,
		[](const Section &section, int top) {
		return (section.top() + section.height()) <= top;
	});
}

auto ListWidget::findSectionAfterBottom(
		std::vector<Section>::const_iterator from,
		int bottom) const -> std::vector<Section>::const_iterator {
	return std::lower_bound(
		from,
		_sections.end(),
		bottom,
		[](const Section &section, int bottom) {
			return section.top() < bottom;
		});
}

ListWidget::~ListWidget() = default;

} // namespace Media
} // namespace Info