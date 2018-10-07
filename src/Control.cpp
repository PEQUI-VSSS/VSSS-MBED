#include "Control.h"
#include "helper_functions.h"
#define PI 3.1415926f

Control::Control() : sensors(&controller) {
	controller.set_target_velocity(0, 0, 0);
	controller.set_pwm(controller.left_wheel, 0);
	controller.set_pwm(controller.right_wheel, 0);
	backwards_timer.start();
}

void Control::start_threads() {
	controller.start_thread();
	sensors.ekf_thread_start();
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
	if(!sleep_enabled) return;

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

void Control::set_target_pose(float x, float y, float theta,
							  bool stop_afterwards, float velocity) {
	target = {x, y, theta, velocity};
	this->stop_afterwards = stop_afterwards;
	state = ControlState::Pose;
	resume_threads();
}

void Control::set_vector_control(float target_theta, float velocity) {
	target = {0, 0, target_theta, velocity};
	state = ControlState::Vector;
	resume_threads();
}

void Control::set_target_orientation(float theta) {
	target = {0, 0, theta, 0};
	state = ControlState::Orientation;
	resume_threads();
}

void Control::set_target_position(float x, float y, float velocity) {
	target = {x, y, 0, velocity};
	state = ControlState::Position;
	resume_threads();
}

void Control::set_ang_vel_control(float angular_velocity) {
	target = {0, 0, 0, angular_velocity};
	state = ControlState::AngularVel;
	resume_threads();
}

TargetVelocity Control::set_stop_and_sleep() {
	state = ControlState::None;
	return {0, 0};
}

void Control::pose_control_thread() {
	while (true) {
		if (state == ControlState::None ||
				sensors.timeout.read_ms() > 500) stop_and_sleep();

		backwards = backwards_select(target.theta);
		auto target = this->target.or_backwards_vel(backwards);
		auto pose = sensors.get_pose().or_backwards(backwards);

		auto target_vel = [&]() {
			switch (state) {
				case ControlState::Pose:
					return pose_control(sensors.get_pose(), this->target);
				case ControlState::Position:
					return position_control(pose, target);
				case ControlState::Vector:
					return vector_control(pose, target.theta, target.velocity);
				case ControlState::Orientation:
					return orientation_control(pose, target.theta);
				case ControlState::AngularVel:
					return TargetVelocity{0, target.velocity};
				default:
					return TargetVelocity{0, 0};
			}
		}();
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
	if(polar_pose.error < 0.02) {
		return vector_control(pose, target.theta, target.velocity);
	} else {
		return control_law(polar_pose, target.velocity);
	}
}

TargetVelocity Control::position_control(Pose pose, Target target) {
	float target_theta = std::atan2(target.y - pose.y,
									target.x - pose.x);
	float error = std::sqrt(std::pow(target.x - pose.x, 2.0f)
							+ std::pow(target.y - pose.y, 2.0f));
	if (error < 0.02) return set_stop_and_sleep();
	else return vector_control(pose, target_theta, target.velocity * std::sqrt(error));
}

TargetVelocity Control::vector_control(Pose pose, float target_theta, float velocity) const {
	return {velocity, 10 * wrap(target_theta - pose.theta)};
}

TargetVelocity Control::orientation_control(Pose pose, float theta) {
	return {0, 15 * wrap(theta - pose.theta)};
}

PolarPose Control::get_polar_pose(Pose pose, Target target) const {
	float error = std::sqrt(std::pow(target.x - pose.x, 2.0f) + std::pow(target.y - pose.y, 2.0f));
	float robot_to_targ = std::atan2(target.y - pose.y, target.x - pose.x);
	float theta = wrap(robot_to_targ - target.theta);
	float alpha = wrap(robot_to_targ - pose.theta);
	return {error, -theta, -alpha};
}

TargetVelocity Control::control_law(PolarPose pose, float vmax) const {
	float k = (-1 / pose.error) *
			(k2 * (pose.alpha - std::atan(-k1 * pose.theta))
			 + (1 + k1 / (1 + std::pow(k1 * pose.theta, 2.0f))) * std::sin(pose.alpha));
	float v = vmax/(1 + B * std::pow(k, 2.0f));
	float w = v * k;
	return {v, w};
}

bool Control::backwards_select(float target_theta) {
	auto theta = sensors.get_pose().theta;
	if(backwards_timer.read_ms() > 50) {
		bool go_backwards = std::abs(wrap(target_theta - theta)) > PI/2;
		if(backwards != go_backwards) backwards_timer.reset();
		backwards = go_backwards;
		return go_backwards;
	} else {
		return backwards;
	}
}
