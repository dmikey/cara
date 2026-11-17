// Copyright 2026 Cara contributors. GPL-2.0-or-later.
#include <cassert>
#include <iostream>
#include "cara/native_mac/mac_preferences.h"

int main() {
    using namespace Cara::MacPreferences;
    Model blank("[Controls]\nbutton_up=\nbutton_down=\nbutton_left=\nbutton_right=\n");
    for (const auto& b : blank.buttons) {
        if (b.key == "button_up") assert(b.value == Keyboard(SDL_SCANCODE_UP));
        if (b.key == "button_down") assert(b.value == Keyboard(SDL_SCANCODE_DOWN));
        if (b.key == "button_left") assert(b.value == Keyboard(SDL_SCANCODE_LEFT));
        if (b.key == "button_right") assert(b.value == Keyboard(SDL_SCANCODE_RIGHT));
    }
    const std::string original =
        "; user's configuration\n[Core]\ncpu_clock_percentage = 100\n"
        "[Renderer]\nresolution_factor = 2\n[Storage]\nnand = /my saves/nand\n"
        "[Controls]\nbutton_a = engine:keyboard,code:4\n";
    Model model(original);
    assert(model.backend == "fastinterp");
    model.buttons[0].value = Keyboard(SDL_SCANCODE_Q);
    model.analogs[0].directions[2] = Keyboard(SDL_SCANCODE_F);
    model.analogs[1].controller = Common::ParamPackage{
        {"engine", "sdl"}, {"guid", "virtual-fixture"}, {"port", "1"},
        {"axis_x", "2"}, {"axis_y", "3"}}.Serialize();
    model.analogs[1].use_controller = true;
    model.backend = "jit";
    bool rejected = false;
    try { model.Serialize(false); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    Settings::InputProfile current;
    current.name="Keep my profile";current.motion_device="motion-custom";
    current.touch_device="touch-custom";current.udp_input_address="127.0.0.2";
    auto controls=model.Controls(current);
    assert(controls.buttons[Settings::NativeButton::A]==Keyboard(SDL_SCANCODE_Q));
    assert(controls.name==current.name && controls.motion_device==current.motion_device);
    assert(controls.touch_device==current.touch_device && controls.udp_input_address==current.udp_input_address);
    const auto written = model.Serialize(true);
    Model restored(written);
    assert(restored.backend == "jit");
    assert(restored.buttons[0].value == Keyboard(SDL_SCANCODE_Q));
    assert(restored.analogs[0].directions[2] == Keyboard(SDL_SCANCODE_F));
    assert(restored.analogs[1].controller == model.analogs[1].controller);
    Ini ini(written);
    assert(ini.Get("Core", "cpu_clock_percentage") == "100");
    assert(ini.Get("Renderer", "resolution_factor") == "2");
    assert(ini.Get("Storage", "nand") == "/my saves/nand");
    assert(written.find("; user's configuration") != std::string::npos);
    assert(restored.Serialize(true) == written);
    Ini duplicate("[Core]\ncpu_backend=jit\ncpu_backend=dyncom\n[Other]\nx=y\n");
    duplicate.Set("Core", "cpu_backend", "fastinterp");
    assert(duplicate.Get("Core", "cpu_backend") == "fastinterp");
    assert(duplicate.Get("Other", "x") == "y");
    rejected = false;
    try { duplicate.Set("Core", "cpu_backend", "jit\n[Storage]"); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try { ValidateBackend("surprise", true); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    Model modifier;
    modifier.analogs[0].keyboard_package = Common::ParamPackage{
        {"engine", "analog_from_button"}, {"modifier", Keyboard(SDL_SCANCODE_LSHIFT)},
        {"modifier_scale", "0.25"}}.Serialize();
    Model modifier_saved(modifier.Serialize(true));
    Common::ParamPackage analog(modifier_saved.analogs[0].keyboard_package);
    assert(analog.Get("modifier", "") == Keyboard(SDL_SCANCODE_LSHIFT));
    assert(analog.Get("modifier_scale", "") == "0.25");
    std::cout << "Mac settings persistence, backend gates and input packages passed\n";
}
