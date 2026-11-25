/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "info/downloads/info_downloads_widget.h"

#include "info/downloads/info_downloads_inner_widget.h"
#include "info/downloads/info_downloads_status.h"
#include "info/info_controller.h"
#include "info/info_memento.h"
#include "info/info_wrap_widget.h"
#include "ui/boxes/confirm_box.h"
#include "ui/painter.h"
#include "ui/search_field_controller.h"
#include "ui/widgets/menu/menu_add_action_callback.h"
#include "ui/widgets/scroll_area.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/popup_menu.h"
#include "ui/text/format_values.h"
#include "ui/ui_utility.h"
#include "data/data_download_manager.h"
#include "data/data_user.h"
#include "history/history_item_helpers.h"
#include "main/main_session.h"
#include "window/window_session_controller.h"
#include "core/application.h"
#include "lang/lang_keys.h"
#include "styles/style_layers.h"
#include "styles/style_menu_icons.h"
#include "styles/style_overview.h"
#include "styles/style_widgets.h"

#include <QtGui/QAction>
#include <QtGui/QPaintEvent>

namespace Info::Downloads {

class ManagementBar final : public Ui::RpWidget {
public:
	ManagementBar(
			QWidget *parent,
			not_null<Controller*> controller,
			not_null<InnerWidget*> inner)
	: RpWidget(parent)
	, _controller(controller)
	, _inner(inner)
	, _account(controller->session().uniqueId())
	, _scope(this, st::boxLabel)
	, _summary(this, st::boxLabel)
	, _filter(this, rpl::single(QString()), st::defaultTableSmallButton)
	, _first(this, rpl::single(QString()), st::defaultTableSmallButton)
	, _second(this, rpl::single(QString()), st::defaultTableSmallButton) {
		for (const auto button : { _filter.data(), _first.data(), _second.data() }) {
			button->setTextTransform(Ui::RoundButtonTextTransform::NoTransform);
			button->setFocusPolicy(Qt::TabFocus);
		}
		_filter->setClickedCallback([=] { showFilters(); });
		_first->setClickedCallback([=] {
			if (_hasSelection) {
				showSelectedActions();
			} else {
				dispatch(Action::Pause, eligible(Action::Pause, false));
			}
		});
		_second->setClickedCallback([=] {
			if (_hasSelection) {
				_inner->selectionAction(SelectionAction::Clear);
			} else {
				dispatch(Action::Resume, eligible(Action::Resume, false));
			}
		});
		_inner->selectedListValue(
		) | rpl::on_next([=](const SelectedItems &selected) {
			_hasSelection = !selected.list.empty();
			auto &manager = Core::App().downloadManager();
			_selected.clear();
			for (const auto &row : selected.list) {
				if (row.globalId.sessionUniqueId == _account) {
					if (const auto item = MessageByGlobalId(row.globalId)) {
						const auto id = manager.pendingIdForRow(item);
						if (!id.job.isNull()) {
							_selected.push_back(id);
						}
					}
				}
			}
			refresh(true);
		}, lifetime());
		auto &manager = Core::App().downloadManager();
		manager.pendingViewsChanged(
		) | rpl::on_next([=] { refresh(true); }, lifetime());
		manager.pendingProgressChanged(
		) | rpl::on_next([=] { refresh(false); }, lifetime());
		manager.pendingCleanupDeferred(
		) | rpl::on_next([=](uint64 account) {
			if (account == _account) {
				_controller->parentController()->showToast(
					tr::lng_downloads_cleanup_deferred(tr::now));
			}
		}, lifetime());
		refresh(true);
	}

protected:
	int resizeGetHeight(int width) override {
		const auto gap = style::ConvertScale(6);
		const auto space = std::max(width - 4 * gap, 3);
		const auto third = space / 3;
		auto top = gap;
		const auto available = std::max(width - 2 * gap, 0);
		_scope->setText(st::normalFont->elided(_scopeText, available));
		_scope->resizeToWidth(available);
		_scope->moveToLeft(gap, top, width);
		top += _scope->height();
		_summary->setText(st::normalFont->elided(_summaryText, available));
		_summary->resizeToWidth(available);
		_summary->moveToLeft(gap, top, width);
		top += _summary->height() + gap;
		_filter->setFullWidth(third);
		_first->setFullWidth(third);
		_second->setFullWidth(space - 2 * third);
		_filter->moveToLeft(gap, top, width);
		_first->moveToLeft(2 * gap + third, top, width);
		_second->moveToLeft(3 * gap + 2 * third, top, width);
		return top + _first->height() + gap;
	}

