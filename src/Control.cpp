#include "Control.h"
#include "helper_functions.h"
#include "PIN_MAP.h"

#define PI 3.1415926f

Control::Control() : sensors(&controller) {
	controller.set_target_velocity(0, 0, 0);
	controller.set_pwm(controller.left_wheel, 0);
	controller.set_pwm(controller.right_wheel, 0);
	backwards_timer.start();
}

void Control::start_threads() {
	controller.start_thread();
	back_apds = new ApdsSensor(p9, p10);
	front_apds = new ApdsSensor(IMU_SDA_PIN, IMU_SCL_PIN);
	sensors.ekf_thread_start(&front_apds->i2c);
	control_thread.start(callback(this, &Control::pose_control_thread));
}

void Control::resume_threads() {
	control_thread.signal_set(CONTINUE_SIGNAL);
	controller.continue_thread();
}

void Control::reset_timeout() {
	sensors.timeout.reset();
}

void Control::stop_and_sleep() {
	if (!sleep_enabled) return;

	state = ControlState::None;
	controller.stop = true;
	Thread::signal_wait(CONTINUE_SIGNAL);
	Thread::signal_clr(CONTINUE_SIGNAL);
	reset_timeout();
}

// Return from sleep
void Control::set_ekf_vision_data(float x, float y, float theta) {
	sensors.set_vision_data(x, y, theta);
}

void Control::set_target(ControlState control_type, Target target,
						 bool stop_afterwards) {
	this->stop_afterwards = stop_afterwards;
	this->target = target;
	state = control_type;
	resume_threads();
}

TargetVelocity Control::set_stop_and_sleep() {
	state = ControlState::None;
	return {0, 0};
}

void Control::pose_control_thread() {
	while (true) {
		if (state == ControlState::None ||
			sensors.timeout.read_ms() > 1000)
			stop_and_sleep();

//		backwards = backwards_select(target.theta);
//		auto target = this->target.or_backwards_vel(backwards);
//		auto pose = sensors.get_pose().or_backwards(backwards);
		auto pose = sensors.get_pose();

		auto target_vel = [&]() {
			switch (state) {
				case ControlState::Pose: {
					auto position = pose.position;
					float error = std::sqrt(std::pow(target.position.x - position.x, 2.0f)
											+ std::pow(target.position.y - position.y, 2.0f));
					if (error < 0.02) return set_stop_and_sleep();
					return vfo.control_law(this->target, sensors.get_pose());
//					return pose_control(sensors.get_pose(), this->target);
				} case ControlState::SeekBall:
					return run_to_ball(sensors.get_pose(), this->target);
				case ControlState::Position:
					return position_control(pose, target);
				case ControlState::Vector:
					return vector_control(pose.theta, target.theta, target.velocity);
				case ControlState::Orientation:
					return orientation_control(pose, target.theta);
				case ControlState::AngularVel:
					return TargetVelocity{0, target.velocity};
				default:
					return TargetVelocity{0, 0};
			}
		}();
		target_vel.v = limit_error(target_vel.v, pose.v, 0.4f);
//		target_vel.w = limit_error(target_vel.w, pose.w, 10);
		auto target_wheel_vel = get_target_wheel_velocity(target_vel);
		controller.set_target_velocity(target_wheel_vel);
		Thread::wait(10);
	}
}

WheelVelocity Control::get_target_wheel_velocity(TargetVelocity target) const {
	return {target.v - target.w * ROBOT_SIZE / 2,
			target.v + target.w * ROBOT_SIZE / 2};
}

TargetVelocity Control::pose_control(Pose pose, Target target) {
	const PolarPose polar_pose = get_polar_pose(pose, target);
	if (polar_pose.error < 0.02) {
		return vector_control(pose.theta, target.theta, target.velocity);
	} else {
		return control_law(polar_pose, target.velocity);
	}
}

TargetVelocity Control::position_control(Pose pose, Target target) {
	auto position = pose.position;
	float target_theta = std::atan2(target.position.y - position.y,
									target.position.x - position.x);
	float error = std::sqrt(std::pow(target.position.x - position.x, 2.0f)
							+ std::pow(target.position.y - position.y, 2.0f));
	if (error < 0.02) return set_stop_and_sleep();
	else return vector_control(pose.theta, target_theta, target.velocity * std::sqrt(error));
}

TargetVelocity Control::vector_control(float theta, float target_theta, float velocity) const {
	auto error = wrap(target_theta - theta);
	return {velocity * std::cos(error), 10 * error};
}

TargetVelocity Control::orientation_control(Pose pose, float theta) {
	return {0, 15 * wrap(theta - pose.theta)};
}

PolarPose Control::get_polar_pose(Pose pose, Target target) const {
	auto position = pose.position;
	float error = std::sqrt(std::pow(target.position.x - position.x, 2.0f)
							+ std::pow(target.position.y - position.y, 2.0f));
	float robot_to_targ = std::atan2(target.position.y - position.y, target.position.x - position.x);
	float theta = wrap(robot_to_targ - target.theta);
	float alpha = wrap(robot_to_targ - pose.theta);
	return {error, -theta, -alpha};
}

TargetVelocity Control::control_law(PolarPose pose, float vmax) const {
	float k = (-1 / pose.error) *
			  (k2 * (pose.alpha - std::atan(-k1 * pose.theta))
			   + (1 + k1 / (1 + std::pow(k1 * pose.theta, 2.0f))) * std::sin(pose.alpha));
	float v = vmax / (1 + B * std::pow(k, 2.0f));
	float w = v * k;
	return {v, w};
}

bool Control::backwards_select(float target_theta) {
	auto theta = sensors.get_pose().theta;
	if (backwards_timer.read_ms() > 50) {
		bool go_backwards = std::abs(wrap(target_theta - theta)) > PI / 2;
		if (backwards != go_backwards) backwards_timer.reset();
		backwards = go_backwards;
		return go_backwards;
	} else {
		return backwards;
	}
}

//TODO: found rules and max proximity
bool found_front, found_back;
TargetVelocity Control::seek_ball(Pose pose, Target target) {
	if (found_front) {
//		auto obj = front_apds.get_obj();
//		if (obj.proximity > 20) {
//			return vector_control(pose.theta, wrap(pose.theta - obj.theta), target.velocity);
//		} else {
//			return vfo.control_law(target, pose);
//		}
	} else if (found_back) {
		auto obj = back_apds->get_obj();
		if (obj.proximity > 20) {
			return vector_control(pose.theta, wrap(pose.theta - obj.theta), -target.velocity);
		} else {
			return vfo.control_law(target, pose);
		}
	} else {
		return vfo.control_law(target, pose);
	}
}

TargetVelocity Control::run_to_ball(Pose pose, Target target) {
	auto obj = front_apds->get_obj();
	if (obj.proximity > 10) {
//	if (false) {
		return vector_control(pose.theta, wrap(pose.theta - obj.theta), target.velocity);
	} else {
		auto back_obj = back_apds->get_obj();
		if (back_obj.proximity > 10) {
			return vector_control(pose.theta, wrap(pose.theta - back_obj.theta), -target.velocity);
		} else {
			return {0, 0};
		}
	}
}
