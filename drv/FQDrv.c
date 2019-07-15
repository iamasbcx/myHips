﻿#include <fltKernel.h>
#include <dontuse.h>
#include <suppress.h>
#include "commonStruct.h"
#include "FQDrv.h"
#include "commonFun.h"

#pragma prefast(disable:__WARNING_ENCODE_MEMBER_FUNCTION_POINTER, "Not valid for kernel mode drivers")

//
//  Structure that contains all the global data structures
//  used throughout the FQDRV.
//
FQDRV_DATA FQDRVData;
UNICODE_STRING g_LastDelFileName = { 0 };

//
//  Function prototypes
//

NTSTATUS
FQDRVPortConnect(
	__in PFLT_PORT ClientPort,
	__in_opt PVOID ServerPortCookie,
	__in_bcount_opt(SizeOfContext) PVOID ConnectionContext,
	__in ULONG SizeOfContext,
	__deref_out_opt PVOID *ConnectionCookie
);

VOID
FQDRVPortDisconnect(
	__in_opt PVOID ConnectionCookie
);

NTSTATUS
FQDRVpScanFileInUserMode(
	__in PFLT_INSTANCE Instance,
	__in PFILE_OBJECT FileObject,
	__in PFLT_CALLBACK_DATA Data,
	__in ULONG		Operation,
	__out PBOOLEAN SafeToOpen
);

//
//  Assign text sections for each routine.
//

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, FQDRVInstanceSetup)
#pragma alloc_text(PAGE, FQDRVPreCreate)
#pragma alloc_text(PAGE, FQDRVPostCreate)
#pragma alloc_text(PAGE, FQDRVPortConnect)
#pragma alloc_text(PAGE, FQDRVPortDisconnect)
#pragma alloc_text(PAGE, FQDRVPreSetInforMation)
#pragma alloc_text(PAGE, FQDRVPostSetInforMation)
#endif


//
//  Constant FLT_REGISTRATION structure for our filter.  This
//  initializes the callback routines our filter wants to register
//  for.  This is only used to register with the filter manager
//

const FLT_OPERATION_REGISTRATION Callbacks[] = {

	{ IRP_MJ_CREATE,
	  0,
	  FQDRVPreCreate,
	  FQDRVPostCreate},

	{ IRP_MJ_CLEANUP,
	  0,
	  FQDRVPreCleanup,
	  NULL},

	{ IRP_MJ_WRITE,
	  0,
	  FQDRVPreWrite,
	  NULL},

	{ IRP_MJ_SET_INFORMATION,
	  0,
	  FQDRVPreSetInforMation,
	  FQDRVPostSetInforMation},

	{ IRP_MJ_OPERATION_END}
};


const FLT_CONTEXT_REGISTRATION ContextRegistration[] = {

	{ FLT_STREAMHANDLE_CONTEXT,
	  0,
	  NULL,
	  sizeof(FQDRV_STREAM_HANDLE_CONTEXT),
	  'chBS' },

	{ FLT_CONTEXT_END }
};

const FLT_REGISTRATION FilterRegistration = {

	sizeof(FLT_REGISTRATION),         //  Size
	FLT_REGISTRATION_VERSION,           //  Version
	0,                                  //  Flags
	ContextRegistration,                //  Context Registration.
	Callbacks,                          //  Operation callbacks
	FQDRVUnload,                      //  FilterUnload
	FQDRVInstanceSetup,               //  InstanceSetup
	FQDRVQueryTeardown,               //  InstanceQueryTeardown
	NULL,                               //  InstanceTeardownStart
	NULL,                               //  InstanceTeardownComplete
	NULL,                               //  GenerateFileName
	NULL,                               //  GenerateDestinationFileName
	NULL                                //  NormalizeNameComponent
};

