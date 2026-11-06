// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "common/input.h"
#include "hid_core/frontend/input_converter.h"
#include "input_common/input_engine.h"
#include "input_common/input_poller.h"

namespace {

class ButtonEngine final : public InputCommon::InputEngine {
public:
    explicit ButtonEngine(std::string name) : InputEngine(std::move(name)) {}

    using InputEngine::SetAxis;
    using InputEngine::SetButton;
    using InputEngine::SetHatButton;

    u8 GetHatButtonId(const std::string& direction) const override {
        return direction == "up" ? 1 : 0;
    }
};

struct AlternateButtonFixture {
    AlternateButtonFixture() {
        Common::Input::RegisterInputFactory("alternate-test-primary", primary_factory);
        Common::Input::RegisterInputFactory("alternate-test-secondary", secondary_factory);
    }

    ~AlternateButtonFixture() {
        Common::Input::UnregisterInputFactory("alternate-test-primary");
        Common::Input::UnregisterInputFactory("alternate-test-secondary");
    }

    Common::ParamPackage Bindings() const {
        Common::ParamPackage binding{{"engine", "alternate-test-primary"}, {"button", "0"}};
        Common::ParamPackage secondary{{"engine", "alternate-test-secondary"}, {"button", "14"}};
        binding.Set("secondary", secondary.Serialize());
        return binding;
    }

    const PadIdentifier identifier{};
    std::shared_ptr<ButtonEngine> primary =
        std::make_shared<ButtonEngine>("alternate-test-primary");
    std::shared_ptr<ButtonEngine> secondary =
        std::make_shared<ButtonEngine>("alternate-test-secondary");
    std::shared_ptr<InputCommon::InputFactory> primary_factory =
        std::make_shared<InputCommon::InputFactory>(primary);
    std::shared_ptr<InputCommon::InputFactory> secondary_factory =
        std::make_shared<InputCommon::InputFactory>(secondary);
};

} // namespace

TEST_CASE("Alternate button preserves overlapping presses after serialization",
          "[input][alternate]") {
    AlternateButtonFixture fixture;
    auto binding = fixture.Bindings();
    binding.Set("toggle", true);
    binding.Set("turbo", true);
    std::vector<Common::Input::ButtonStatus> events;
    auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
    device->SetCallback({[&](const Common::Input::CallbackStatus& status) {
        events.push_back(Core::HID::TransformToButton(status));
    }});
    device->ForceUpdate();
    REQUIRE(events.size() == 1);
    CHECK_FALSE(events.back().value);
    CHECK(events.back().toggle);
    CHECK(events.back().turbo);

    fixture.primary->SetButton(fixture.identifier, 0, true);
    REQUIRE(events.size() == 2);
    CHECK(events.back().value);
    fixture.secondary->SetButton(fixture.identifier, 14, true);
    CHECK(events.size() == 2);

    SECTION("Release primary first") {
        fixture.primary->SetButton(fixture.identifier, 0, false);
        CHECK(events.size() == 2);
        fixture.secondary->SetButton(fixture.identifier, 14, false);
    }
    SECTION("Release alternate first") {
        fixture.secondary->SetButton(fixture.identifier, 14, false);
        CHECK(events.size() == 2);
        fixture.primary->SetButton(fixture.identifier, 0, false);
    }
    REQUIRE(events.size() == 3);
    CHECK_FALSE(events.back().value);

    fixture.secondary->SetButton(fixture.identifier, 14, true);
    CHECK(events.back().value);
    const auto event_count = events.size();
    device.reset();
    fixture.primary->SetButton(fixture.identifier, 0, true);
    fixture.secondary->SetButton(fixture.identifier, 14, false);
    CHECK(events.size() == event_count);
}

