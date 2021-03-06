/*
 * Copyright (c) 2012, Yujin Robot.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Yujin Robot nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * @file /kobuki_node/src/node/kobuki_node.cpp
 *
 * @brief Implementation for the ros kobuki node wrapper.
 **/

/*****************************************************************************
 ** Includes
 *****************************************************************************/

#include <float.h>
#include <tf/tf.h>
#include <ecl/streams/string_stream.hpp>
#include <kobuki_comms/VersionInfo.h>
#include "kobuki_node/kobuki_node.hpp"

/*****************************************************************************
 ** Namespaces
 *****************************************************************************/

namespace kobuki
{

/*****************************************************************************
 ** Implementation [KobukiNode]
 *****************************************************************************/

/**
 * @brief Default constructor.
 *
 * Make sure you call the init() method to fully define this node.
 */
KobukiNode::KobukiNode(std::string& node_name) :
    name(node_name),
    slot_version_info(&KobukiNode::publishVersionInfo, *this),
    slot_stream_data(&KobukiNode::processStreamData, *this),
    slot_button_event(&KobukiNode::publishButtonEvent, *this),
    slot_bumper_event(&KobukiNode::publishBumperEvent, *this),
    slot_debug(&KobukiNode::rosDebug, *this),
    slot_info(&KobukiNode::rosInfo, *this),
    slot_warn(&KobukiNode::rosWarn, *this),
    slot_error(&KobukiNode::rosError, *this)
{
  joint_states.name.push_back("left_wheel_joint");
  joint_states.name.push_back("right_wheel_joint");
  joint_states.name.push_back("front_wheel_joint"); // front_castor_joint in create tbot
  joint_states.name.push_back("rear_wheel_joint");  // back_castor_joint in create tbot
  joint_states.position.resize(4,0.0);
  joint_states.velocity.resize(4,0.0);
  joint_states.effort.resize(4,0.0);

  updater.setHardwareID("Kobuki");
  updater.add(battery_diagnostics);
  updater.add(watchdog_diagnostics);
}

/**
 * This will wait some time while kobuki internally closes its threads and destructs
 * itself.
 */
KobukiNode::~KobukiNode()
{
  ROS_INFO_STREAM("Kobuki : waiting for kobuki thread to finish [" << name << "].");
}

bool KobukiNode::init(ros::NodeHandle& nh)
{
  /*********************
   ** Communications
   **********************/
  advertiseTopics(nh);
  subscribeTopics(nh);

  /*********************
   ** Slots
   **********************/
  slot_stream_data.connect(name + std::string("/stream_data"));
  slot_version_info.connect(name + std::string("/version_info"));
  slot_button_event.connect(name + std::string("/button_event"));
  slot_bumper_event.connect(name + std::string("/bumper_event"));
  slot_debug.connect(name + std::string("/ros_debug"));
  slot_info.connect(name + std::string("/ros_info"));
  slot_warn.connect(name + std::string("/ros_warn"));
  slot_error.connect(name + std::string("/ros_error"));

  /*********************
   ** Driver Parameters
   **********************/
  Parameters parameters;

  nh.param("battery_capacity", parameters.battery_capacity, Battery::capacity);
  nh.param("battery_low", parameters.battery_low, Battery::low);
  nh.param("battery_dangerous", parameters.battery_dangerous, Battery::dangerous);

  parameters.sigslots_namespace = name; // name is automatically picked up by device_nodelet parent.
  if (!nh.getParam("device_port", parameters.device_port))
  {
    ROS_ERROR_STREAM("Kobuki : no device port given on the parameter server (e.g. /dev/ttyUSB0)[" << name << "].");
    return false;
  }
  if (!nh.getParam("protocol_version", parameters.protocol_version))
  {
    ROS_ERROR_STREAM("Kobuki : no protocol version given on the parameter server ('2.0')[" << name << "].");
    return false;
  }

  /*********************
   ** Validation
   **********************/
  if (!parameters.validate())
  {
    ROS_ERROR_STREAM("Kobuki : parameter configuration failed [" << name << "].");
    ROS_ERROR_STREAM("Kobuki : " << parameters.error_msg << "[" << name << "]");
    return false;
  }
  else
  {
    if ( parameters.simulation ) {
      ROS_INFO("Kobuki : driver going into loopback (simulation) mode.");
    } else {
      ROS_INFO_STREAM("Kobuki : configured for connection on device_port " << parameters.device_port << " [" << name << "].");
      ROS_INFO_STREAM("Kobuki : configured for firmware protocol_version " << parameters.protocol_version << " [" << name << "].");
      ROS_INFO_STREAM("Kobuki : driver running in normal (non-simulation) mode" << " [" << name << "].");
    }
  }

  odometry.init(nh, name);

  /*********************
   ** Driver Init
   **********************/
  try
  {
    kobuki.init(parameters);
    ros::Duration(0.25).sleep(); // wait for some data to come in.
    if ( !kobuki.isAlive() ) {
      ROS_ERROR_STREAM("Kobuki : no data stream, is kobuki turned on?");
      // don't need to return false here - simply turning kobuki on while spin()'ing should resurrect the situation.
    }
    kobuki.enable();
  }
  catch (const ecl::StandardException &e)
  {
    switch (e.flag())
    {
      case (ecl::OpenError):
      {
        ROS_ERROR_STREAM("Kobuki : could not open connection [" << parameters.device_port << "][" << name << "].");
        break;
      }
      case (ecl::NotFoundError):
      {
        ROS_ERROR_STREAM("Kobuki : could not find the device [" << parameters.device_port << "][" << name << "].");
        break;
      }
      default:
      {
        ROS_ERROR_STREAM("Kobuki : initialisation failed [" << name << "].");
        ROS_ERROR_STREAM(e.what());
        break;
      }
    }
    return false;
  }

//  ecl::SigSlotsManager<>::printStatistics();
//  ecl::SigSlotsManager<const std::string&>::printStatistics();
//  ecl::SigSlotsManager<const VersionInfo&>::printStatistics();
//  ecl::SigSlotsManager<const ButtonEvent&>::printStatistics();

  return true;
}

bool KobukiNode::spin()
{
  ros::Rate loop_rate(10); // 100ms - cmd_vel_timeout should be greater than this
  bool timed_out = false; // stops warning spam when vel_cmd flags as timed out more than once in a row

  while (ros::ok())
  {
    if ( (kobuki.isEnabled() == true) && odometry.commandTimeout()) {
      if ( !timed_out ) {
        std_msgs::StringPtr msg;
        //disable(msg);
        kobuki.setBaseControl(0, 0);
        timed_out = true;
        ROS_WARN("Incoming velocity commands not received for more than %.2f seconds -> disabling driver", odometry.timeout().toSec());
      }
    } else {
      timed_out = false;
    }

    bool is_alive = kobuki.isAlive();
    if ( watchdog_diagnostics.isAlive() && !is_alive ) {
      ROS_ERROR_STREAM("Kobuki : timed out waiting for the serial data stream [" << name << "].");
    }
    watchdog_diagnostics.update(is_alive);
    battery_diagnostics.update(kobuki.batteryStatus());
    updater.update();
    ros::spinOnce();
    loop_rate.sleep();
  }

  return true;
}

/**
 * Two groups of publishers, one required by turtlebot, the other for
 * kobuki esoterics.
 */
void KobukiNode::advertiseTopics(ros::NodeHandle& nh)
{
  /*********************
  ** Turtlebot Required
  **********************/
  joint_state_publisher = nh.advertise <sensor_msgs::JointState>("joint_states",100);

  /*********************
  ** Kobuki Esoterics
  **********************/
  version_info_publisher = nh.advertise < kobuki_comms::VersionInfo > ("version_info", 100, true); // latched publisher
  button_event_publisher = nh.advertise < kobuki_comms::ButtonEvent > ("events/buttons", 100);
  bumper_event_publisher = nh.advertise < kobuki_comms::BumperEvent > ("events/bumpers", 100);
  sensor_state_publisher = nh.advertise < kobuki_comms::SensorState > ("sensors/core", 100);
  imu_data_publisher = nh.advertise < sensor_msgs::Imu > ("sensors/imu_data", 100);
}

/**
 * Two groups of subscribers, one required by turtlebot, the other for
 * kobuki esoterics.
 */
void KobukiNode::subscribeTopics(ros::NodeHandle& nh)
{
  velocity_command_subscriber = nh.subscribe(std::string("cmd_vel"), 10, &KobukiNode::subscribeVelocityCommand, this);
  led_command_subscriber =  nh.subscribe(std::string("commands/led"), 10, &KobukiNode::subscribeLedCommand, this);
  digital_output_command_subscriber =  nh.subscribe(std::string("commands/digital_output"), 10, &KobukiNode::subscribeDigitalOutputCommand, this);
  sound_command_subscriber =  nh.subscribe(std::string("commands/sound"), 10, &KobukiNode::subscribeSoundCommand, this);
  // A group enable/disable channel to listen to (these should get remapped to /enable in most cases).
  enable_subscriber = nh.subscribe("enable", 10, &KobukiNode::enable, this); // 10 is queue size
  disable_subscriber = nh.subscribe("disable", 10, &KobukiNode::disable, this);
  reset_odometry_subscriber = nh.subscribe("reset_odometry", 10, &KobukiNode::subscribeResetOdometry, this);
}


} // namespace kobuki


/*		slot_reserved0, Rei */

