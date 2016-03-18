#include "ElDorito.hpp"

#include "Utils/Utils.hpp"
#include "ElPatches.hpp"
#include "Patches/Network.hpp"
#include "ThirdParty/WebSockets.hpp"
#include "Server/ServerChat.hpp"
#include "Server/VariableSynchronization.hpp"
#include "Server/BanList.hpp"
#include "Patches/Core.hpp"
#include "Console.hpp"
#include "Web/Ui/ScreenLayer.hpp"
#include "Web/Ui/WebConsole.hpp"
#include "Web/Ui/WebLoadingScreen.hpp"
#include "Web/Ui/MpEventDispatcher.hpp"
#include "ElModules.hpp"
#include "Modules/ModuleGame.hpp"
#include "Patch.hpp"
#include "Modules/ModuleCamera.hpp"

#include <Windows.h>
#include <TlHelp32.h>
#include <codecvt>

size_t ElDorito::MainThreadID = 0;

ElDorito::ElDorito()
{
}

//bool(__cdecl * ElDorito::Video_InitD3D)(bool, bool) = (bool(__cdecl *) (bool, bool)) 0xA21B40;
//
//bool __cdecl ElDorito::hooked_Video_InitD3D(bool windowless, bool nullRefDevice) {
//	// TEMP: leave window enabled for now so async networkWndProc stuff still works
//	return (*Video_InitD3D)(false, true);
//}

void ElDorito::killProcessByName(const char *filename, int ourProcessID)
{
	HANDLE hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, NULL);
	PROCESSENTRY32 pEntry;
	pEntry.dwSize = sizeof(pEntry);
	BOOL hRes = Process32First(hSnapShot, &pEntry);
	while (hRes)
	{
		if (strcmp(pEntry.szExeFile, filename) == 0)
		{
			if (pEntry.th32ProcessID != ourProcessID)
			{
				HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, 0, (DWORD)pEntry.th32ProcessID);

				if (hProcess != NULL)
				{
					TerminateProcess(hProcess, 9);
					CloseHandle(hProcess);
				}
			}
		}
		hRes = Process32Next(hSnapShot, &pEntry);
	}
	CloseHandle(hSnapShot);
}

void ElDorito::Initialize()
{
	::CreateDirectoryA(GetDirectory().c_str(), NULL);

	// init our command modules
	Console::Init();
	Modules::ElModules::Instance();

	// load variables/commands from cfg file
	Modules::CommandMap::Instance().ExecuteCommand("Execute dewrito_prefs.cfg");
	Modules::CommandMap::Instance().ExecuteCommand("Execute autoexec.cfg"); // also execute autoexec, which is a user-made cfg guaranteed not to be overwritten by ElDew

	//This should be removed when we can save binds
	Modules::CommandMap::Instance().ExecuteCommand("Bind CAPITAL +VoIP.Talk");

	// Parse command-line commands
	int numArgs = 0;
	LPWSTR* szArgList = CommandLineToArgvW(GetCommandLineW(), &numArgs);
	bool usingLauncher = Modules::ModuleGame::Instance().VarSkipLauncher->ValueInt == 1;
	mapsFolder = "maps\\";

	if( szArgList && numArgs > 1 )
	{
		std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

		for( int i = 1; i < numArgs; i++ )
		{
			std::wstring arg = std::wstring(szArgList[i]);
			if( arg.compare(0, 1, L"-") != 0 ) // if it doesn't start with -
				continue;

#ifndef _DEBUG
			if (arg.compare(L"-launcher") == 0)
				usingLauncher = true;
#endif

			if (arg.compare(L"-dedicated") == 0)
			{
				isDedicated = true;
				usingLauncher = true;
			}

			if (arg.compare(L"-maps") == 0 && i < numArgs - 1)
			{
				mapsFolder = Utils::String::ThinString(szArgList[i + 1]);
				if (mapsFolder.length() > 0 && mapsFolder.back() != '\\' && mapsFolder.back() != '/')
					mapsFolder += "\\";
			}

			if (arg.compare(L"-webdebug") == 0)
				webDebugging = true;

			size_t pos = arg.find(L"=");
			if( pos == std::wstring::npos || arg.length() <= pos + 1 ) // if it doesn't contain an =, or there's nothing after the =
				continue;

			std::string argname = converter.to_bytes(arg.substr(1, pos - 1));
			std::string argvalue = converter.to_bytes(arg.substr(pos + 1));

			Modules::CommandMap::Instance().ExecuteCommand(argname + " \"" + argvalue + "\"", true);
		}
	}

#if _DEBUG
	// Always enable web debugging in debug builds
	webDebugging = true;
#endif

	Patches::Core::SetMapsFolder(mapsFolder);

	if (isDedicated)
	{
		Patches::Network::ForceDedicated();

		//// Commenting this out for now because it makes testing difficult
		//DetourRestoreAfterWith();
		//DetourTransactionBegin();
		//DetourUpdateThread(GetCurrentThread());
		//DetourAttach((PVOID*)&Video_InitD3D, &hooked_Video_InitD3D);

		//if (DetourTransactionCommit() != NO_ERROR) {
		//return;
		//}

	}
	else
	{
		Web::Ui::ScreenLayer::Init();
		Web::Ui::MpEventDispatcher::Init();
		Web::Ui::WebConsole::Init();
		Web::Ui::WebLoadingScreen::Init();
	}

	// Language patch
	Patch(0x2333FD, { (uint8_t)Modules::ModuleGame::Instance().VarLanguageID->ValueInt }).Apply();

	setWatermarkText("ElDewrito | Version: " + Utils::Version::GetVersionString() + " | Build Date: " __DATE__);

#ifndef _DEBUG
	if (!usingLauncher) // force release builds to use launcher, simple check so its easy to get around if needed
	{
		MessageBox(GetConsoleWindow(), "Please run Halo Online using the ElDewrito launcher.\nIt should be named DewritoUpdater.exe.", "ElDewrito", MB_OK | MB_ICONINFORMATION);
		TerminateProcess(GetCurrentProcess(), 0);
	}
#endif

	// Ensure a ban list file exists
	Server::SaveDefaultBanList(Server::LoadDefaultBanList());

	// Initialize server modules
	Server::Chat::Initialize();
	Server::VariableSynchronization::Initialize();
	CreateThread(0, 0, StartRconWebSocketServer, 0, 0, 0);
}

