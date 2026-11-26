// SPDX-License-Identifier: GPL-2.0-or-later
// DirectWrite text backend (Windows). Bundled fonts are combined with the system
// font set, so users can also pick any installed family, and DirectWrite's system
// fallback covers scripts the bundled fonts lack (Japanese, Korean, Arabic, ...).
#include "core/text.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace tonearm {

namespace {

template <typename T> class Com {
public:
	Com() = default;
	~Com() { reset(); }
	Com(const Com &) = delete;
	Com &operator=(const Com &) = delete;
	T **put()
	{
		reset();
		return &p_;
	}
	T *get() const { return p_; }
	T *operator->() const { return p_; }
	explicit operator bool() const { return p_ != nullptr; }
	void reset()
	{
		if (p_)
			p_->Release();
		p_ = nullptr;
	}

private:
	T *p_ = nullptr;
};

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return {};
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

std::string narrow(const std::wstring &w)
{
	if (w.empty())
		return {};
	const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<size_t>(n), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
	return s;
}

class DWriteTextEngine final : public TextEngine {
public:
	DWriteTextEngine()
	{
		// S_OK and S_FALSE (already initialized in the same mode) each take a reference
		// that this object must return; RPC_E_CHANGED_MODE (the thread is already an STA)
		// takes none, and COM stays usable in that case. The engine is created and
		// destroyed on the same thread (RasterWorker::run), so the pair stays balanced.
		comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
		if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory5),
					       reinterpret_cast<IUnknown **>(factory_.put()))))
			factory_.reset();
		if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.put())))
			d2d_.reset();
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic_.put()))))
			wic_.reset();
		rebuildCollection();
	}

	~DWriteTextEngine() override
	{
		// Every COM object goes before the apartment reference is returned.
		collection_.reset();
		wic_.reset();
		d2d_.reset();
		factory_.reset();
		if (comInitialized_)
			CoUninitialize();
	}

	bool addFontFile(const std::string &path) override
	{
		if (!factory_)
			return false;
		files_.push_back(widen(path));
		return rebuildCollection();
	}

	std::vector<std::string> addFontFiles(const std::vector<std::string> &paths) override
	{
		// Each rebuild enumerates the whole system font set, so check the files one by
		// one and rebuild the collection once at the end.
		std::vector<std::string> failed;
		if (!factory_)
			return paths;
		for (const std::string &p : paths) {
			const std::wstring w = widen(p);
			Com<IDWriteFontFile> file;
			BOOL supported = FALSE;
			DWRITE_FONT_FILE_TYPE fileType{};
			DWRITE_FONT_FACE_TYPE faceType{};
			UINT32 faces = 0;
			if (FAILED(factory_->CreateFontFileReference(w.c_str(), nullptr, file.put())) || !file ||
			    FAILED(file->Analyze(&supported, &fileType, &faceType, &faces)) || !supported) {
				failed.push_back(p);
				continue;
			}
			files_.push_back(w);
		}
		rebuildCollection();
		return failed;
	}

	std::vector<std::string> families() override
	{
		std::vector<std::string> out;
		if (!collection_)
			return out;
		const UINT32 n = collection_->GetFontFamilyCount();
		for (UINT32 i = 0; i < n; ++i) {
			Com<IDWriteFontFamily1> fam;
			Com<IDWriteLocalizedStrings> names;
			if (FAILED(collection_->GetFontFamily(i, fam.put())) || FAILED(fam->GetFamilyNames(names.put())))
				continue;
			UINT32 idx = 0;
			BOOL exists = FALSE;
			names->FindLocaleName(L"en-us", &idx, &exists);
			UINT32 len = 0;
			names->GetStringLength(exists ? idx : 0, &len);
			std::wstring w(len + 1, L'\0');
			names->GetString(exists ? idx : 0, w.data(), len + 1);
			w.resize(len);
			out.push_back(narrow(w));
		}
		std::sort(out.begin(), out.end());
		return out;
	}

	TextMetrics measure(const std::string &utf8, const FontSpec &font) override
	{
		TextMetrics m;
		Com<IDWriteTextLayout> layout;
		if (makeLayout(utf8, font, 0, false, layout))
			m = metricsOf(layout.get());
		return m;
	}

	TextRun render(const std::string &utf8, const FontSpec &font, const Color &color, float maxWidth, bool ellipsize, int pad) override
	{
		TextRun run;
		run.pad = pad;
		Com<IDWriteTextLayout> natural;
		if (!makeLayout(utf8, font, 0, false, natural))
			return run;
		const TextMetrics nm = metricsOf(natural.get());
		Com<IDWriteTextLayout> layout;
		IDWriteTextLayout *use = natural.get();
		if (ellipsize && maxWidth > 0 && nm.width > maxWidth) {
			if (makeLayout(utf8, font, maxWidth, true, layout)) {
				use = layout.get();
				run.truncated = true;
			}
		}
		run.metrics = metricsOf(use);
		if (run.truncated)
			run.metrics.width = std::min(run.metrics.width, maxWidth);

		const UINT w = static_cast<UINT>(std::ceil(run.metrics.width)) + pad * 2 + 2;
		const UINT h = static_cast<UINT>(std::ceil(run.metrics.lineHeight)) + pad * 2;
		Com<IWICBitmap> bitmap;
		if (!wic_ || !d2d_ || FAILED(wic_->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.put())) ||
		    !bitmap)
			return run;

		const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
		Com<ID2D1RenderTarget> rt;
		if (FAILED(d2d_->CreateWicBitmapRenderTarget(bitmap.get(), props, rt.put())) || !rt)
			return run;
		rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
		// Antialiased in both directions with grid fitting off. With vertical grid fitting,
		// DirectWrite snaps the small gap above Inter's i stem shut at overlay sizes and
		// "Alicia" reads "Allcia". The overlay is composited and often scaled, so outlines
		// placed exactly as designed matter more than crisp horizontal stems.
		Com<IDWriteRenderingParams2> params;
		IDWriteFactory2 *f2 = factory_.get();
		if (SUCCEEDED(f2->CreateCustomRenderingParams(1.8f, 0.5f, 0.5f, 0.0f, DWRITE_PIXEL_GEOMETRY_FLAT,
							      DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_GRID_FIT_MODE_DISABLED,
							      params.put())))
			rt->SetTextRenderingParams(params.get());

		Com<ID2D1SolidColorBrush> brush;
		if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(color.r, color.g, color.b, color.a), brush.put())) || !brush)
			return run;
		rt->BeginDraw();
		rt->Clear(D2D1::ColorF(0, 0, 0, 0));
		rt->DrawTextLayout(D2D1::Point2F(static_cast<float>(pad), static_cast<float>(pad)), use, brush.get(),
				   D2D1_DRAW_TEXT_OPTIONS_NONE);
		if (FAILED(rt->EndDraw()))
			return run;

		Image img(static_cast<int>(w), static_cast<int>(h));
		if (FAILED(bitmap->CopyPixels(nullptr, w * 4, w * 4 * h, img.data())))
			return run;
		run.image = std::move(img);
		return run;
	}

