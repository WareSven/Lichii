// SPDX-FileCopyrightText: Copyright 2026 Lichii Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "input_common/drivers/sdl_driver.h"
#include "input_common/input_mapping.h"

namespace {

// Keep these tests from opening a physical controller used by another process.
class DisablePhysicalJoysticks {
public:
    DisablePhysicalJoysticks() {
        for (std::size_t i = 0; i < hints.size(); ++i) {
            if (const char* value = SDL_GetHint(hints[i])) {
                previous[i] = value;
            }
            SDL_SetHintWithPriority(hints[i], "0", SDL_HINT_OVERRIDE);
        }
    }

    ~DisablePhysicalJoysticks() {
        for (std::size_t i = 0; i < hints.size(); ++i) {
            SDL_ResetHint(hints[i]);
            if (previous[i]) {
                SDL_SetHint(hints[i], previous[i]->c_str());
            }
        }
    }

private:
    static constexpr std::array hints{
        SDL_HINT_JOYSTICK_DIRECTINPUT,
        SDL_HINT_JOYSTICK_GAMEINPUT,
        SDL_HINT_JOYSTICK_HIDAPI,
        SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS,
        SDL_HINT_JOYSTICK_HIDAPI_STEAM,
        SDL_HINT_JOYSTICK_HIDAPI_SWITCH,
        SDL_HINT_JOYSTICK_HIDAPI_SWITCH2,
        SDL_HINT_JOYSTICK_RAWINPUT,
        SDL_HINT_JOYSTICK_WGI,
        SDL_HINT_XINPUT_ENABLED,
    };
    std::array<std::optional<std::string>, hints.size()> previous;
};

struct VirtualGamepad {
    SDL_JoystickID id{};
    SDL_Joystick* joystick{};

    ~VirtualGamepad() {
        Disconnect();
        if (joystick) {
            SDL_CloseJoystick(joystick);
        }
    }

    void Attach(Uint16 product_id = 0x2069) {
        SDL_VirtualJoystickDesc desc{};
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.vendor_id = 0x057e;
        desc.product_id = product_id;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
        desc.name = "Lichii virtual controller test";
        id = SDL_AttachVirtualJoystick(&desc);
        REQUIRE(id != 0);
        joystick = SDL_OpenJoystick(id);
        REQUIRE(joystick != nullptr);
        REQUIRE(SDL_IsGamepad(id));
    }

    void SetMapping(const char* buttons) const {
        std::array<char, 33> guid{};
        SDL_GUIDToString(SDL_GetJoystickGUID(joystick), guid.data(), static_cast<int>(guid.size()));
        const std::string mapping =
            std::string{guid.data()} + ",Lichii virtual controller test,a:b0,b:b1," + buttons;
        REQUIRE(SDL_SetGamepadMapping(id, mapping.c_str()));
    }

    void SetButton(int button, bool pressed) const {
        REQUIRE(SDL_SetJoystickVirtualButton(joystick, button, pressed));
        SDL_UpdateJoysticks();
    }

    void Disconnect() {
        if (id != 0) {
            SDL_DetachVirtualJoystick(id);
            id = 0;
        }
    }
};

Common::ParamPackage CaptureButton(InputCommon::SDLDriver& driver,
                                   InputCommon::MappingFactory& mapping, const VirtualGamepad& pad,
                                   int button) {
    mapping.BeginMapping(InputCommon::Polling::InputType::Button);
    driver.BeginConfiguration();
    pad.SetButton(button, true);
    auto params = mapping.GetNextInput();
    driver.EndConfiguration();
    pad.SetButton(button, false);
    mapping.StopMapping();
    REQUIRE(params.Get("engine", "") == "sdl");
    REQUIRE(params.Get("button", -1) == button);
    return params;
}

PadIdentifier IdentifierFor(const Common::ParamPackage& params) {
    return {
        .guid = Common::UUID{params.Get("guid", "")},
        .port = static_cast<std::size_t>(params.Get("port", 0)),
        .pad = 0,
    };
}

} // namespace