TEST_CASE("Alternate button keeps initial held inputs and ignores nested bindings",
          "[input][alternate]") {
    AlternateButtonFixture fixture;
    auto binding = fixture.Bindings();
    Common::ParamPackage secondary{binding.Get("secondary", "")};
    Common::ParamPackage nested{{"engine", "alternate-test-primary"}, {"button", "7"}};
    secondary.Set("secondary", nested.Serialize());
    binding.Set("secondary", secondary.Serialize());

    fixture.primary->PreSetController(fixture.identifier);
    fixture.primary->SetButton(fixture.identifier, 0, true);
    fixture.secondary->PreSetController(fixture.identifier);
    fixture.secondary->SetButton(fixture.identifier, 14, true);
    std::vector<bool> events;
    auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
    device->SetCallback({[&](const Common::Input::CallbackStatus& status) {
        events.push_back(Core::HID::TransformToButton(status).value);
    }});
    device->ForceUpdate();
    REQUIRE(events == std::vector<bool>{true});
    fixture.primary->SetButton(fixture.identifier, 0, false);
    CHECK(events.size() == 1);
    fixture.secondary->SetButton(fixture.identifier, 14, false);
    REQUIRE(events == std::vector<bool>{true, false});
    fixture.primary->SetButton(fixture.identifier, 7, true);
    CHECK(events.size() == 2);
}

TEST_CASE("Alternate buttons retain keyboard and hat primary bindings", "[input][alternate]") {
    AlternateButtonFixture fixture;
    auto binding = fixture.Bindings();
    binding.Erase("button");
    bool pressed = false;
    SECTION("Keyboard") {
        binding.Set("code", 65);
        auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
        device->SetCallback({[&](const Common::Input::CallbackStatus& status) {
            pressed = Core::HID::TransformToButton(status).value;
        }});
        fixture.primary->SetButton(fixture.identifier, 65, true);
        CHECK(pressed);
        fixture.secondary->SetButton(fixture.identifier, 14, true);
        fixture.primary->SetButton(fixture.identifier, 65, false);
        CHECK(pressed);
        fixture.secondary->SetButton(fixture.identifier, 14, false);
        CHECK_FALSE(pressed);
    }
    SECTION("Hat") {
        binding.Set("hat", 0);
        binding.Set("direction", "up");
        auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
        device->SetCallback({[&](const Common::Input::CallbackStatus& status) {
            pressed = Core::HID::TransformToButton(status).value;
        }});
        fixture.primary->SetHatButton(fixture.identifier, 0, 1);
        CHECK(pressed);
        fixture.secondary->SetButton(fixture.identifier, 14, true);
        fixture.primary->SetHatButton(fixture.identifier, 0, 0);
        CHECK(pressed);
        fixture.secondary->SetButton(fixture.identifier, 14, false);
        CHECK_FALSE(pressed);
    }
}

TEST_CASE("Alternate button preserves analog pressure thresholds and command flags",
          "[input][alternate]") {
    AlternateButtonFixture fixture;
    auto binding = fixture.Bindings();
    binding.Erase("button");
    binding.Set("axis", 2);
    binding.Set("threshold", 0.75f);
    binding.Set("toggle", true);
    binding.Set("turbo", true);
    fixture.primary->PreSetController(fixture.identifier);
    fixture.primary->SetAxis(fixture.identifier, 2, 0.4f);
    Common::Input::CallbackStatus latest;
    auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
    device->SetCallback({[&](const Common::Input::CallbackStatus& status) { latest = status; }});
    device->ForceUpdate();
    CHECK(Core::HID::TransformToTrigger(latest).analog.value == Catch::Approx(0.4f));
    CHECK_FALSE(Core::HID::TransformToButton(latest).value);
    CHECK(Core::HID::TransformToButton(latest).toggle);
    CHECK(Core::HID::TransformToButton(latest).turbo);

    fixture.secondary->SetButton(fixture.identifier, 14, true);
    CHECK(Core::HID::TransformToTrigger(latest).analog.value == Catch::Approx(1.0f));
    CHECK(Core::HID::TransformToButton(latest).value);
    fixture.primary->SetAxis(fixture.identifier, 2, 0.9f);
    fixture.secondary->SetButton(fixture.identifier, 14, false);
    CHECK(Core::HID::TransformToTrigger(latest).analog.value == Catch::Approx(0.9f));
    CHECK(Core::HID::TransformToButton(latest).value);
    fixture.primary->SetAxis(fixture.identifier, 2, 0.6f);
    CHECK(Core::HID::TransformToTrigger(latest).analog.value == Catch::Approx(0.6f));
    CHECK_FALSE(Core::HID::TransformToButton(latest).value);
}

