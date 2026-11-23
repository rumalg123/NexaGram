/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "history/history_smart_selection.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/toast/toast.h"
#include "ui/widgets/fields/input_field.h"

#include "styles/style_chat.h"
#include "styles/style_layers.h"

namespace HistoryView {

// Uses Telegram's themed GenericBox and InputField instead of an OS-native
// input dialog, so custom day/night themes remain consistent.
inline void SmartSelectFilenameBox(
		not_null<Ui::GenericBox*> box,
		bool regex,
		Fn<void(SmartMessageSelection)> apply) {
	constexpr auto kQueryLimit = 256;
	box->setTitle(regex
		? tr::lng_context_smart_select_filename_regex()
		: tr::lng_context_smart_select_filename());

	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::factcheckField,
		Ui::InputField::Mode::NoNewlines,
		regex
			? tr::lng_context_smart_select_regex_placeholder()
			: tr::lng_context_smart_select_filename_placeholder()));

	const auto submit = [=] {
		const auto query = field->getLastText().trimmed();
		if (query.isEmpty() || query.size() > kQueryLimit) {
			field->showError();
			Ui::Toast::Show(tr::lng_smart_select_query_length(
				tr::now,
				lt_limit,
				QString::number(kQueryLimit)));
			return;
		}
		const auto accept = [=](SmartMessageSelection selection) {
			const auto callback = apply;
			box->closeBox();
			callback(std::move(selection));
		};
		if (regex) {
			auto expression = QRegularExpression(
				u"(*NO_JIT)(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=256)"_q + query,
				QRegularExpression::CaseInsensitiveOption);
			if (!expression.isValid()) {
				field->showError();
				Ui::Toast::Show(tr::lng_smart_select_regex_invalid(
					tr::now,
					lt_error,
					expression.errorString()));
				return;
			}
			accept(SmartMessageSelection(std::move(expression)));
		} else {
			accept(SmartMessageSelection(
				query.simplified().split(' ', Qt::SkipEmptyParts)));
		}
	};
	field->submits(
	) | rpl::on_next([=](auto) { submit(); }, field->lifetime());
	box->addButton(tr::lng_box_ok(), submit);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
}

} // namespace HistoryView
