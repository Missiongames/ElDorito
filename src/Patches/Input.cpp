#include "Input.hpp"
#include <stack>

#include "../Patch.hpp"
#include "../Blam/BlamInput.hpp"
#include "../Modules/ModuleInput.hpp"

namespace
{
	using namespace Patches::Input;

	void UpdateInputHook();
	void ProcessUiInputHook();
	void QuickUpdateUiInputHook();
	void KeyTestHook();
	void InitBindingsHook(Blam::Input::BindingsTable *bindings);
	Blam::Input::BindingsPreferences* PreferencesGetKeyBindingsHook(Blam::Input::BindingsPreferences *result);
	void PreferencesSetKeyBindingsHook(Blam::Input::BindingsPreferences newBindings);
	void GetDefaultBindingsHook(int type, Blam::Input::BindingsTable *result);

	std::stack<std::shared_ptr<InputContext>> contextStack;
	std::vector<DefaultInputHandler> defaultHandlers;
	bool contextDone = false;
}

namespace Patches
{
	namespace Input
	{
		void ApplyAll()
		{
			Hook(0x1129A0, UpdateInputHook).Apply();
			Hook(0x105CBA, ProcessUiInputHook, HookFlags::IsCall).Apply();
			Hook(0x106417, QuickUpdateUiInputHook, HookFlags::IsCall).Apply();
			Hook(0x111B66, KeyTestHook, HookFlags::IsCall).Apply();
			Hook(0x111CE6, KeyTestHook, HookFlags::IsCall).Apply();
			Hook(0x20BF00, InitBindingsHook).Apply();
			Hook(0x10AB40, PreferencesGetKeyBindingsHook).Apply();
			Hook(0x10D040, PreferencesSetKeyBindingsHook).Apply();
			Hook(0x20C040, GetDefaultBindingsHook).Apply();
			Patch::NopFill(Pointer::Base(0x6A225B), 2); // Prevent the game from forcing certain binds on load
		}

		void PushContext(std::shared_ptr<InputContext> context)
		{
			if (!contextStack.empty())
				contextStack.top()->InputDeactivated();
			contextStack.push(context);
			context->InputActivated();
		}

		void RegisterDefaultInputHandler(DefaultInputHandler func)
		{
			defaultHandlers.push_back(func);
		}
	}
}

namespace
{
	void PopContext()
	{
		contextStack.top()->InputDeactivated();
		contextStack.pop();
		if (!contextStack.empty())
			contextStack.top()->InputActivated();
	}

	void UpdateInputHook()
	{
		// If the current context is done, pop it off
		if (contextDone)
		{
			PopContext();
			contextDone = false;
		}

		if (!contextStack.empty())
		{
			// Tick the active context
			if (!contextStack.top()->GameInputTick())
				contextDone = true;
		}
		else
		{
			// Run default handlers
			for (auto &&handler : defaultHandlers)
				handler();
		}
	}

	void UiInputTick()
	{
		// Tick the active context
		if (!contextStack.empty() && !contextStack.top()->UiInputTick())
			contextDone = true;
	}

	void ProcessUiInputHook()
	{
		// Pump Windows messages (replaced function)
		typedef void(*PumpMessagesPtr)();
		auto PumpMessages = reinterpret_cast<PumpMessagesPtr>(0x508170);
		PumpMessages();

		UiInputTick();
	}

	void QuickUpdateUiInputHook()
	{
		// Quick pump Windows messages (replaced function)
		typedef void(*QuickPumpMessagesPtr)();
		auto QuickPumpMessages = reinterpret_cast<QuickPumpMessagesPtr>(0x42E940);
		QuickPumpMessages();

		UiInputTick();
	}

	// Returns true if a key should be blocked.
	bool KeyTestHookImpl(Blam::Input::InputType type)
	{
		return (type == Blam::Input::eInputTypeGame || type == Blam::Input::eInputTypeSpecial) && !contextStack.empty();
	}

	// Hook for the engine's "get key ticks" and "get key ms" functions. If the
	// zero flag is not set when this returns, the key will be blocked.
	__declspec(naked) void KeyTestHook()
	{
		__asm
		{
			// Replaced code (checks if the input type is blocked)
			cmp byte ptr ds:0x238DBEB[eax], 0
			jnz done

			push eax
			call KeyTestHookImpl
			add esp, 4
			test eax, eax

		done:
			// The replaced instruction is 7 bytes long, so get the return
			// address and add 2 without affecting the flags
			pop eax
			lea eax, [eax + 2]
			jmp eax
		}
	}

	// Hook to initialize bindings with ModuleInput's values
	void InitBindingsHook(Blam::Input::BindingsTable *bindings)
	{
		*bindings = *Modules::ModuleInput::GetBindings();
	}

	// Hook to redirect keybind preference reads to ModuleInput
	Blam::Input::BindingsPreferences* PreferencesGetKeyBindingsHook(Blam::Input::BindingsPreferences *result)
	{
		auto bindings = Modules::ModuleInput::GetBindings();
		memcpy(result->PrimaryKeys, bindings->PrimaryKeys, sizeof(result->PrimaryKeys));
		memcpy(result->PrimaryMouseButtons, bindings->PrimaryMouseButtons, sizeof(result->PrimaryMouseButtons));
		memcpy(result->SecondaryKeys, bindings->SecondaryKeys, sizeof(result->SecondaryKeys));
		memcpy(result->SecondaryMouseButtons, bindings->SecondaryMouseButtons, sizeof(result->SecondaryMouseButtons));
		return result;
	}

	// Hook to redirect keybind preference writes to ModuleInput
	void PreferencesSetKeyBindingsHook(Blam::Input::BindingsPreferences newBindings)
	{
		auto bindings = Modules::ModuleInput::GetBindings();
		memcpy(bindings->PrimaryKeys, newBindings.PrimaryKeys, sizeof(bindings->PrimaryKeys));
		memcpy(bindings->PrimaryMouseButtons, newBindings.PrimaryMouseButtons, sizeof(bindings->PrimaryMouseButtons));
		memcpy(bindings->SecondaryKeys, newBindings.SecondaryKeys, sizeof(bindings->SecondaryKeys));
		memcpy(bindings->SecondaryMouseButtons, newBindings.SecondaryMouseButtons, sizeof(bindings->SecondaryMouseButtons));
		Blam::Input::SetBindings(0, *bindings);
		Modules::CommandMap::Instance().ExecuteCommand("WriteConfig");
	}

	// Hook to prevent the game from resetting keybindings when we don't want it to
	void GetDefaultBindingsHook(int type, Blam::Input::BindingsTable *result)
	{
		*result = *Modules::ModuleInput::GetBindings();
	}
}