	void paintEvent(QPaintEvent *event) override {
		auto p = QPainter(this);
		p.fillRect(event->rect(), st::windowBg);
		p.fillRect(0, 0, width(), st::lineWidth, st::linksBorderFg);
		if (_bytes.total > 0) {
			const auto fill = int(width() * (_bytes.written / float64(_bytes.total)));
			p.fillRect(style::rtlrect(0, 0, fill, st::lineWidth, width()), st::windowBgActive);
		}
	}

private:
	using Filter = Storage::DownloadFilter;
	using Status = Storage::DownloadStatus;
	using Action = Storage::DownloadAction;
	using Id = Storage::PendingDownloadId;

	static QString CountLabel(QString label, int count) {
		return label + u" ("_q + QString::number(count) + u")"_q;
	}

	std::vector<Id> eligible(Action action, bool selected) const {
		auto result = std::vector<Id>();
		auto &manager = Core::App().downloadManager();
		if (manager.pendingBatchActive(_account)) {
			return result;
		}
		for (const auto &view : manager.pendingViews(_account)) {
			if (view.allows(action) && (!selected
				|| ranges::find(_selected, view.id) != end(_selected))) {
				result.push_back(view.id);
			}
		}
		return result;
	}

	void setButton(not_null<Ui::RoundButton*> button, QString text, bool enabled = true) {
		button->setText(rpl::single(text));
		button->setToolTip(text);
		button->setAccessibleName(text);
		button->setDisabled(!enabled);
	}

	void refresh(bool controls) {
		const auto views = Core::App().downloadManager().pendingViews(_account);
		auto downloading = 0;
		auto paused = 0;
		auto interrupted = 0;
		auto failed = 0;
		auto pending = 0;
		auto rate = int64(0);
		_bytes = {};
		for (const auto &view : views) {
			const auto status = view.transfer.status;
			if (status != Status::Cancelling) {
				++pending;
				_bytes.add(view.transfer.written, view.total);
			}
			downloading += (status == Status::Downloading);
			paused += Storage::MatchesDownloadFilter(status, Filter::Paused);
			interrupted += Storage::MatchesDownloadFilter(status, Filter::Interrupted);
			failed += Storage::MatchesDownloadFilter(status, Filter::Failed);
			rate += view.transfer.bytesPerSecond;
		}
		const auto middle = u" · "_q;
		_scopeText = CountLabel(tr::lng_downloads_pending_scope(
			tr::now, lt_user, _controller->session().user()->name()), pending);
		if (_bytes.total) {
			_scopeText += middle + Ui::FormatDownloadText(_bytes.written, _bytes.total)
				+ middle + QString::number(_bytes.percent()) + u"%"_q;
		}
		_summaryText = CountLabel(StatusText(Status::Downloading), downloading)
			+ middle + CountLabel(FilterText(Filter::Paused), paused)
			+ middle + CountLabel(FilterText(Filter::Interrupted), interrupted)
			+ middle + CountLabel(FilterText(Filter::Failed), failed);
		if (rate > 0) {
			_summaryText += middle + tr::lng_downloads_transfer_rate(
				tr::now, lt_size, Ui::FormatSizeText(rate));
		}
		_scope->setToolTip(_scopeText);
		_summary->setToolTip(_summaryText);
		if (controls) {
			setButton(_filter.data(), (_currentFilter == Filter::All)
				? tr::lng_downloads_filter_menu(tr::now)
				: FilterText(_currentFilter));
			if (_hasSelection) {
				setButton(_first.data(), tr::lng_downloads_selection_actions(tr::now),
					!Core::App().downloadManager().pendingBatchActive(_account));
				setButton(_second.data(), tr::lng_downloads_clear_selection(tr::now));
			} else {
				const auto pauses = int(eligible(Action::Pause, false).size());
				const auto resumes = int(eligible(Action::Resume, false).size());
				setButton(_first.data(), CountLabel(tr::lng_downloads_pause_all(tr::now), pauses), pauses > 0);
				setButton(_second.data(), CountLabel(tr::lng_downloads_resume_all(tr::now), resumes), resumes > 0);
			}
		}
		resizeToWidth(width());
		update();
	}