void ElDorito::Tick()
{
	Server::VariableSynchronization::Tick();
	Patches::Tick();
	Web::Ui::ScreenLayer::Tick();

	// TODO: refactor this elsewhere
	Modules::ModuleCamera::Instance().UpdatePosition();

	Utils::AntiSpeedHack::OnTickCheck();

	if (executeCommandQueue)
	{
		Modules::CommandMap::Instance().ExecuteQueue();
		executeCommandQueue = false;
	}
}

namespace
{
	static void HandleFinder()
	{
	};
}

std::string ElDorito::GetDirectory()
{
	char Path[MAX_PATH];
	HMODULE hMod;
	if( !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&::HandleFinder, &hMod) )
	{
		int Error = GetLastError();
		OutputDebugString(std::string("Unable to resolve current directory, error code: ").append(std::to_string(Error)).c_str());
	}
	GetModuleFileNameA(hMod, Path, sizeof(Path));
	std::string Dir(Path);
	Dir = Dir.substr(0, std::string(Dir).find_last_of('\\') + 1);
	return Dir;
}

void ElDorito::OnMainMenuShown()
{
	if (GameHasMenuShown)
		return;
	GameHasMenuShown = true;
	executeCommandQueue = true;
}

// This is for the watermark in the bottom right corner (hidden by default)
void ElDorito::setWatermarkText(const std::string& Message)
{
	static wchar_t msgBuf[256];
	wmemset(msgBuf, 0, 256);

	std::wstring msgUnicode = Utils::String::WidenString(Message);
	wcscpy_s(msgBuf, 256, msgUnicode.c_str());

	Pointer::Base(0x2E5338).Write<uint8_t>(0x68);
	Pointer::Base(0x2E5339).Write(&msgBuf);
	Pointer::Base(0x2E533D).Write<uint8_t>(0x90);
	Pointer::Base(0x2E533E).Write<uint8_t>(0x90);
}

Pointer ElDorito::GetMainTls(size_t Offset)
{
	static Pointer ThreadLocalStorage;
	if( !ThreadLocalStorage && GetMainThreadID() )
	{
		size_t MainThreadID = GetMainThreadID();

		HANDLE MainThreadHandle = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, false, MainThreadID);

		// Get thread context
		CONTEXT MainThreadContext;
		MainThreadContext.ContextFlags = CONTEXT_FULL;

		if (MainThreadID != GetCurrentThreadId())
			SuspendThread(MainThreadHandle);

		BOOL success = GetThreadContext(MainThreadHandle, &MainThreadContext);
		if( !success )
		{
			OutputDebugString(std::string("Error getting thread context: ").append(std::to_string(GetLastError())).c_str());
			std::exit(1);
		}
		ResumeThread(MainThreadHandle);

		// Get thread selector

		LDT_ENTRY MainThreadLdt;

		success = GetThreadSelectorEntry(MainThreadHandle, MainThreadContext.SegFs, &MainThreadLdt);
		if( !success )
		{
			OutputDebugString(std::string("Error getting thread context: ").append(std::to_string(GetLastError())).c_str());
		}
		size_t TlsPtrArrayAddress = (size_t)((size_t)(MainThreadLdt.HighWord.Bits.BaseHi << 24) | (MainThreadLdt.HighWord.Bits.BaseMid << 16) | MainThreadLdt.BaseLow) + 0x2C;
		size_t TlsPtrAddress = Pointer(TlsPtrArrayAddress).Read<uint32_t>();

		// Index has been consistantly 0. Keep a look out.
		ThreadLocalStorage = Pointer(TlsPtrAddress)[0];
	}

	return ThreadLocalStorage(Offset);
}