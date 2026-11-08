#include "input/emulated/EmulatedController.h"

#include "input/api/Controller.h"


std::string_view EmulatedController::type_to_string(Type type)
{
	switch (type)
	{
	case VPAD: return "Wii U GamePad";
	case Pro: return "Wii U Pro Controller";
	case Classic: return "Wii U Classic Controller";
	case Wiimote: return "Wiimote";
	}

	throw std::runtime_error(fmt::format("unknown emulated controller: {}", stdx::to_underlying(type)));
}

EmulatedController::Type EmulatedController::type_from_string(std::string_view str)
{
	if (str == "Wii U GamePad")
		return VPAD;
	else if (str == "Wii U Pro Controller")
		return Pro;
	else if (str == "Wii U Classic Controller")
		return Classic;
	else if (str == "Wiimote")
		return Wiimote;

	throw std::runtime_error(fmt::format("unknown emulated controller: {}", str));
}

EmulatedController::EmulatedController(size_t player_index)
	: m_player_index(player_index)
{
}

void EmulatedController::calibrate()
{
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		controller->calibrate();
	}
}

void EmulatedController::connect()
{
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		controller->connect();
	}
}

void EmulatedController::update()
{
	const auto state = m_state.Read();
	for(const auto& controller : state->controllers)
	{
		controller->update();
	}
}

void EmulatedController::controllers_update_states()
{
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		controller->update_state();
	}
}

void EmulatedController::start_rumble()
{
	m_rumble = true;
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		controller->start_rumble();
	}
}

void EmulatedController::stop_rumble()
{
	if (!m_rumble)
		return;

	m_rumble = false;

	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		controller->stop_rumble();
	}
}

bool EmulatedController::is_battery_low() const
{
	const auto state = m_state.Read();
	return std::any_of(state->controllers.cbegin(), state->controllers.cend(), [](const auto& c) {return c->has_low_battery(); });
}

bool EmulatedController::has_motion() const
{
	const auto state = m_state.Read();
	return std::any_of(state->controllers.cbegin(), state->controllers.cend(), [](const auto& c) {return c->use_motion(); });
}

MotionSample EmulatedController::get_motion_data() const
{
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		if (controller->use_motion())
			return controller->get_motion_sample();
	}

	return {};
}

bool EmulatedController::has_second_motion() const
{
	int motion = 0;
	const auto state = m_state.Read();
	for(const auto& controller : state->controllers)
	{
		if(controller->use_motion())
		{
			// if wiimote has nunchuck connected, we use its acceleration
			motion++;
		}
	}

	return motion >= 2;
}

MotionSample EmulatedController::get_second_motion_data() const
{
	int motion = 0;
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		if (controller->use_motion())
		{
			// if wiimote has nunchuck connected, we use its acceleration

			motion++;
			if(motion == 2)
			{
				return controller->get_motion_sample();
			}
		}
	}

	return {};
}

bool EmulatedController::has_position() const
{
	const auto state = m_state.Read();
	return std::any_of(state->controllers.cbegin(), state->controllers.cend(), [](const auto& c) {return c->has_position(); });
}

glm::vec2 EmulatedController::get_position() const
{
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		if (controller->has_position())
			return controller->get_position();
	}

	return {};
}

glm::vec2 EmulatedController::get_prev_position() const
{
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		if (controller->has_position())
			return controller->get_prev_position();
	}

	return {};
}

PositionVisibility EmulatedController::GetPositionVisibility() const
{
	const auto state = m_state.Read();
	for (const auto& controller : state->controllers)
	{
		if (controller->has_position())
			return controller->GetPositionVisibility();
	}
	return PositionVisibility::NONE;
}

void EmulatedController::add_controller(std::shared_ptr<ControllerBase> controller)
{
	controller->connect();

	ChangeState([&] { m_controllers.emplace_back(std::move(controller)); });
}

void EmulatedController::remove_controller(const std::shared_ptr<ControllerBase>& controller)
{
	ChangeState([&] {
		const auto it = std::find(m_controllers.cbegin(), m_controllers.cend(), controller);
		if (it != m_controllers.cend())
		{
			m_controllers.erase(it);

			for(auto m = m_mappings.begin(); m != m_mappings.end();)
			{
				if(auto mc = m->second.controller.lock())
				{
					if(*mc == *controller)
					{
						m = m_mappings.erase(m);
						continue;
					}
				}

				++m;
			}
		}
	});
}

void EmulatedController::clear_controllers()
{
	ChangeState([&] { m_controllers.clear(); });
}

float EmulatedController::get_axis_value(uint64 mapping) const
{
	const auto state = m_state.Read();
	const auto it = state->mappings.find(mapping);
	if (it != state->mappings.cend())
	{
		if (const auto controller = it->second.controller.lock()) {
			return controller->get_axis_value(it->second.button);
		}
	}

	return 0;
}

bool EmulatedController::is_mapping_down(uint64 mapping) const
{
	const auto state = m_state.Read();
	const auto it = state->mappings.find(mapping);
	if (it != state->mappings.cend())
	{
		if (const auto controller = it->second.controller.lock())
			return controller->get_state().buttons.GetButtonState(it->second.button);
	}
	return false;
}


std::string EmulatedController::get_mapping_name(uint64 mapping) const
{
	const auto state = m_state.Read();
	const auto it = state->mappings.find(mapping);
	if (it != state->mappings.cend())
	{
		if (const auto controller = it->second.controller.lock()) {
			return controller->get_button_name(it->second.button);
		}
	}

	return {};
}

std::shared_ptr<ControllerBase> EmulatedController::get_mapping_controller(uint64 mapping) const
{
	const auto state = m_state.Read();
	const auto it = state->mappings.find(mapping);
	if (it != state->mappings.cend())
	{
		if (const auto controller = it->second.controller.lock()) {
			return controller;
		}
	}

	return {};
}

void EmulatedController::delete_mapping(uint64 mapping)
{
	ChangeState([&] { m_mappings.erase(mapping); });
}

void EmulatedController::clear_mappings()
{
	ChangeState([&] { m_mappings.clear(); });
}

void EmulatedController::set_mapping(uint64 mapping, const std::shared_ptr<ControllerBase>& controller,
                                     uint64 button)
{
	ChangeState([&] { m_mappings[mapping] = { controller, button }; });
}

bool EmulatedController::operator==(const EmulatedController& o) const
{
	return type() == o.type() && m_player_index == o.m_player_index;
}

bool EmulatedController::operator!=(const EmulatedController& o) const
{
	return !(*this == o);
}
