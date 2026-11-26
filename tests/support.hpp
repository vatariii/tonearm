// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "core/text.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace testsupport {

// One text engine for the whole run, with the bundled fonts loaded.
inline tonearm::TextEngine &text()
{
	static std::unique_ptr<tonearm::TextEngine> engine = [] {
		auto e = tonearm::createTextEngine();
		const std::filesystem::path dir = std::filesystem::path(TONEARM_DATA_DIR) / "fonts";
		std::error_code ec;
		std::vector<std::string> fonts;
		for (const auto &f : std::filesystem::directory_iterator(dir, ec)) {
			const auto ext = f.path().extension().string();
			if (ext == ".otf" || ext == ".ttf")
				fonts.push_back(f.path().string());
		}
		e->addFontFiles(fonts); // the batch path the plugin uses
		return e;
	}();
	return *engine;
}

} // namespace testsupport
