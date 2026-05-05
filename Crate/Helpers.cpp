#include "Helpers.h"

std::string Helpers::GetNameFromFileName(const std::wstring& fileName)
{
	//making pretty name
	std::string fileNameStr(fileName.begin(), fileName.end());
	size_t lastPeriod = fileNameStr.find_last_of('.');
	fileNameStr = fileNameStr.substr(0, lastPeriod);
	size_t lastSlash = fileNameStr.find_last_of('/');
	if (lastSlash == std::string::npos) {
		lastSlash = fileNameStr.find_last_of('\\');
		if (lastSlash == std::string::npos) {
			return fileNameStr;
		}
	}

	return fileNameStr.substr(lastSlash + 1);
}