TEST_CASE("Alternate button normalizes inversion once and rejects non-button alternates",
          "[input][alternate]") {
    AlternateButtonFixture fixture;
    auto binding = fixture.Bindings();
    bool pressed = false;
    SECTION("Inverted primary") {
        binding.Set("inverted", true);
        auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
        device->SetCallback({[&](const Common::Input::CallbackStatus& status) {
            pressed = Core::HID::TransformToButton(status).value;
        }});
        device->ForceUpdate();
        CHECK(pressed);
        fixture.primary->SetButton(fixture.identifier, 0, true);
        CHECK_FALSE(pressed);
        fixture.secondary->SetButton(fixture.identifier, 14, true);
        CHECK(pressed);
        fixture.secondary->SetButton(fixture.identifier, 14, false);
        CHECK_FALSE(pressed);
    }
    SECTION("Malformed alternate falls back to primary") {
        Common::ParamPackage secondary{{"engine", "alternate-test-secondary"}, {"motion", "0"}};
        binding.Set("secondary", secondary.Serialize());
        auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
        device->SetCallback({[&](const Common::Input::CallbackStatus& status) {
            pressed = Core::HID::TransformToButton(status).value;
        }});
        fixture.primary->SetButton(fixture.identifier, 0, true);
        CHECK(pressed);
        fixture.primary->SetButton(fixture.identifier, 0, false);
        CHECK_FALSE(pressed);
    }
}

TEST_CASE("Alternate button preserves separate analog and digital trigger states",
          "[input][alternate]") {
    AlternateButtonFixture fixture;
    auto binding = fixture.Bindings();
    binding.Set("axis", 2);
    binding.Set("inverted", true);
    binding.Set("toggle", true);
    binding.Set("turbo", true);
    fixture.primary->PreSetController(fixture.identifier);
    fixture.primary->SetAxis(fixture.identifier, 2, 0.3f);
    fixture.primary->SetButton(fixture.identifier, 0, true);
    Common::Input::CallbackStatus latest;
    auto device = Common::Input::CreateInputDeviceFromString(binding.Serialize());
    device->SetCallback({[&](const Common::Input::CallbackStatus& status) { latest = status; }});
    device->ForceUpdate();
    CHECK_FALSE(Core::HID::TransformToButton(latest).value);
    CHECK(Core::HID::TransformToTrigger(latest).analog.value == Catch::Approx(0.3f));
    CHECK(Core::HID::TransformToButton(latest).toggle);
    CHECK(Core::HID::TransformToButton(latest).turbo);
    fixture.primary->SetButton(fixture.identifier, 0, false);
    CHECK(Core::HID::TransformToButton(latest).value);
    fixture.secondary->SetButton(fixture.identifier, 14, true);
    CHECK(Core::HID::TransformToTrigger(latest).analog.value == Catch::Approx(1.0f));
    fixture.primary->SetButton(fixture.identifier, 0, true);
    CHECK(Core::HID::TransformToButton(latest).value);
    fixture.secondary->SetButton(fixture.identifier, 14, false);
    CHECK_FALSE(Core::HID::TransformToButton(latest).value);
    CHECK(Core::HID::TransformToTrigger(latest).analog.value == Catch::Approx(0.3f));
}