	void showFilters() {
		_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
		for (const auto filter : { Filter::All, Filter::Active, Filter::Paused,
				Filter::Interrupted, Filter::Failed, Filter::Completed }) {
			_menu->addAction(FilterText(filter), [=] {
				_currentFilter = filter;
				_inner->setDownloadFilter(filter);
				refresh(true);
			});
		}
		_menu->addSeparator();
		_menu->addAction(tr::lng_downloads_select_visible(tr::now), [=] {
			_inner->selectAllResults();
		}, &st::menuIconSelect);
		_menu->popup(_filter->mapToGlobal(QPoint(0, 0)));
	}

	void showSelectedActions() {
		_menu = base::make_unique_q<Ui::PopupMenu>(this, st::popupMenuWithIcons);
		const auto add = [&](Action action, QString title, const style::icon *icon) {
			auto ids = eligible(action, true);
			const auto count = int(ids.size());
			const auto item = _menu->addAction(CountLabel(std::move(title), count),
				[=, ids = std::move(ids)] { dispatch(action, ids, true); }, icon);
			item->setEnabled(count > 0);
		};
		add(Action::Pause, tr::lng_downloads_pause_selected(tr::now), &st::menuIconDownload);
		add(Action::Resume, tr::lng_downloads_resume_selected(tr::now), &st::menuIconDownload);
		add(Action::Cancel, tr::lng_downloads_cancel_selected(tr::now), &st::menuIconCancel);
		_menu->popup(_first->mapToGlobal(QPoint(0, 0)));
	}

	void dispatch(Action action, std::vector<Id> ids, bool selected = false) {
		if (selected) {
			std::erase_if(ids, [&](const auto &id) {
				return ranges::find(_selected, id) == end(_selected);
			});
		}
		if (ids.empty()) {
			return;
		}
		const auto account = _account;
		const auto run = crl::guard(this, [=] {
			if (_controller->session().uniqueId() != account) {
				return;
			}
			auto chosen = ids;
			if (selected) {
				std::erase_if(chosen, [&](const auto &id) {
					return ranges::find(_selected, id) == end(_selected);
				});
			}
			const auto removed = int(ids.size() - chosen.size());
			Core::App().downloadManager().actOnPendingBatch(account, std::move(chosen), action,
				crl::guard(this, [=](Data::DownloadActionResult result) {
					result.skipped += removed;
					const auto middle = u" · "_q;
					_controller->parentController()->showToast(
						tr::lng_downloads_bulk_accepted(
							tr::now, lt_result_count, QString::number(result.accepted))
						+ middle + tr::lng_downloads_bulk_skipped(
							tr::now, lt_result_count, QString::number(result.skipped))
						+ middle + tr::lng_downloads_bulk_failed(
							tr::now, lt_result_count, QString::number(result.failed)));
					refresh(true);
				}));
			refresh(true);
		});
		if (action == Action::Cancel) {
			_controller->parentController()->show(Ui::MakeConfirmBox({
				.text = tr::lng_downloads_bulk_confirm(tr::now,
					lt_count, ids.size(), lt_user, _controller->session().user()->name()),
				.confirmed = crl::guard(this, [=](Fn<void()> close) {
					close();
					run();
				}),
				.confirmText = tr::lng_downloads_cancel_selected(tr::now),
				.confirmStyle = &st::attentionBoxButton,
			}));
		} else {
			run();
		}
	}