TEST_CASE("Switch 2 rear buttons retain independent input and names after remapping",
          "[input][sdl][switch2]") {
    REQUIRE(SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD) == 0);
    DisablePhysicalJoysticks disable_physical;
    InputCommon::MappingFactory mapping;
    InputCommon::SDLDriver driver{"sdl"};
    REQUIRE(driver.GetInputDevices().empty());
    driver.SetMappingCallback(
        {[&mapping](const InputCommon::MappingData& data) { mapping.RegisterInput(data); }});

    VirtualGamepad pad;
    pad.Attach();
    pad.SetMapping("paddle1:b13,paddle2:b14,misc2:b12,");
    const auto gl = CaptureButton(driver, mapping, pad, 14);
    const auto gr = CaptureButton(driver, mapping, pad, 13);
    const auto c = CaptureButton(driver, mapping, pad, 12);
    CHECK(driver.GetUIName(gl) == Common::Input::ButtonNames::ButtonGL);
    CHECK(driver.GetUIName(gr) == Common::Input::ButtonNames::ButtonGR);
    CHECK(driver.GetUIName(c) == Common::Input::ButtonNames::ButtonC);
    const auto devices = driver.GetInputDevices();
    REQUIRE(devices.size() == 1);
    CHECK(devices.front().Get("display", "").starts_with("Nintendo Switch 2 Pro Controller"));

    const auto identifier = IdentifierFor(gl);
    pad.SetButton(14, true);
    CHECK(driver.GetButton(identifier, 14));
    CHECK_FALSE(driver.GetButton(identifier, 13));
    pad.SetButton(13, true);
    pad.SetButton(14, false);
    CHECK_FALSE(driver.GetButton(identifier, 14));
    CHECK(driver.GetButton(identifier, 13));
    pad.SetButton(13, false);
    CHECK_FALSE(driver.GetButton(identifier, 13));

    pad.SetMapping("paddle1:b21,paddle2:b20,misc2:b22,");
    const auto remapped_gl = CaptureButton(driver, mapping, pad, 20);
    const auto remapped_gr = CaptureButton(driver, mapping, pad, 21);
    const auto remapped_c = CaptureButton(driver, mapping, pad, 22);
    CHECK(driver.GetUIName(remapped_gl) == Common::Input::ButtonNames::ButtonGL);
    CHECK(driver.GetUIName(remapped_gr) == Common::Input::ButtonNames::ButtonGR);
    CHECK(driver.GetUIName(remapped_c) == Common::Input::ButtonNames::ButtonC);
    CHECK(driver.GetUIName(gl) == Common::Input::ButtonNames::Value);
    CHECK(driver.GetUIName(gr) == Common::Input::ButtonNames::Value);
    CHECK(driver.GetUIName(c) == Common::Input::ButtonNames::Value);
    const Common::ParamPackage saved_gl{remapped_gl.Serialize()};
    CHECK(driver.GetUIName(saved_gl) == Common::Input::ButtonNames::ButtonGL);

    VirtualGamepad second;
    second.Attach();
    second.SetMapping("paddle1:b20,paddle2:b21,misc2:b22,");
    const auto second_gr = CaptureButton(driver, mapping, second, 20);
    CHECK(second_gr.Get("guid", "") == saved_gl.Get("guid", ""));
    CHECK(second_gr.Get("port", -1) != saved_gl.Get("port", -1));
    CHECK(driver.GetUIName(second_gr) == Common::Input::ButtonNames::ButtonGR);
    CHECK(driver.GetUIName(saved_gl) == Common::Input::ButtonNames::ButtonGL);
    pad.SetButton(20, true);
    CHECK(driver.GetButton(IdentifierFor(saved_gl), 20));
    CHECK_FALSE(driver.GetButton(IdentifierFor(second_gr), 20));
    pad.SetButton(20, false);

    VirtualGamepad other;
    other.Attach(0x2009);
    other.SetMapping("paddle1:b21,paddle2:b20,misc2:b22,");
    const auto other_gl = CaptureButton(driver, mapping, other, 20);
    CHECK(driver.GetUIName(other_gl) == Common::Input::ButtonNames::Value);
    CHECK(driver.GetUIName(saved_gl) == Common::Input::ButtonNames::ButtonGL);

    auto invalid_port = saved_gl;
    invalid_port.Set("port", -1);
    CHECK(driver.GetUIName(invalid_port) == Common::Input::ButtonNames::Value);
    invalid_port.Set("port", 100);
    CHECK(driver.GetUIName(invalid_port) == Common::Input::ButtonNames::Value);

    pad.Disconnect();
    CHECK(driver.GetUIName(saved_gl) == Common::Input::ButtonNames::Value);
}
