#pragma once
#include "FnvHash.hpp"

std::unordered_map<uint32_t, const wchar_t*> hashToStrMap;

#pragma pack(push, 1)
struct YMOData
{
	uint16_t len;
	struct
	{
		uint32_t hash;
		uint16_t offset;
	} table[1];
};
#pragma pack(pop)

// External: set by the language menu, persisted in settings.
// Empty = auto-detect from system. "en" = English, "zh-CN" = Simplified Chinese.
extern std::wstring g_language;

LANGID LanguageToLangId(const std::wstring& lang)
{
	if (lang == L"zh-CN")  return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED);
	if (lang == L"zh-TW")  return MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
	return MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
}

void ClearTranslations()
{
	hashToStrMap.clear();
}

void LoadTranslateData()
{
	// Clear previous translations if reloading
	hashToStrMap.clear();

	LANGID langId;
	if (!g_language.empty())
		langId = LanguageToLangId(g_language);
	else
		langId = GetThreadUILanguage();

	auto hRes = FindResourceExW(g_hInst, L"YMO", MAKEINTRESOURCEW(1), langId);
	if (hRes)
	{
		auto hResData = LoadResource(g_hInst, hRes);
		if (hResData)
		{
			auto ymo = reinterpret_cast<const YMOData*>(LockResource(hResData));
			if (ymo)
			{
				hashToStrMap.reserve(ymo->len);

				for (int i = 0; i < ymo->len; ++i)
				{
					auto hash = ymo->table[i].hash;
					auto offset = ymo->table[i].offset;
					auto str = reinterpret_cast<const wchar_t*>(reinterpret_cast<const uint8_t*>(hResData) + offset);
					hashToStrMap.emplace(hash, str);
				}
			}
		}
	}
}

const wchar_t* Translate(const wchar_t* str)
{
	static std::unordered_map<const wchar_t*, const wchar_t*> ptrToStrMap;

	auto translation = str;

	auto i = ptrToStrMap.find(str);
	if (i == ptrToStrMap.end())
	{
		auto hash = fnv1a_32(str, wcslen(str) * sizeof(wchar_t));
		auto j = hashToStrMap.find(hash);
		if (j != hashToStrMap.end())
			translation = j->second;

		ptrToStrMap.emplace(str, translation);
	}
	else
		translation = i->second;

	return translation;
}

const wchar_t* TranslateContext(const wchar_t* str, const wchar_t* ctxtStr)
{
	auto translation = Translate(ctxtStr);
	if (translation == ctxtStr)
		return str;
	return translation;
}

#define _(str) Translate(str)
#define C_(ctxt, str) TranslateContext(str, ctxt L"\004" str)
