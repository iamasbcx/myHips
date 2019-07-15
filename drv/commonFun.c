
#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>
#include <strsafe.h>
#include "FQDrv.h"
#include "commonFun.h"

#define  MAXPATHLEN         512    
//UNICODE_STRINGz ת��Ϊ CHAR*
//���� UNICODE_STRING ��ָ�룬���խ�ַ�����BUFFER ��Ҫ�Ѿ�����ÿռ�
VOID UnicodeToChar(PUNICODE_STRING dst, char *src)
{
	ANSI_STRING string;
	RtlUnicodeStringToAnsiString(&string, dst, TRUE);
	strcpy(src, string.Buffer);
	RtlFreeAnsiString(&string);
}

//WCHAR*ת��Ϊ CHAR*
//������ַ����׵�ַ�����խ�ַ�����BUFFER ��Ҫ�Ѿ�����ÿռ�
VOID WcharToChar(PWCHAR src, PCHAR dst)
{
	UNICODE_STRING uString;
	ANSI_STRING aString;
	RtlInitUnicodeString(&uString, src);
	RtlUnicodeStringToAnsiString(&aString, &uString, TRUE);
	strcpy(dst, aString.Buffer);
	RtlFreeAnsiString(&aString);
}

//CHAR*ת WCHAR*
//����խ�ַ����׵�ַ��������ַ�����BUFFER ��Ҫ�Ѿ�����ÿռ�
VOID CharToWchar(PCHAR src, PWCHAR dst)
{
	UNICODE_STRING uString;
	ANSI_STRING aString;
	RtlInitAnsiString(&aString, src);
	RtlAnsiStringToUnicodeString(&uString, &aString, TRUE);
	wcscpy(dst, uString.Buffer);
	RtlFreeUnicodeString(&uString);
}



wchar_t *my_wcsrstr(const wchar_t *str, const wchar_t *sub_str)
{
	const wchar_t	*p = NULL;
	const wchar_t	*q = NULL;
	wchar_t			*last = NULL;

	if (NULL == str || NULL == sub_str)
	{
		return NULL;
	}

	for (; *str; str++)
	{
		p = str, q = sub_str;
		while (*q && *p)
		{
			if (*q != *p)
			{
				break;
			}
			p++, q++;
		}
		if (*q == 0)
		{
			last = (wchar_t *)str;
			str = p - 1;
		}
	}
	return last;
}


BOOLEAN IsPatternMatch(const PWCHAR pExpression, const PWCHAR pName, BOOLEAN IgnoreCase)
{
	if (NULL == pExpression || wcslen(pExpression) == 0)
	{
		return TRUE;
	}
	if (pName == NULL || wcslen(pName) == 0)
	{
		return FALSE;
	}
	if (L'$' == pExpression[0])
	{
		UNICODE_STRING			uNewExpression = { 0 };
		WCHAR					buffer[MAXPATHLEN] = { 0 };
		UNICODE_STRING			uExpression = { 0 };
		UNICODE_STRING			uName = { 0 };

		RtlInitUnicodeString(&uName, pName);
		RtlInitUnicodeString(&uExpression, &pExpression[1]);

		if (FsRtlIsNameInExpression(&uExpression, &uName, IgnoreCase, NULL))
		{
			WCHAR* last = my_wcsrstr(pExpression, L"\\*");
			if (NULL == last)
			{
				return FALSE;
			}
			StringCbCopyNW(buffer, MAXPATHLEN * sizeof(WCHAR), &pExpression[1], (last - pExpression + 1) * sizeof(WCHAR));
			StringCbCatW(buffer, MAXPATHLEN * sizeof(WCHAR), last);
			RtlInitUnicodeString(&uNewExpression, buffer);
			if (FsRtlIsNameInExpression(&uNewExpression, &uName, IgnoreCase, NULL))
			{
				return FALSE;
			}
			else
			{
				return TRUE;
			}
		}
		else
		{
			return FALSE;
		}

	}
	else
	{
		UNICODE_STRING			uExpression = { 0 };
		UNICODE_STRING			uName = { 0 };

		RtlInitUnicodeString(&uExpression, pExpression);
		RtlInitUnicodeString(&uName, pName);

		return FsRtlIsNameInExpression(&uExpression, &uName, IgnoreCase, NULL);
	}
}