////////////////////////////////////////////////////////////////////////////
//
//    Filter initialization and unload routines.
//
////////////////////////////////////////////////////////////////////////////

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT DriverObject,
	__in PUNICODE_STRING RegistryPath
)
{
	OBJECT_ATTRIBUTES oa;
	UNICODE_STRING uniString;
	PSECURITY_DESCRIPTOR sd;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(RegistryPath);

	g_LastDelFileName.Buffer = ExAllocatePool(NonPagedPool, MAX_PATH * 2);
	g_LastDelFileName.Length = g_LastDelFileName.MaximumLength = MAX_PATH * 2;
	memset(g_LastDelFileName.Buffer, '\0', MAX_PATH * 2);

	//
	//  Register with filter manager.
	//

	status = FltRegisterFilter(DriverObject,
		&FilterRegistration,
		&FQDRVData.Filter);


	if (!NT_SUCCESS(status)) {

		return status;
	}

	//
	//  Create a communication port.
	//

	RtlInitUnicodeString(&uniString, FQDRVPortName);

	//
	//  We secure the port so only ADMINs & SYSTEM can acecss it.
	//

	status = FltBuildDefaultSecurityDescriptor(&sd, FLT_PORT_ALL_ACCESS);//创建安全描述符，防止端口被非管理员用户打开

	if (NT_SUCCESS(status)) {

		InitializeObjectAttributes(&oa,
			&uniString,
			OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
			NULL,
			sd);

		status = FltCreateCommunicationPort(FQDRVData.Filter,
			&FQDRVData.ServerPort,
			&oa,
			NULL,
			FQDRVPortConnect,
			FQDRVPortDisconnect,
			NULL,//作业，补充
			1);
		//
		//  Free the security descriptor in all cases. It is not needed once
		//  the call to FltCreateCommunicationPort() is made.
		//

		FltFreeSecurityDescriptor(sd);
		
		if (NT_SUCCESS(status)) {

			//
			//  Start filtering I/O.
			//

			status = FltStartFiltering(FQDRVData.Filter);

			if (NT_SUCCESS(status)) {

				return STATUS_SUCCESS;
			}

			FltCloseCommunicationPort(FQDRVData.ServerPort);
		}
	}

	FltUnregisterFilter(FQDRVData.Filter);

	return status;
}


NTSTATUS
FQDRVPortConnect(
	__in PFLT_PORT ClientPort,//应用层端口
	__in_opt PVOID ServerPortCookie,
	__in_bcount_opt(SizeOfContext) PVOID ConnectionContext,
	__in ULONG SizeOfContext,
	__deref_out_opt PVOID *ConnectionCookie
)
{
	PAGED_CODE();

	UNREFERENCED_PARAMETER(ServerPortCookie);
	UNREFERENCED_PARAMETER(ConnectionContext);
	UNREFERENCED_PARAMETER(SizeOfContext);
	UNREFERENCED_PARAMETER(ConnectionCookie);

	ASSERT(FQDRVData.ClientPort == NULL);
	ASSERT(FQDRVData.UserProcess == NULL);

	//
	//  Set the user process and port.
	//

	FQDRVData.UserProcess = PsGetCurrentProcess();//应用层EPROCESS结构
	FQDRVData.ClientPort = ClientPort;

	DbgPrint("!!! FQDRV.sys --- connected, port=0x%p\n", ClientPort);

	return STATUS_SUCCESS;
}

VOID
FQDRVPortDisconnect(
	__in_opt PVOID ConnectionCookie
)
{
	UNREFERENCED_PARAMETER(ConnectionCookie);

	PAGED_CODE();

	DbgPrint("!!! FQDRV.sys --- disconnected, port=0x%p\n", FQDRVData.ClientPort);
	FltCloseClientPort(FQDRVData.Filter, &FQDRVData.ClientPort);
	FQDRVData.UserProcess = NULL;
}

