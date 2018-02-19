#include "autopilot/autopilot.h"

#include <quadrotor_common/math_common.h>
#include <quadrotor_common/parameter_helper.h>

namespace autopilot
{

// TODO: Make sure everything is initialized properly
AutoPilot::AutoPilot(const ros::NodeHandle& nh, const ros::NodeHandle& pnh) :
    nh_(nh), pnh_(pnh), autopilot_state_(States::OFF), state_before_rc_manual_flight_(
        States::OFF), state_predictor_(nh_, pnh_), reference_state_(), received_state_est_(), state_estimate_available_(
        false), time_of_switch_to_current_state_(), first_time_in_new_state_(
        true), time_to_ramp_down_(false)
{
  if (!loadParameters())
  {
    ROS_ERROR("[%s] Could not load parameters.", pnh_.getNamespace().c_str());
    ros::shutdown();
    return;
  }

  // Publishers
  control_command_pub_ = nh_.advertise<quadrotor_msgs::ControlCommand>(
      "control_command", 1);

  // Subscribers
  state_estimate_sub_ = nh_.subscribe("autopilot/state_estimate", 1,
                                      &AutoPilot::stateEstimateCallback, this);
  low_level_feedback_sub_ = nh_.subscribe("low_level_feedback", 1,
                                          &AutoPilot::lowLevelFeedbackCallback,
                                          this);

  pose_command_sub_ = nh_.subscribe("autopilot/pose_command", 1,
                                    &AutoPilot::poseCommandCallback, this);
  velocity_command_sub_ = nh_.subscribe("autopilot/velocity_command", 1,
                                        &AutoPilot::velocityCommandCallback,
                                        this);
  reference_state_sub_ = nh_.subscribe("autopilot/reference_state", 1,
                                       &AutoPilot::referenceStateCallback,
                                       this);
  trajectory_sub_ = nh_.subscribe("autopilot/trajectory", 1,
                                  &AutoPilot::trajectoryCallback, this);
  control_command_input_sub_ = nh_.subscribe(
      "autopilot/control_command_input", 1,
      &AutoPilot::controlCommandInputCallback, this);

  start_sub_ = nh_.subscribe("autopilot/start", 1, &AutoPilot::startCallback,
                             this);
  land_sub_ = nh_.subscribe("autopilot/land", 1, &AutoPilot::landCallback,
                            this);
  off_sub_ = nh_.subscribe("autopilot/off", 1, &AutoPilot::offCallback, this);
}

AutoPilot::~AutoPilot()
{
}

// TODO watchdog thread to check when the last state estimate was received
// -> trigger emergency landing if necessary
// -> set state_estimate_available_ false

void AutoPilot::stateEstimateCallback(const nav_msgs::Odometry::ConstPtr& msg)
{
  // This triggers the control loop
  state_estimate_available_ = true;

  received_state_est_ = quadrotor_common::QuadStateEstimate(*msg);
  if (!received_state_est_.isValid())
  {
    state_estimate_available_ = false;
    if (autopilot_state_ != States::OFF
        && autopilot_state_ != States::EMERGENCY_LAND)
    {
      setAutoPilotState(States::EMERGENCY_LAND);
      ROS_ERROR(
          "[%s] Received invalid state estimate, going to EMERGENCY_LANDING",
          pnh_.getNamespace().c_str());
    }
  }

  // Push received state estimate into predictor
  state_predictor_.updateWithStateEstimate(received_state_est_);

  quadrotor_common::ControlCommand control_cmd;

  ros::Time wall_time_now = ros::Time::now();
  ros::Time command_execution_time = wall_time_now
      + ros::Duration(control_command_delay_);

  quadrotor_common::QuadStateEstimate predicted_state =
      getPredictedStateEstimate(command_execution_time);

  // Compute control command depending on autopilot state
  switch (autopilot_state_)
  {
    case States::OFF:
      control_cmd.zero();
      break;
    case States::START:
      control_cmd = start(predicted_state);
      break;
    case States::HOVER:
      control_cmd = hover(predicted_state);
      break;
    case States::LAND:
      control_cmd = land(predicted_state);
      break;
    case States::EMERGENCY_LAND:
      if (state_estimate_available_)
      {
        setAutoPilotState(States::HOVER);
      }
      break;
    case States::BREAKING:
      break;
    case States::GO_TO_POSE:
      break;
    case States::VELOCITY_CONTROL:
      break;
    case States::REFERENCE_CONTROL:
      break;
    case States::TRAJECTORY_CONTROL:
      break;
    case States::COMMAND_FEEDTHROUGH:
      // Do nothing here, command is being published in the command input
      // callback directly
      break;
    case States::RC_MANUAL:
      // Send an armed command with zero body rates and hover thrust to avoid
      // letting the low level part switch off when the remote control gives
      // the authority back to our autonomous controller
      control_cmd.zero();
      control_cmd.armed = true;
      control_cmd.collective_thrust = 9.81;
      break;
  }

  if (autopilot_state_ != States::COMMAND_FEEDTHROUGH)
  {
    control_cmd.timestamp = wall_time_now;
    control_cmd.expected_execution_time = command_execution_time;
    publishControlCommand(control_cmd);
  }
}

void AutoPilot::lowLevelFeedbackCallback(
    const quadrotor_msgs::LowLevelFeedback::ConstPtr& msg)
{
  if (msg->control_mode == msg->RC_MANUAL
      && autopilot_state_ != States::RC_MANUAL)
  {
    autopilot_state_ = States::RC_MANUAL;
  }
  if (msg->control_mode != msg->RC_MANUAL
      && autopilot_state_ == States::RC_MANUAL)
  {
    if (state_before_rc_manual_flight_ == States::OFF)
    {
      autopilot_state_ = States::OFF;
    }
    else
    {
      autopilot_state_ = States::BREAKING;
    }
  }
}

void AutoPilot::poseCommandCallback(
    const geometry_msgs::PoseStamped::ConstPtr& msg)
{
}

void AutoPilot::velocityCommandCallback(
    const geometry_msgs::TwistStamped::ConstPtr& msg)
{
}

void AutoPilot::referenceStateCallback(
    const quadrotor_msgs::TrajectoryPoint::ConstPtr& msg)
{

}

void AutoPilot::trajectoryCallback(
    const quadrotor_msgs::Trajectory::ConstPtr& msg)
{

}

void AutoPilot::controlCommandInputCallback(
    const quadrotor_msgs::ControlCommand::ConstPtr& msg)
{
  if (autopilot_state_ != States::OFF && autopilot_state_ != States::HOVER)
  {
    // Only allow this if the current state is OFF or HOVER
    return;
  }

  if (autopilot_state_ != States::COMMAND_FEEDTHROUGH)
  {
    autopilot_state_ = States::COMMAND_FEEDTHROUGH;
  }

  control_command_pub_.publish(*msg);
}

void AutoPilot::startCallback(const std_msgs::Empty::ConstPtr& msg)
{
  ROS_INFO_THROTTLE(0.5, "[%s] START command received",
                    pnh_.getNamespace().c_str());
  if (autopilot_state_ == States::OFF)
  {
    if (state_estimate_available_)
    {
      if (received_state_est_.coordinate_frame
          == quadrotor_common::QuadStateEstimate::CoordinateFrame::WORLD
          || received_state_est_.coordinate_frame
              == quadrotor_common::QuadStateEstimate::CoordinateFrame::OPTITRACK)
      {
        ROS_INFO(
            "[%s] Absolute state estimate available, taking off based on it",
            pnh_.getNamespace().c_str());
        setAutoPilotState(States::START);
      }
      else if (received_state_est_.coordinate_frame
          == quadrotor_common::QuadStateEstimate::CoordinateFrame::VISION
          || received_state_est_.coordinate_frame
              == quadrotor_common::QuadStateEstimate::CoordinateFrame::LOCAL)
      {
        ROS_INFO("[%s] Relative state estimate available, switch to hover",
                 pnh_.getNamespace().c_str());
        setAutoPilotState(States::HOVER);
      }
    }
    else
    {
      ROS_ERROR("[%s] No state estimate available, will not start",
                pnh_.getNamespace().c_str());
    }
  }
  else
  {
    ROS_WARN("[%s] Autopilot is not OFF, will not switch to START",
             pnh_.getNamespace().c_str());
  }
}

void AutoPilot::landCallback(const std_msgs::Empty::ConstPtr& msg)
{
  ROS_INFO_THROTTLE(0.5, "[%s] LAND command received",
                    pnh_.getNamespace().c_str());
  if (autopilot_state_ == States::OFF || autopilot_state_ == States::LAND
      || autopilot_state_ == States::EMERGENCY_LAND
      || autopilot_state_ == States::COMMAND_FEEDTHROUGH
      || autopilot_state_ == States::RC_MANUAL)
  {
    return;
  }
  if (state_estimate_available_)
  {
    if (received_state_est_.coordinate_frame
        == quadrotor_common::QuadStateEstimate::CoordinateFrame::WORLD
        || received_state_est_.coordinate_frame
            == quadrotor_common::QuadStateEstimate::CoordinateFrame::OPTITRACK)
    {
      ROS_INFO("[%s] Absolute state estimate available, landing based on it",
               pnh_.getNamespace().c_str());
      setAutoPilotState(States::LAND);
    }
    else
    {
      ROS_INFO(
          "[%s] No absolute state estimate available, EMERGENCY_LAND instead",
          pnh_.getNamespace().c_str());
      setAutoPilotState(States::EMERGENCY_LAND);
    }
  }
  else
  {
    setAutoPilotState(States::EMERGENCY_LAND);
  }
}

void AutoPilot::offCallback(const std_msgs::Empty::ConstPtr& msg)
{
  if (autopilot_state_ != States::OFF)
  {
    ROS_INFO("[%s] OFF command received", pnh_.getNamespace().c_str());
    setAutoPilotState(States::OFF);
    // Allow user to take over manually and land the vehicle, then off the
    // controller and disable the RC without the vehicle going back to hover
    state_before_rc_manual_flight_ = States::OFF;
  }
}

quadrotor_common::ControlCommand AutoPilot::start(
    const quadrotor_common::QuadStateEstimate& state_estimate)
{
  quadrotor_common::ControlCommand command;

  if (first_time_in_new_state_)
  {
    first_time_in_new_state_ = false;
    initial_start_position_ = state_estimate.position;
    reference_state_ = quadrotor_common::TrajectoryPoint();
    reference_state_.position = state_estimate.position;
    reference_state_.heading = quadrotor_common::quaternionToEulerAnglesZYX(
        state_estimate.orientation).z();
    if (state_estimate.position.z() >= optitrack_land_drop_height_)
    {
      setAutoPilotState(States::HOVER);
    }
  }

  if (timeInCurrentState() > optitrack_start_land_timeout_
      || reference_state_.position.z() >= optitrack_start_height_)
  {
    // TODO: Switch to breaking
    setAutoPilotState(States::HOVER);
  }
  else
  {
    if (timeInCurrentState() < start_idle_duration_)
    {
      command.control_mode = quadrotor_common::ControlMode::BODY_RATES;
      command.armed = true;
      command.bodyrates = Eigen::Vector3d::Zero();
      command.collective_thrust = idle_thrust_;
      return command;
    }
    else
    {
      reference_state_.position.z() = initial_start_position_.z()
          + start_land_velocity_
              * (timeInCurrentState() - start_idle_duration_);
      reference_state_.velocity.z() = start_land_velocity_;
    }
  }

  command = base_controller_.run(state_estimate, reference_state_,
                                 base_controller_params_);

  return command;
}

quadrotor_common::ControlCommand AutoPilot::hover(
    const quadrotor_common::QuadStateEstimate& state_estimate)
{
  if (first_time_in_new_state_)
  {
    first_time_in_new_state_ = false;
    reference_state_ = quadrotor_common::TrajectoryPoint();
    reference_state_.position = state_estimate.position;
    reference_state_.heading = quadrotor_common::quaternionToEulerAnglesZYX(
        state_estimate.orientation).z();
  }

  const quadrotor_common::ControlCommand command = base_controller_.run(
      state_estimate, reference_state_, base_controller_params_);

  return command;
}

quadrotor_common::ControlCommand AutoPilot::land(
    const quadrotor_common::QuadStateEstimate& state_estimate)
{
  quadrotor_common::ControlCommand command;

  if (first_time_in_new_state_)
  {
    first_time_in_new_state_ = false;
    initial_land_position_ = state_estimate.position;
    reference_state_ = quadrotor_common::TrajectoryPoint();
    reference_state_.position = state_estimate.position;
    reference_state_.heading = quadrotor_common::quaternionToEulerAnglesZYX(
        state_estimate.orientation).z();
    // Initialize drop thrust for the case quad is already below drop height
    time_to_ramp_down_ = false;
  }

  reference_state_.position.z() = fmax(
      0.0,
      initial_land_position_.z() - start_land_velocity_ * timeInCurrentState());
  reference_state_.velocity.z() = -start_land_velocity_;

  command = base_controller_.run(state_estimate, reference_state_,
                                 base_controller_params_);

  if (!time_to_ramp_down_
      && (state_estimate.position.z() < optitrack_land_drop_height_
          || timeInCurrentState() > optitrack_start_land_timeout_))
  {
    time_to_ramp_down_ = true;
    time_started_ramping_down_ = ros::Time::now();
  }

  if (time_to_ramp_down_)
  {
    // we are low enough -> ramp down the thrust
    // we timed out on landing -> ramp down the thrust
    ROS_INFO_THROTTLE(2, "[%s] Ramping propeller down",
                      pnh_.getNamespace().c_str());
    command.collective_thrust = initial_drop_thrust_
        - initial_drop_thrust_ / propeller_ramp_down_timeout_
            * (ros::Time::now() - time_started_ramping_down_).toSec();
  }

  if (command.collective_thrust <= 0.0)
  {
    setAutoPilotState(States::OFF);
    command.zero();
  }

  return command;
}

void AutoPilot::setAutoPilotState(const States& new_state)
{
  time_of_switch_to_current_state_ = ros::Time::now();
  first_time_in_new_state_ = true;

  if (new_state == States::RC_MANUAL)
  {
    state_before_rc_manual_flight_ = States::OFF;
  }

  autopilot_state_ = new_state;
}

double AutoPilot::timeInCurrentState() const
{
  return (ros::Time::now() - time_of_switch_to_current_state_).toSec();
}

quadrotor_common::QuadStateEstimate AutoPilot::getPredictedStateEstimate(
    const ros::Time& time) const
{
  return state_predictor_.predictState(time);
}

void AutoPilot::publishControlCommand(
    const quadrotor_common::ControlCommand& control_cmd)
{
  if (control_cmd.control_mode == quadrotor_common::ControlMode::NONE)
  {
    ROS_ERROR("[%s] Control mode is NONE, will not publish ControlCommand",
              pnh_.getNamespace().c_str());
  }
  else
  {
    quadrotor_msgs::ControlCommand control_cmd_msg;

    control_cmd_msg = control_cmd.toRosMessage();

    control_command_pub_.publish(control_cmd_msg);
    state_predictor_.pushCommandToQueue(control_cmd);
    // Save applied thrust to initialize propeller ramping down if necessary
    initial_drop_thrust_ = control_cmd.collective_thrust;
  }
}

bool AutoPilot::loadParameters()
{
#define GET_PARAM(name) \
if (!quadrotor_common::getParam(#name, name ## _, pnh_)) \
  return false

  GET_PARAM(velocity_estimate_in_world_frame);
  GET_PARAM(control_command_delay);
  GET_PARAM(optitrack_land_drop_height);
  GET_PARAM(optitrack_start_land_timeout);
  GET_PARAM(optitrack_start_height);
  GET_PARAM(start_idle_duration);
  GET_PARAM(idle_thrust);
  GET_PARAM(start_land_velocity);
  GET_PARAM(propeller_ramp_down_timeout);

  if (!base_controller_params_.loadParameters(pnh_))
  {
    return false;
  }

  return true;

#undef GET_PARAM
}

} // namespace autopilot
