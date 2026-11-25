#include "log_redact.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <shlobj.h>

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/sink.h>
#include <spdlog/spdlog.h>

namespace
{
	struct Replacement
	{
		std::string from, to;
	};
	std::vector<Replacement> g_replacements;  // longest first; set once before the sinks are wrapped

	bool SameChar(char a, char b)
	{
		const auto fold = [](char c) { return c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
		return fold(a) == fold(b);
	}

	std::size_t FindFolded(const std::string& text, const std::string& what, std::size_t from)
	{
		if (what.empty() || text.size() < what.size()) return std::string::npos;
		for (std::size_t i = from; i + what.size() <= text.size(); ++i)
			if (std::equal(what.begin(), what.end(), text.begin() + static_cast<std::ptrdiff_t>(i), SameChar)) return i;
		return std::string::npos;
	}

	// "\Users\<name>\" and "\home\<name>\" (either slash, any case): the
	// name becomes <user>.
	void RedactUserSegments(std::string& text)
	{
		for (const char* marker : { "\\users\\", "\\home\\" }) {
			const std::string m = marker;
			for (std::size_t at = FindFolded(text, m, 0); at != std::string::npos; at = FindFolded(text, m, at + 1)) {
				const std::size_t start = at + m.size();
				std::size_t end = start;
				while (end < text.size() && text[end] != '\\' && text[end] != '/' && text[end] != ' ' && text[end] != ']' && text[end] != '\'' &&
					   text[end] != '"')
					++end;
				if (end == start || end >= text.size() || (text[end] != '\\' && text[end] != '/')) continue;
				const std::string name = text.substr(start, end - start);
				if (name == "<user>" || name == "Public" || name == "public") continue;
				text.replace(start, end - start, "<user>");
			}
		}
	}

	std::string Redact(std::string text)
	{
		for (const auto& r : g_replacements)
			for (std::size_t at = FindFolded(text, r.from, 0); at != std::string::npos; at = FindFolded(text, r.from, at + r.to.size()))
				text.replace(at, r.from.size(), r.to);
		RedactUserSegments(text);
		return text;
	}

	class RedactSink final : public spdlog::sinks::sink
	{
	public:
		explicit RedactSink(spdlog::sink_ptr inner) : inner_(std::move(inner)) {}

		void log(const spdlog::details::log_msg& msg) override
		{
			const std::string clean = Redact(std::string(msg.payload.data(), msg.payload.size()));
			spdlog::details::log_msg copy = msg;
			copy.payload = spdlog::string_view_t(clean.data(), clean.size());
			inner_->log(copy);
		}
		void flush() override { inner_->flush(); }
		void set_pattern(const std::string& pattern) override { inner_->set_pattern(pattern); }
		void set_formatter(std::unique_ptr<spdlog::formatter> formatter) override { inner_->set_formatter(std::move(formatter)); }

	private:
		spdlog::sink_ptr inner_;
	};

	std::string Directory(std::string path)
	{
		if (const auto slash = path.find_last_of("\\/"); slash != std::string::npos) path.resize(slash);
		return path;
	}

	std::string Narrow(const wchar_t* text)
	{
		if (!text || !*text) return {};
		const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (n <= 1) return {};
		std::string out(static_cast<std::size_t>(n - 1), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
		return out;
	}

	void Add(std::string from, const char* to)
	{
		while (!from.empty() && (from.back() == '\\' || from.back() == '/')) from.pop_back();
		if (from.size() < 4) return;  // never a bare drive root
		g_replacements.push_back({ std::move(from), to });
	}
}

namespace f4sr::log_redact
{
	void Install()
	{
		char exe[MAX_PATH]{};
		const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
		Add(Directory(std::string(exe, n)), "<game>");
		char profile[MAX_PATH]{};
		if (const DWORD p = GetEnvironmentVariableA("USERPROFILE", profile, MAX_PATH); p && p < MAX_PATH) Add(profile, "<profile>");
		PWSTR documents{};
		if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents) {
			Add(Narrow(documents), "<documents>");
			CoTaskMemFree(documents);
		}
		std::sort(g_replacements.begin(), g_replacements.end(),
			[](const Replacement& a, const Replacement& b) { return a.from.size() > b.from.size(); });

		auto logger = spdlog::default_logger();
		if (!logger) return;
		for (auto& sink : logger->sinks())
			if (!std::dynamic_pointer_cast<RedactSink>(sink)) sink = std::make_shared<RedactSink>(sink);
	}
}