NTSTATUS
FQDRVUnload(
	__in FLT_FILTER_UNLOAD_FLAGS Flags
)
{
	UNREFERENCED_PARAMETER(Flags);

	//
	//  Close the server port.
	//
	if (g_LastDelFileName.Buffer)
		ExFreePool(g_LastDelFileName.Buffer);
	FltCloseCommunicationPort(FQDRVData.ServerPort);

	//
	//  Unregister the filter
	//

	FltUnregisterFilter(FQDRVData.Filter);

	return STATUS_SUCCESS;
}

NTSTATUS
FQDRVInstanceSetup(
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__in FLT_INSTANCE_SETUP_FLAGS Flags,
	__in DEVICE_TYPE VolumeDeviceType,
	__in FLT_FILESYSTEM_TYPE VolumeFilesystemType
)
{
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(Flags);
	UNREFERENCED_PARAMETER(VolumeFilesystemType);

	PAGED_CODE();

	ASSERT(FltObjects->Filter == FQDRVData.Filter);

	//
	//  Don't attach to network volumes.
	//

	if (VolumeDeviceType == FILE_DEVICE_NETWORK_FILE_SYSTEM) {

		return STATUS_FLT_DO_NOT_ATTACH;
	}

	return STATUS_SUCCESS;
}

NTSTATUS
FQDRVQueryTeardown(
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__in FLT_INSTANCE_QUERY_TEARDOWN_FLAGS Flags
)
{
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(Flags);

	return STATUS_SUCCESS;
}


FLT_PREOP_CALLBACK_STATUS
FQDRVPreCreate(
	__inout PFLT_CALLBACK_DATA Data,
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__deref_out_opt PVOID *CompletionContext
)
{
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);

	PAGED_CODE();

	//
	//  See if this create is being done by our user process.
	//

	if (IoThreadToProcess(Data->Thread) == FQDRVData.UserProcess) {//拿到发送创建请求的Eprocess结构

		DbgPrint("!!! FQDRV.sys -- allowing create for trusted process \n");

		return FLT_PREOP_SUCCESS_NO_CALLBACK;
	}

	return FLT_PREOP_SUCCESS_WITH_CALLBACK;
}


