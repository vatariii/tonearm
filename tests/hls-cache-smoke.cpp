// SPDX-License-Identifier: GPL-2.0-or-later
// Windows CI smoke test: exercise the same finite-HLS cache used by Tonearm.
#include "obs/hls-cache.hpp"
#include "obs/motion-index.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char **argv)
{
	if (argc < 2 || argc > 3)
		return 2;
	const auto quality = argc == 3 && std::string(argv[2]) == "balanced" ?
		tonearm::MotionQuality::Balanced : tonearm::MotionQuality::Maximum;
	std::atomic<bool> cancelled{false};
	const auto local = tonearm::cacheMotionHls(argv[1], cancelled, quality);
	if (!local) {
		std::cerr << "HLS caching failed\n";
		return 1;
	}
	const std::filesystem::path manifest = std::filesystem::path(std::u8string(local->begin(), local->end()));
	// Persistent album index must survive provider recreation and identify
	// the same release across different track artists.
	tonearm::MotionQuery q;
	q.artist = "Gorillaz";
	q.albumArtist = "Gorillaz";
	q.album = "Plastic Beach";
	q.title = "Melancholy Hill";
	q.preferred = tonearm::MotionShape::Square;
	q.quality = quality;
	const tonearm::MotionAsset asset{*local, tonearm::MotionShape::Square};
	if (!tonearm::rememberMotionForAlbum(q, asset)) {
		std::cerr << "Failed to persist album motion index\n";
		return 1;
	}
	q.title = "Empire Ants";
	q.artist = "Gorillaz feat. Little Dragon";
	const auto replay = tonearm::cachedMotionForAlbum(q);
	if (!replay || replay->location != *local) {
		std::cerr << "Cached motion video not found for a different track of the same album\n";
		return 1;
	}
	q.album = "Plastic Beach (Super Deluxe)";
	if (tonearm::cachedMotionForAlbum(q)) {
		std::cerr << "Persistent index confused two album editions\n";
		return 1;
	}
	std::cout << "Persistent album replay lookup passed\n";
	if (manifest.extension() == ".mp4") {
		std::ifstream file(manifest, std::ios::binary);
		char header[8]{};
		file.read(header, 8);
		if (file.gcount() != 8 || std::string(header + 4, 4) != "ftyp" ||
		    std::filesystem::file_size(manifest) < 4096) {
			std::cerr << "Cached MP4 is invalid\n";
			return 1;
		}
		std::cout << "Cached full local Apple MP4: bytes="
			  << std::filesystem::file_size(manifest) << "\n";
		return 0;
	}
	std::ifstream stream(manifest);
	if (!stream) {
		std::cerr << "Missing local manifest\n";
		return 1;
	}
	size_t count = 0;
	std::string line;
	bool ended = false;
	while (std::getline(stream, line)) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line == "#EXT-X-ENDLIST")
			ended = true;
		if (!line.empty() && line.front() != '#') {
			++count;
			if (!std::filesystem::exists(manifest.parent_path() / std::filesystem::path(std::u8string(line.begin(), line.end())))) {
				std::cerr << "Missing local HLS segment\n";
				return 1;
			}
		}
	}
	if (!ended || count < 1) {
		std::cerr << "Incomplete HLS VOD\n";
		return 1;
	}
	std::cout << "Cached complete local HLS video: segments=" << count << "\n";
	return 0;
}
