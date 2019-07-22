// MonoJabber.cpp : This file contains the 'main' function. Program execution begins and ends there.
//		-Args List-
// arg 1: Target Application Name
// arg 2: Payload DLL Path
// arg 3: Payload Namespace
// arg 4: Payload Classname
// arg 5: Payload Methodname

#include "pch.h"
#include "stdafx.h"
#include "MonoJabber.h"
#include "mProcess.h"
#include "mMemory.h"
#include <iostream>
#include <psapi.h>
#include <sysinfoapi.h>
#include <sys/stat.h>
#include <Tlhelp32.h>
#include <windows.h>


void EndApplication() {
	std::getchar();
	exit(0);
}

bool DoesDLLExist(const char **DLL_PATH) {
	struct stat st;
	return stat(*DLL_PATH, &st) == 0;
}

bool DoesTargetUseMono(HANDLE &TARGET_HANDLE) {
	return mProcessFunctions::mGetModuleAddress(TARGET_HANDLE, "mono-2.0-bdwgc.dll") != NULL;
}

uintptr_t GetMonoLoaderFuncAddress(const std::string &MONO_LOADER_DLL_PATH, const HANDLE &INJECTEE_HANDLE) {
	HMODULE loaderModule = LoadLibraryEx(MONO_LOADER_DLL_PATH.c_str(), NULL, DONT_RESOLVE_DLL_REFERENCES);
	uintptr_t funcOffset = mProcessFunctions::mGetExportedFunctionOffset(loaderModule, "Inject");
	uintptr_t injectedLoaderBase = mProcessFunctions::mGetModuleAddress(INJECTEE_HANDLE, "MonoLoaderDLL.dll");
	uintptr_t funcAddress = injectedLoaderBase + funcOffset;
	FreeLibrary(loaderModule);
	return funcAddress;
}

std::string GetMonoLoaderDLLPath() {
	char buffer[MAX_PATH];
	GetModuleFileName(NULL, buffer, MAX_PATH);
	std::string::size_type positionToTrunc = std::string(buffer).find_last_of("\\/");
	std::string currentDirectory = std::string(buffer).substr(0, positionToTrunc);
	std::string pathToLoaderDLL = currentDirectory + "\\MonoLoaderDLL.dll";
	const char *pathArg = pathToLoaderDLL.c_str(); // Really don't like this.

	if (!DoesDLLExist(&pathArg)) {
		return "";
	}

	return pathToLoaderDLL;
}

LoaderArguments CreateArgsStruct(char* program_args[]) {
	LoaderArguments loaderArgs;
	strcpy_s(loaderArgs.DLL_PATH, program_args[2]);
	strcpy_s(loaderArgs.LOADER_NAMESPACE, program_args[3]);
	strcpy_s(loaderArgs.LOADER_CLASSNAME, program_args[4]);
	strcpy_s(loaderArgs.LOADER_METHODNAME, program_args[5]);
	return loaderArgs;
}

bool IsTarget64Bit(const HANDLE &TARGET_PROCESS) {
	// IsWow64Process:
	//		If we are on a 64 bit OS:
	//			it returns true if the target is 32 bit, false if it is 64 bit
	//		If we are on a 32 bit OS:
	//			it returns false if the target is 32 bit
	// So determine the bitness of the operating system, and return based off of that.
	// NOTE: This function ignores cases for when wProcessorArchitecture == UNKNOWN.
	SYSTEM_INFO sysInfo;
	GetNativeSystemInfo(&sysInfo);
	BOOL is64Bit = FALSE;

	if (sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 || 
		sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_IA64 ||
		sysInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
		IsWow64Process(TARGET_PROCESS, &is64Bit);
		return !is64Bit ? true : false; // If it is false, it is 64 bit. otherwise, its 32 bit
	}
	else {
		return false;
	}
}

int main(int argc, char* argv[]) {
	printf("	-=MonoJabber=-\n");

	if (argc != 6) {
		printf(
			"Error: Application requires six arguments: {TargetProcess} {Path to Payload DLL} "
			"{PayloadNamespace} {PayloadClassname} {PayloadMethodname}\n"
		);
		EndApplication();
	}
	
	const char *targetProcess = argv[1];
	const char *dllPath = argv[2];
	std::string monoLoaderDLLPath = GetMonoLoaderDLLPath();
	LoaderArguments lArgs = CreateArgsStruct(argv);

	int injecteePID = mProcessFunctions::mGetPID(targetProcess);
	if (injecteePID == NULL) {
		printf("Error: Failed to get target PID. Check if it's running and try again.\n" 
			"LastErrorCode: %i\n", GetLastError()
		);
		EndApplication();
	}
	printf("PID for target acquired.\n");

	if (!DoesDLLExist(&dllPath)) {
		printf("Error: The DLL at the specified location was not found.\n");
		EndApplication();
	}

	if (monoLoaderDLLPath == "") {
		printf("Error: MonoLoaderDLL.dll was not found in the current directory.\n");
		EndApplication();
	}

	HANDLE injecteeHandle = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
		| PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, injecteePID);

	bool handleIsValid = mProcessFunctions::mValidateHandle(injecteeHandle);
	if (!handleIsValid) {
		printf("Error: Failed to get a handle to the target process. Are you running as admin?"
			"LastErrorCode: %i\n", GetLastError()
		);
		EndApplication();
	}
	printf("Target handle opened.\n");

	if (!IsTarget64Bit(injecteeHandle)) {
		printf("Error: The target is a 32 bit process - This tool does not (currently) support this.\n");
		EndApplication();
	}

	// Inject MonoLoaderDLL.dll into the injectee
	if (!mMemoryFunctions::mInjectDLL(targetProcess, monoLoaderDLLPath)) {
		printf("Error: Failed to inject MonoLoaderDLL.dll into the target process. Are you running as admin?"
			"LastErrorCode: %i\n", GetLastError()
		);
		EndApplication();
	}
	printf("MonoLoader.dll injected.\n");

	// Write the parameter struct to the injectee's memory
	LPVOID addressOfParams = VirtualAllocEx(injecteeHandle, NULL, sizeof(LoaderArguments), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!WriteProcessMemory(injecteeHandle, addressOfParams, &lArgs, sizeof(LoaderArguments), 0)) {
		printf("Error: WriteProcessMemory returned false. Are you running as admin?"
			"LastErrorCode: %i\n", GetLastError());
		EndApplication();
	}
	printf("Paramater struct written to target.\n");

	// Grab MonoLoaderDLL.dll's Inject method offset, add it to the injectee's base, 
	// call it with the param struct, then close the handle.
	uintptr_t targetFunctionAddress = GetMonoLoaderFuncAddress(monoLoaderDLLPath, injecteeHandle);

	CreateRemoteThread(injecteeHandle, NULL, 0, (LPTHREAD_START_ROUTINE)(targetFunctionAddress), addressOfParams, 0, NULL);
	if (!mProcessFunctions::mValidateHandle(injecteeHandle)) {
		printf("Error: CreateRemoteThread call failed - Handle is invalid. Last error code: %i\n", GetLastError());
		return 1;
	}
	CloseHandle(injecteeHandle);

	printf("Done.\n");
}