FLT_POSTOP_CALLBACK_STATUS
FQDRVPostCreate(
	__inout PFLT_CALLBACK_DATA Data,
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__in_opt PVOID CompletionContext,
	__in FLT_POST_OPERATION_FLAGS Flags
)
{
	//PFQDRV_STREAM_HANDLE_CONTEXT FQDRVContext;
	FLT_POSTOP_CALLBACK_STATUS returnStatus = FLT_POSTOP_FINISHED_PROCESSING;
	PFLT_FILE_NAME_INFORMATION nameInfo;
	NTSTATUS status;
	BOOLEAN safeToOpen, scanFile;
	ULONG options;
	ULONG ulDisposition;
	BOOLEAN isPopWindow = FALSE;
	FILE_DISPOSITION_INFORMATION  fdi;

	UNREFERENCED_PARAMETER(CompletionContext);
	UNREFERENCED_PARAMETER(Flags);

	//
	//  If this create was failing anyway, don't bother scanning now.
	//

	if (!NT_SUCCESS(Data->IoStatus.Status) ||//打开不成功
		(STATUS_REPARSE == Data->IoStatus.Status)) {//文件是重定向的

		return FLT_POSTOP_FINISHED_PROCESSING;
	}

	options= Data->Iopb->Parameters.Create.Options;
	//下面判断
	if (FlagOn(options, FILE_DIRECTORY_FILE) ||//是目录
		FlagOn(FltObjects->FileObject->Flags, FO_VOLUME_OPEN) || //文件对象表示卷打开请求。
		FlagOn(Data->Flags, SL_OPEN_PAGING_FILE))//打开标识
	{
		return FLT_POSTOP_FINISHED_PROCESSING;
	}
	ulDisposition = (Data->Iopb->Parameters.Create.Options >> 24) & 0xFF;
	if (ulDisposition == FILE_CREATE || ulDisposition == FILE_OVERWRITE || ulDisposition == FILE_OVERWRITE_IF)
	{
		isPopWindow = TRUE;
	}

	//
	//  Check if we are interested in this file.
	//

	status = FltGetFileNameInformation(Data,//拿到文件名字
		FLT_FILE_NAME_NORMALIZED |
		FLT_FILE_NAME_QUERY_DEFAULT,
		&nameInfo);

	if (!NT_SUCCESS(status)) {

		return FLT_POSTOP_FINISHED_PROCESSING;
	}

	FltParseFileNameInformation(nameInfo);//w文件路径解析，扩展名，路径，等

	//scanFile = FQDRVpCheckExtension(&nameInfo->Extension);//看扩展名是不是需要的


	
	//185519
	//这里先写死，后面进行链表循环判断
	//RtlInitUnicodeString(&ustrRule, L"\\*\\*\\WINDOWS\\SYSTEM32\\*\\*.*");
	//scanFile = IsPatternMatch(&ustrRule, &nameInfo->Name, TRUE);
	//RtlInitUnicodeString(&ustrRule, L"C:\Windows\\System32\\drivers\\*.*");
	WCHAR tmp[256] = { 0 };
	wcsncpy_s(tmp, nameInfo->Name.Length, nameInfo->Name.Buffer, nameInfo->Name.Length);

	//DbgPrint(" tmp路径是%S\n", tmp);

	scanFile = IsPatternMatch(L"\\*\\*\\WINDOWS\\SYSTEM32\\*\\*.*", tmp, TRUE);

	
	FltReleaseFileNameInformation(nameInfo);

	if (!scanFile) {
		return FLT_POSTOP_FINISHED_PROCESSING;
	}
	if (isPopWindow)
	{
		FQDRVpScanFileInUserMode(
			FltObjects->Instance,
			FltObjects->FileObject,
			Data,
			1,//1是创建
			&safeToOpen
		);

		if (!safeToOpen) {


			DbgPrint("拒绝创建操作\n");

			//就算拒绝了 也会创建一个空文件 这里我们删除
			fdi.DeleteFile = TRUE;
			FltSetInformationFile(FltObjects->Instance, FltObjects->FileObject, &fdi, sizeof(FILE_DISPOSITION_INFORMATION), FileDispositionInformation);
			FltCancelFileOpen(FltObjects->Instance, FltObjects->FileObject);

			Data->IoStatus.Status = STATUS_ACCESS_DENIED;
			Data->IoStatus.Information = 0;

			returnStatus = FLT_POSTOP_FINISHED_PROCESSING;

		}
	}

	
	return returnStatus;
}


FLT_PREOP_CALLBACK_STATUS
FQDRVPreCleanup(
	__inout PFLT_CALLBACK_DATA Data,
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__deref_out_opt PVOID *CompletionContext
)
{
	UNREFERENCED_PARAMETER(Data);
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);
	return FLT_PREOP_SUCCESS_NO_CALLBACK;
}

FLT_PREOP_CALLBACK_STATUS
FQDRVPreWrite(
	__inout PFLT_CALLBACK_DATA Data,
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__deref_out_opt PVOID *CompletionContext
)
{
	FLT_PREOP_CALLBACK_STATUS returnStatus = FLT_PREOP_SUCCESS_NO_CALLBACK;
	UNREFERENCED_PARAMETER(CompletionContext);
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(Data);
	//
	//  If not client port just ignore this write.
	//

	if (FQDRVData.ClientPort == NULL) {

		return FLT_PREOP_SUCCESS_NO_CALLBACK;
	}
	return returnStatus;
}

