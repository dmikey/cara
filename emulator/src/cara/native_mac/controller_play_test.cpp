// Copyright 2026 Cara contributors. GPL-2.0-or-later.
// Explicit authored-bootstrap fixture: actual input factories and live HID updates.
#include <SDL.h>
#include <array>
#include <chrono>
#include <stdexcept>
#include "cara/native_mac/controller_play_test.h"
#include "cara/native_mac/mac_preferences.h"
#include "core/core.h"
#include "core/hle/service/hid/hid.h"
#include "core/settings.h"
#include "input_common/main.h"
#include "input_common/keyboard.h"
namespace Cara::ControllerPlayTest {
unsigned Run(Core::System& system) {
    unsigned checks=0;
    const auto require=[&](bool condition,const char* message){if(!condition)throw std::runtime_error(message);++checks;};
    auto hid=Service::HID::GetModule(system);require(bool(hid),"Live HID module missing");
    std::array<ARM_Interface*,4> cpus{};for(unsigned i=0;i<4;++i)cpus[i]=&system.GetCore(i);
    const bool cpu_jit=Settings::values.use_cpu_jit;
    std::array<double,4> clocks{};for(unsigned i=0;i<4;++i)clocks[i]=system.CoreTiming().GetTimer(i)->GetClockScaleForDiagnostics();
    const auto sink=Settings::values.sink_id;
    const auto original=Settings::values.current_input_profile;
    const int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,2,2,1);
    require(index>=0,"Virtual controller attach failed");
    auto* joystick=SDL_JoystickOpen(index);require(joystick!=nullptr,"Virtual controller open failed");
    struct Cleanup { SDL_Joystick* joystick;int index;~Cleanup(){SDL_JoystickClose(joystick);SDL_JoystickDetachVirtual(index);SDL_PumpEvents();}} cleanup{joystick,index};
    const auto pump=[] {SDL_JoystickUpdate();SDL_PumpEvents();};
    const auto capture=[&](int button) {
        auto pollers=InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Button);
        for(auto& p:pollers)p->Start();
        SDL_JoystickSetVirtualButton(joystick,button,1);pump();
        SDL_JoystickSetVirtualButton(joystick,button,0);pump();
        Common::ParamPackage result;for(auto& p:pollers){auto candidate=p->GetNextInput();if(candidate.Has("engine"))result=candidate;p->Stop();}
        require(result.Get("engine","")=="sdl","Actual Settings poller did not capture controller");return result.Serialize();
    };
    auto first=capture(0);
    auto analog_pollers=InputCommon::Polling::GetPollers(InputCommon::Polling::DeviceType::Analog);
    for(auto& p:analog_pollers)p->Start();
    SDL_JoystickSetVirtualAxis(joystick,0,26000);pump();SDL_JoystickSetVirtualAxis(joystick,1,26000);pump();
    Common::ParamPackage analog;for(auto& p:analog_pollers){auto value=p->GetNextInput();if(value.Has("engine"))analog=value;p->Stop();}
    require(analog.Get("engine","")=="sdl","Settings stick poller did not capture controller");
    MacPreferences::Model model;model.backend=cpu_jit?"fastinterp":"jit";
    model.buttons[0].value=first;model.analogs[0].use_controller=true;model.analogs[0].controller=analog.Serialize();
    // Match the same Save path: serialize and reconstruct bindings, then reload only input.
    MacPreferences::Model saved(model.Serialize(true));
    Settings::ApplyInputProfile(saved.Controls(original));
    const auto await=[&](auto predicate) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        for(unsigned i=0;i<20000 && std::chrono::steady_clock::now()<deadline;++i){
            require(system.RunLoop()==Core::System::ResultStatus::Success,"Authored bootstrap RunLoop failed");
            if(predicate(hid->GetState())){++checks;return;}
        }
        throw std::runtime_error("Controller change did not reach live guest HID");
    };
    const auto settle_reload=[&] {
        const auto origin=system.CoreTiming().GetTicks();
        await([&](const auto&){return system.CoreTiming().GetTicks()>origin+4000000;});
    };
    settle_reload();
    SDL_JoystickSetVirtualAxis(joystick,0,0);SDL_JoystickSetVirtualAxis(joystick,1,0);pump();
    SDL_JoystickSetVirtualAxis(joystick,0,26000);SDL_JoystickSetVirtualAxis(joystick,1,26000);
    SDL_JoystickSetVirtualButton(joystick,0,1);pump();
    await([](const auto& pad){return pad.a && pad.circle_right && pad.circle_down;});
    SDL_JoystickSetVirtualButton(joystick,0,0);SDL_JoystickSetVirtualAxis(joystick,0,0);SDL_JoystickSetVirtualAxis(joystick,1,0);pump();
    await([](const auto& pad){return !pad.a && !pad.circle_right && !pad.circle_down;});
    // Rebind A during the SAME running core; no ROM reopen or CPU replacement.
    model.buttons[0].value=capture(1);Settings::ApplyInputProfile(model.Controls(original));settle_reload();
    SDL_JoystickSetVirtualButton(joystick,1,1);pump();await([](const auto& pad){return bool(pad.a);});
    SDL_JoystickSetVirtualButton(joystick,1,0);pump();await([](const auto& pad){return !pad.a;});
    const auto before=system.CoreTiming().GetTicks();SDL_JoystickSetVirtualButton(joystick,0,1);pump();
    await([&](const auto& pad){return system.CoreTiming().GetTicks()>before+4000000 && !pad.a;});
    SDL_JoystickSetVirtualButton(joystick,0,0);pump();
    // Factory arrow defaults must also reach the actual live HID service.
    MacPreferences::Model defaults;Settings::ApplyInputProfile(defaults.Controls(original));settle_reload();
    InputCommon::GetKeyboard()->PressKey(SDL_SCANCODE_UP);
    await([](const auto& pad){return pad.up && pad.circle_up;});
    InputCommon::GetKeyboard()->ReleaseKey(SDL_SCANCODE_UP);
    await([](const auto& pad){return !pad.up && !pad.circle_up;});
    require(Settings::values.use_cpu_jit==cpu_jit,"Control Save changed the CPU backend setting");
    require(Settings::values.sink_id==sink,"Control Save changed sound");
    for(unsigned i=0;i<4;++i){require(&system.GetCore(i)==cpus[i],"Control Save replaced a running CPU");require(system.CoreTiming().GetTimer(i)->GetClockScaleForDiagnostics()==clocks[i] && clocks[i]==1.0,"A guest timer changed from 100 percent");}
    Settings::ApplyInputProfile(original);
    return checks;
}
}
