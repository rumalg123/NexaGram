/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/toast/toast.h"

#include <QtCore/QRegularExpression>
#include "lang/lang_keys.h"
#include "styles/style_chat.h"
#include "styles/style_layers.h"

namespace HistoryView {

// Uses Telegram's themed GenericBox and InputField instead of an OS-native
// input dialog, so custom day/night themes remain consistent.
inline void SmartSelectFilenameBox(
		not_null<Ui::GenericBox*> box,
		bool regex,
		Fn<void(QString)> apply) {
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

	box->addButton(tr::lng_box_ok(), [=] {
		const auto query = field->getLastText().trimmed();
		if (query.isEmpty() || query.size() > 256) {
			field->showError();
			Ui::Toast::Show(u"Enter 1 to 256 characters."_q);
			return;
		}
		if (regex) {
			const auto expression = QRegularExpression(
				query, QRegularExpression::CaseInsensitiveOption);
			if (!expression.isValid()) {
				field->showError();
				Ui::Toast::Show(u"Invalid regex: %1"_q.arg(
					expression.errorString()));
				return;
			}
		}
		box->closeBox();
		apply(query);
	});
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
	box->setFocusCallback([=] {
		field->setFocusFast();
	});
}

} // namespace HistoryView