private:
	bool rebuildCollection()
	{
		if (!factory_)
			return false;
		Com<IDWriteFontSetBuilder1> builder;
		if (FAILED(factory_->CreateFontSetBuilder(builder.put())))
			return false;
		bool allOk = true;
		for (const std::wstring &f : files_) {
			Com<IDWriteFontFile> file;
			if (FAILED(factory_->CreateFontFileReference(f.c_str(), nullptr, file.put())) || FAILED(builder->AddFontFile(file.get())))
				allOk = false;
		}
		// Bundled fonts first, then everything installed on the system.
		Com<IDWriteFontSet> system;
		if (SUCCEEDED(factory_->GetSystemFontSet(system.put())))
			builder->AddFontSet(system.get());
		Com<IDWriteFontSet> set;
		if (FAILED(builder->CreateFontSet(set.put())))
			return false;
		Com<IDWriteFontCollection1> collection;
		if (FAILED(factory_->CreateFontCollectionFromFontSet(set.get(), collection.put())))
			return false;
		collection_.reset();
		*collection_.put() = collection.get();
		collection.get()->AddRef();
		return allOk;
	}

	bool makeLayout(const std::string &utf8, const FontSpec &font, float maxWidth, bool ellipsize, Com<IDWriteTextLayout> &out)
	{
		if (!factory_)
			return false;
		std::wstring text = widen(utf8);
		for (wchar_t &c : text)
			if (c == L'\n' || c == L'\r' || c == L'\t')
				c = L' ';
		if (font.uppercase && !text.empty()) {
			std::wstring up(text.size() * 2 + 8, L'\0');
			const int n = LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE | LCMAP_LINGUISTIC_CASING, text.c_str(),
						    static_cast<int>(text.size()), up.data(), static_cast<int>(up.size()), nullptr, nullptr, 0);
			if (n > 0) {
				up.resize(static_cast<size_t>(n));
				text = up;
			}
		}

		const std::wstring family = widen(font.family);
		Com<IDWriteTextFormat> format;
		if (FAILED(factory_->CreateTextFormat(family.c_str(), collection_.get(),
						      static_cast<DWRITE_FONT_WEIGHT>(std::clamp(font.weight, 100, 900)),
						      font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
						      font.sizePx, L"en-us", format.put())))
			return false;
		format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		if (ellipsize) {
			Com<IDWriteInlineObject> sign;
			factory_->CreateEllipsisTrimmingSign(format.get(), sign.put());
			DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
			format->SetTrimming(&trimming, sign.get());
		}

		const float width = maxWidth > 0 ? maxWidth : 100000.0f;
		Com<IDWriteTextLayout> layout;
		if (FAILED(factory_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format.get(), width, font.sizePx * 4,
						      layout.put())))
			return false;
		const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(text.size())};
		if (font.trackingEm != 0) {
			Com<IDWriteTextLayout1> l1;
			if (SUCCEEDED(layout->QueryInterface(IID_PPV_ARGS(l1.put()))))
				l1->SetCharacterSpacing(0, font.trackingEm * font.sizePx, 0, all);
		}
		if (font.tabularNumbers) {
			Com<IDWriteTypography> typo;
			if (SUCCEEDED(factory_->CreateTypography(typo.put()))) {
				typo->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
				layout->SetTypography(typo.get(), all);
			}
		}
		out.reset();
		*out.put() = layout.get();
		layout.get()->AddRef();
		return true;
	}

	static TextMetrics metricsOf(IDWriteTextLayout *layout)
	{
		TextMetrics m;
		DWRITE_TEXT_METRICS tm{};
		if (FAILED(layout->GetMetrics(&tm)))
			return m;
		m.width = tm.widthIncludingTrailingWhitespace;
		m.lineHeight = tm.height;
		DWRITE_LINE_METRICS lm{};
		UINT32 count = 0;
		if (SUCCEEDED(layout->GetLineMetrics(&lm, 1, &count)) && count > 0) {
			m.ascent = lm.baseline;
			m.descent = lm.height - lm.baseline;
		}
		return m;
	}

	Com<IDWriteFactory5> factory_;
	Com<ID2D1Factory> d2d_;
	Com<IWICImagingFactory> wic_;
	Com<IDWriteFontCollection1> collection_;
	std::vector<std::wstring> files_;
	bool comInitialized_ = false;
};

} // namespace

std::unique_ptr<TextEngine> createTextEngine()
{
	return std::make_unique<DWriteTextEngine>();
}

} // namespace tonearm
