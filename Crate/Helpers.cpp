#include "Helpers.h"

std::string Helpers::GetNameFromFileName(const std::wstring& fileName)
{
	const size_t lastPeriod = fileName.find_last_of(L'.');
	const std::wstring withoutExtension = fileName.substr(0, lastPeriod);
	const size_t lastSlash = withoutExtension.find_last_of(L"/\\");
	const std::wstring stem = lastSlash == std::wstring::npos
		? withoutExtension
		: withoutExtension.substr(lastSlash + 1);

	std::string result;
	result.reserve(stem.size());
	for (const wchar_t character : stem)
		result.push_back(static_cast<char>(character));
	return result;
}