	const not_null<Controller*> _controller;
	const not_null<InnerWidget*> _inner;
	const uint64 _account;
	object_ptr<Ui::FlatLabel> _scope;
	object_ptr<Ui::FlatLabel> _summary;
	object_ptr<Ui::RoundButton> _filter;
	object_ptr<Ui::RoundButton> _first;
	object_ptr<Ui::RoundButton> _second;
	base::unique_qptr<Ui::PopupMenu> _menu;
	QString _scopeText;
	QString _summaryText;
	std::vector<Id> _selected;
	Storage::DownloadByteTotals _bytes;
	Filter _currentFilter = Filter::All;
	bool _hasSelection = false;
};

Memento::Memento(not_null<Controller*> controller)
: ContentMemento(Tag{})
, _media(controller) {
}

Memento::Memento(not_null<UserData*> self)
: ContentMemento(Tag{})
, _media(self, 0, Media::Type::File) {
}

Memento::~Memento() = default;

Section Memento::section() const {
	return Section(Section::Type::Downloads);
}

object_ptr<ContentWidget> Memento::createWidget(
		QWidget *parent,
		not_null<Controller*> controller,
		const QRect &geometry) {
	auto result = object_ptr<Widget>(parent, controller);
	result->setInternalState(geometry, this);
	return result;
}

Widget::Widget(
	QWidget *parent,
	not_null<Controller*> controller)
: ContentWidget(parent, controller) {
	_inner = setInnerWidget(object_ptr<InnerWidget>(
		this,
		controller));
	_inner->setScrollHeightValue(scrollHeightValue());
	_inner->scrollToRequests(
	) | rpl::on_next([this](Ui::ScrollToRequest request) {
		scrollTo(request);
	}, _inner->lifetime());
	if (Storage::PendingDownloadsEnabled()) {
		_management = Ui::CreateChild<ManagementBar>(this, controller, _inner);
		_management->show();
	}
}

void Widget::resizeEvent(QResizeEvent *e) {
	if (_management) {
		_management->resizeToWidth(width());
		_management->moveToLeft(0, height() - _management->height(), width());
		setScrollBottomSkip(_management->height());
	}
	ContentWidget::resizeEvent(e);
}

bool Widget::showInternal(not_null<ContentMemento*> memento) {
	if (auto downloadsMemento = dynamic_cast<Memento*>(memento.get())) {
		restoreState(downloadsMemento);
		return true;
	}
	return false;
}

void Widget::setInternalState(
		const QRect &geometry,
		not_null<Memento*> memento) {
	setGeometry(geometry);
	Ui::SendPendingMoveResizeEvents(this);
	restoreState(memento);
}

std::shared_ptr<ContentMemento> Widget::doCreateMemento() {
	auto result = std::make_shared<Memento>(controller());
	saveState(result.get());
	return result;
}

void Widget::saveState(not_null<Memento*> memento) {
	memento->setScrollTop(scrollTopSave());
	_inner->saveState(memento);
}

void Widget::restoreState(not_null<Memento*> memento) {
	_inner->restoreState(memento);
	scrollTopRestore(memento->scrollTop());
}

rpl::producer<SelectedItems> Widget::selectedListValue() const {
	return _inner->selectedListValue();
}

void Widget::selectionAction(SelectionAction action) {
	_inner->selectionAction(action);
}

void Widget::fillTopBarMenu(const Ui::Menu::MenuCallback &addAction) {
	AddDeleteAllAction(this, controller(), addAction);
}

void AddDeleteAllAction(
		not_null<QWidget*> widget,
		not_null<Controller*> controller,
		const Ui::Menu::MenuCallback &addAction) {
	const auto window = controller->parentController();
	const auto deleteAll = [=] {
		auto &manager = Core::App().downloadManager();
		const auto phrase = tr::lng_downloads_delete_sure_all(tr::now);
		const auto added = manager.loadedHasNonCloudFile()
			? QString()
			: tr::lng_downloads_delete_in_cloud(tr::now);
		const auto deleteSure = [=, &manager](Fn<void()> close) {
			Ui::PostponeCall(widget, close);
			manager.deleteAll();
		};
		window->show(Ui::MakeConfirmBox({
			.text = phrase + (added.isEmpty() ? QString() : "\n\n" + added),
			.confirmed = deleteSure,
			.confirmText = tr::lng_box_delete(tr::now),
			.confirmStyle = &st::attentionBoxButton,
		}));
	};
	addAction(
		tr::lng_context_delete_all_files(tr::now),
		deleteAll,
		&st::menuIconDelete);
}

rpl::producer<QString> Widget::title() {
	return tr::lng_downloads_section();
}

std::shared_ptr<Info::Memento> Make(not_null<UserData*> self) {
	return std::make_shared<Info::Memento>(
		std::vector<std::shared_ptr<ContentMemento>>(
			1,
			std::make_shared<Memento>(self)));
}

} // namespace Info::Downloads

