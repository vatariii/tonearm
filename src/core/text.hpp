// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "image.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tonearm {

struct FontSpec {
	std::string family = "Inter";
	int weight = 400;          // 100..900
	bool italic = false;
	float sizePx = 16;         // em size in device pixels
	float trackingEm = 0;      // letter spacing in em units
	bool tabularNumbers = false;
	bool uppercase = false;
};

struct TextMetrics {
	float width = 0;      // advance width of the laid out text
	float ascent = 0;     // from top of line box to baseline
	float descent = 0;
	float lineHeight = 0;
};

struct TextRun {
	Image image;          // premultiplied, tightly sized to the line box plus `pad` on each side
	TextMetrics metrics;
	int pad = 0;          // transparent margin baked into the image (room for glow)
	bool truncated = false;
};

// Platform text rasterizer. Implementations: Pango/Cairo (Linux), DirectWrite (Windows).
// Implementations must be safe to call from one worker thread at a time.
class TextEngine {
public:
	virtual ~TextEngine() = default;

	// Lays out a single line. If maxWidth > 0 and the text is wider, it is ellipsized when
	// `ellipsize` is true, otherwise rendered at full width (for marquee scrolling).
	virtual TextRun render(const std::string &utf8, const FontSpec &font, const Color &color, float maxWidth, bool ellipsize,
			       int pad) = 0;

	virtual TextMetrics measure(const std::string &utf8, const FontSpec &font) = 0;

	// Registers a font file shipped with the plugin. Returns false if it could not be loaded.
	virtual bool addFontFile(const std::string &path) = 0;
	// Registers several font files at once and returns the ones that failed. Backends
	// that rebuild a font collection per file override this to rebuild only once.
	virtual std::vector<std::string> addFontFiles(const std::vector<std::string> &paths)
	{
		std::vector<std::string> failed;
		for (const std::string &p : paths)
			if (!addFontFile(p))
				failed.push_back(p);
		return failed;
	}

	virtual std::vector<std::string> families() = 0;
};

std::unique_ptr<TextEngine> createTextEngine();

} // namespace tonearm
