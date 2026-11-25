/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

namespace Media::Streaming {

[[nodiscard]] constexpr bool IsCompletePart(
		std::int64_t size,
		std::int64_t offset,
		std::int64_t received,
		std::int64_t partSize) {
	return offset >= 0 && offset < size && !(offset % partSize)
		&& received == std::min(partSize, size - offset);
}

class DownloadAttachment final {
public:
	using Id = std::uint64_t;

	[[nodiscard]] Id attach() {
		_pending.clear();
		return _active = ++_last;
	}

	bool detach(Id id) {
		if (!matches(id)) {
			return false;
		}
		_pending.clear();
		_active = 0;
		return true;
	}

	[[nodiscard]] Id active() const {
		return _active;
	}

	[[nodiscard]] bool matches(Id id) const {
		return id && id == _active;
	}

	bool request(Id id, std::uint32_t offset) {
		return matches(id) && _pending.insert(offset).second;
	}

	[[nodiscard]] bool pending(Id id, std::uint32_t offset) const {
		return matches(id) && _pending.contains(offset);
	}

	bool consume(Id id, std::uint32_t offset) {
		return matches(id) && _pending.erase(offset);
	}

private:
	Id _last = 0;
	Id _active = 0;
	std::set<std::uint32_t> _pending;

};

class PartRequestOwners final {
public:
	enum class Consumer { Playback, Download };
	using Offset = std::uint32_t;

	void add(Consumer consumer, Offset offset) {
		offsets(consumer).insert(offset);
	}

	[[nodiscard]] bool contains(Consumer consumer, Offset offset) const {
		return offsets(consumer).contains(offset);
	}

	[[nodiscard]] bool empty(Consumer consumer) const {
		return offsets(consumer).empty();
	}

	bool remove(Consumer consumer, Offset offset) {
		return offsets(consumer).erase(offset) != 0;
	}

	[[nodiscard]] bool release(Consumer consumer, Offset offset) {
		return remove(consumer, offset) && !contains(other(consumer), offset);
	}

	[[nodiscard]] std::set<Offset> detach(Consumer consumer) {
		auto released = std::exchange(offsets(consumer), {});
		const auto &keep = offsets(other(consumer));
		std::erase_if(released, [&](Offset offset) { return keep.contains(offset); });
		return released;
	}

private:
	static Consumer other(Consumer consumer) {
		return (consumer == Consumer::Playback)
			? Consumer::Download
			: Consumer::Playback;
	}

	std::set<Offset> &offsets(Consumer consumer) {
		return (consumer == Consumer::Playback) ? _playback : _downloads;
	}

	const std::set<Offset> &offsets(Consumer consumer) const {
		return (consumer == Consumer::Playback) ? _playback : _downloads;
	}

	std::set<Offset> _playback;
	std::set<Offset> _downloads;

};

class SharedRequestOrder final {
public:
	[[nodiscard]] bool takeDownload(bool queuedPlayback, bool queuedDownload) {
		if (!queuedDownload) {
			_playbackTaken = 0;
			return false;
		}
		if (!queuedPlayback || _playbackTaken == 4) {
			_playbackTaken = 0;
			return true;
		}
		++_playbackTaken;
		return false;
	}

private:
	int _playbackTaken = 0;

};

} // namespace Media::Streaming