BOOLEAN isNeedWatchFile(PFLT_CALLBACK_DATA Data)
{
	BOOLEAN Ret = FALSE;
	UNICODE_STRING ustrRule = { 0 };
	PFLT_FILE_NAME_INFORMATION nameInfo = { 0 };
	NTSTATUS status = STATUS_SUCCESS;
	status = FltGetFileNameInformation(Data,
		FLT_FILE_NAME_NORMALIZED |
		FLT_FILE_NAME_QUERY_DEFAULT,
		&nameInfo);
	if (!NT_SUCCESS(status)) {
		return FALSE;
	}
	FltParseFileNameInformation(nameInfo);

	WCHAR tmp[256] = { 0 };
	wcsncpy_s(tmp, nameInfo->Name.Length, nameInfo->Name.Buffer, nameInfo->Name.Length);
	//RtlInitUnicodeString(&ustrRule, L"\\*\\*\\WINDOWS\\SYSTEM32\\*\\*.SYS");
	Ret = IsPatternMatch(L"\\*\\*\\WINDOWS\\SYSTEM32\\*\\*.*", tmp, TRUE);
	FltReleaseFileNameInformation(nameInfo);
	return Ret;
}

BOOLEAN isRecycle(PFLT_CALLBACK_DATA Data, PCFLT_RELATED_OBJECTS FltObje)
{
	BOOLEAN Ret = FALSE;
	PFLT_FILE_NAME_INFORMATION nameInfo = { 0 };
	PFILE_RENAME_INFORMATION pRenameInfo = { 0 };
	NTSTATUS status = STATUS_SUCCESS;
	char *temp = (char*)ExAllocatePool(NonPagedPool, MAX_PATH * 2);
	if (temp == NULL)
		return TRUE;
	memset(temp, '\0', MAX_PATH * 2);
	//特殊情况,当字符串中包含$Recycle.Bin时是普通删除,实际上删除只是更名而已
	pRenameInfo = (PFILE_RENAME_INFORMATION)Data->Iopb->Parameters.SetFileInformation.InfoBuffer;
	status = FltGetDestinationFileNameInformation(FltObje->Instance, Data->Iopb->TargetFileObject, pRenameInfo->RootDirectory, pRenameInfo->FileName, pRenameInfo->FileNameLength, FLT_FILE_NAME_NORMALIZED, &nameInfo);
	if (!NT_SUCCESS(status))
	{
		DbgPrint("FltGetDestinationFileNameInformation is faild! 0x%x", status);
		return TRUE;
	}
	UnicodeToChar(&nameInfo->Name, temp);
	if (strstr(temp, "Recycle.Bin"))
		Ret = TRUE;
	else
		Ret = FALSE;
	FltReleaseFileNameInformation(nameInfo);
	ExFreePool(temp);
	return Ret;
}


FLT_PREOP_CALLBACK_STATUS
FQDRVPreSetInforMation(
	__inout PFLT_CALLBACK_DATA Data,
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__deref_out_opt PVOID *CompletionContext
)
{
	FLT_PREOP_CALLBACK_STATUS status = FLT_PREOP_SUCCESS_NO_CALLBACK;
	ULONG Options = 0;//记录操作类型 1创建,2重命名,3删除
	BOOLEAN isAllow = TRUE;//是否放行
	UNREFERENCED_PARAMETER(Data);
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);
	//UNREFERENCED_PARAMETER(FltObjects);
	if (FQDRVData.ClientPort == NULL)
	{
		return FLT_PREOP_SUCCESS_NO_CALLBACK;
	}
	if (FQDRVData.UserProcess == PsGetCurrentProcess())
	{
		return FLT_PREOP_SUCCESS_NO_CALLBACK;
	}
	/*
			lpIrpStack->Parameters.SetFile.FileInformationClass == FileRenameInformation ||//重命名
			lpIrpStack->Parameters.SetFile.FileInformationClass == FileBasicInformation || //设置基础信息
			lpIrpStack->Parameters.SetFile.FileInformationClass == FileAllocationInformation ||
			lpIrpStack->Parameters.SetFile.FileInformationClass == FileEndOfFileInformation ||//设置大小
			lpIrpStack->Parameters.SetFile.FileInformationClass == FileDispositionInformation)//删除
	*/
	if (Data->Iopb->Parameters.SetFileInformation.FileInformationClass == FileRenameInformation ||
		Data->Iopb->Parameters.SetFileInformation.FileInformationClass == FileDispositionInformation)
	{
		switch (Data->Iopb->Parameters.SetFileInformation.FileInformationClass)
		{
		case FileRenameInformation:
			Options = 2;
			break;
		case FileDispositionInformation:
			Options = 3;
			break;
		default:
			Options = 0;//爆炸啦
			break;
		}
		//判断是不是我们要监控的
		if (!isNeedWatchFile(Data))
		{
			return FLT_PREOP_SUCCESS_NO_CALLBACK;
		}
		if (Options == 2)
		{
			if (isRecycle(Data, FltObjects))
			{
				return FLT_PREOP_SUCCESS_NO_CALLBACK;
			}
		}
		//进程路径,操作类型,原路径,重命名后路径
		FQDRVpScanFileInUserMode(FltObjects->Instance, FltObjects->FileObject, Data, Options, &isAllow);
		if (!isAllow)
		{
			DbgPrint("ReName in PreSetInforMation被拒绝 !\n");
			Data->IoStatus.Status = STATUS_ACCESS_DENIED;
			Data->IoStatus.Information = 0;
			status = FLT_PREOP_COMPLETE;
		}
		else
		{
			status = FLT_PREOP_SUCCESS_NO_CALLBACK;
		}
	}
	return status;
}

FLT_POSTOP_CALLBACK_STATUS
FQDRVPostSetInforMation(
	__inout PFLT_CALLBACK_DATA Data,
	__in PCFLT_RELATED_OBJECTS FltObjects,
	__in_opt PVOID CompletionContext,
	__in FLT_POST_OPERATION_FLAGS Flags
)
{
	//FLT_POSTOP_CALLBACK_STATUS status = FLT_POSTOP_FINISHED_PROCESSING;
	//ULONG Options = 0;//记录操作类型 1创建,2重命名,3删除
	//BOOLEAN isAllow = TRUE;//是否放行
	UNREFERENCED_PARAMETER(Flags);
	UNREFERENCED_PARAMETER(Data);
	UNREFERENCED_PARAMETER(FltObjects);
	UNREFERENCED_PARAMETER(CompletionContext);
	return FLT_POSTOP_FINISHED_PROCESSING;
}
//////////////////////////////////////////////////////////////////////////
//  Local support routines.
//
/////////////////////////////////////////////////////////////////////////
//操作类型 1创建 2重命名 3 删除
NTSTATUS
FQDRVpScanFileInUserMode(
	__in PFLT_INSTANCE Instance,
	__in PFILE_OBJECT FileObject,
	__in PFLT_CALLBACK_DATA Data,
	__in ULONG		Operation,
	__out PBOOLEAN SafeToOpen
)
{
	NTSTATUS status = STATUS_SUCCESS;

	PFQDRV_NOTIFICATION notification = NULL;

	ULONG replyLength = 0;
	PFLT_FILE_NAME_INFORMATION nameInfo;
	PFLT_FILE_NAME_INFORMATION pOutReNameinfo;
	PFILE_RENAME_INFORMATION pRenameInfo;

	UNREFERENCED_PARAMETER(FileObject);
	UNREFERENCED_PARAMETER(Instance);
	*SafeToOpen = TRUE;

	//
	//  If not client port just return.
	//

	if (FQDRVData.ClientPort == NULL) {

		return STATUS_SUCCESS;
	}

	try {
		notification = ExAllocatePoolWithTag(NonPagedPool,
			sizeof(FQDRV_NOTIFICATION),
			'nacS');

		if (NULL == notification)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			leave;
		}

		//在这里获取进程路径,操作类型,目标路径
		//拷贝操作类型

		notification->Operation = Operation;


		status = FltGetFileNameInformation(Data,
			FLT_FILE_NAME_NORMALIZED |
			FLT_FILE_NAME_QUERY_DEFAULT,
			&nameInfo);
		//拷贝进程路径
		//wcsncpy(notification->ProcessPath, uSProcessPath->Buffer, uSProcessPath->Length);
		if (!NT_SUCCESS(status)) {

			status = STATUS_INSUFFICIENT_RESOURCES;
			leave;
		}

		//拷贝目标路径
		FltParseFileNameInformation(nameInfo);
		wcsncpy(notification->ProcessPath, nameInfo->Name.Buffer, nameInfo->Name.Length);
		FltReleaseFileNameInformation(nameInfo);
		//wcsncpy(¬ification->ProcessPath,L"test",wcslen(L"test"));


		status = FltGetFileNameInformation(Data,
			FLT_FILE_NAME_NORMALIZED |
			FLT_FILE_NAME_QUERY_DEFAULT,
			&nameInfo);
		//FltGetDestinationFileNameInformation(

		if (!NT_SUCCESS(status)) {

			status = STATUS_INSUFFICIENT_RESOURCES;
			leave;
		}



		//拷贝目标路径
		FltParseFileNameInformation(nameInfo);

		//这里应该注意下多线程的
		if (Operation == 3)
		{
			//if (wcsncmp(g_LastDelFileName.Buffer, nameInfo->Name.Buffer, nameInfo->Name.MaximumLength) == 0)
			//{
			//	FltReleaseFileNameInformation(nameInfo);
			//	memset(g_LastDelFileName.Buffer, '\0', MAX_PATH * 2);
			//	*SafeToOpen = TRUE;
			//	leave;
			//}

		}

		if (Operation == 3)
		{
			wcsncpy(g_LastDelFileName.Buffer, nameInfo->Name.Buffer, nameInfo->Name.MaximumLength);
		}

		wcsncpy(notification->TargetPath, nameInfo->Name.Buffer, nameInfo->Name.MaximumLength);

		FltReleaseFileNameInformation(nameInfo);

		if (Operation == 2)//重命名
		{
			pRenameInfo = (PFILE_RENAME_INFORMATION)Data->Iopb->Parameters.SetFileInformation.InfoBuffer;
			status = FltGetDestinationFileNameInformation(Instance, Data->Iopb->TargetFileObject, pRenameInfo->RootDirectory, pRenameInfo->FileName, pRenameInfo->FileNameLength, FLT_FILE_NAME_NORMALIZED, &pOutReNameinfo);
			if (!NT_SUCCESS(status))
			{
				DbgPrint("FltGetDestinationFileNameInformation is faild! 0x%x", status);
				leave;
			}
			wcsncpy(notification->RePathName, pOutReNameinfo->Name.Buffer, pOutReNameinfo->Name.MaximumLength);

			DbgPrint("重命名：%wZ\n", &pOutReNameinfo->Name);

			FltReleaseFileNameInformation(pOutReNameinfo);
		}

		replyLength = sizeof(FQDRV_REPLY);

		status = FltSendMessage(FQDRVData.Filter,
			&FQDRVData.ClientPort,
			notification,
			sizeof(FQDRV_NOTIFICATION),
			notification,
			&replyLength,
			NULL);

		if (STATUS_SUCCESS == status) {

			*SafeToOpen = ((PFQDRV_REPLY)notification)->SafeToOpen;

		}
		else {

			//
			//  Couldn't send message
			//

			DbgPrint("!!! FQDRV.sys --- couldn't send message to user-mode to scan file, status 0x%X\n", status);
		}

	}
	finally{

	 if (NULL != notification) {

		 ExFreePoolWithTag(notification, 'nacS');
	 }

	}

	return status;
}

