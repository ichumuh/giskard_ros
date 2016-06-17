/*
* Copyright (C) 2015, 2016 Jannik Buckelo <jannikbu@cs.uni-bremen.de>,
* Georg Bartels <georg.bartels@cs.uni-bremen.de>
*
*
* This file is part of giskard_examples.
*
* giskard_examples is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2 
* of the License, or (at your option) any later version.  
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License 
* along with this program; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <ros/ros.h>
#include <ros/package.h>
#include <sensor_msgs/JointState.h>
#include <std_msgs/Float64.h>
#include <giskard_msgs/WholeBodyCommand.h>
#include <giskard_msgs/ControllerFeedback.h>
#include <yaml-cpp/yaml.h>
#include <giskard/giskard.hpp>
#include <kdl_conversions/kdl_msg.h>
#include <boost/lexical_cast.hpp>
#include <giskard_examples/utils.hpp>

// TODO: separate this into a library and executable part
// TODO: refactor this into classes

int nWSR_;
giskard::QPController controller_;
std::vector<std::string> joint_names_;
std::vector<ros::Publisher> vel_controllers_;
ros::Publisher feedback_pub_, current_goal_pub_;
ros::Subscriber js_sub_;
Eigen::VectorXd state_;
bool controller_started_;
std::string frame_id_;
giskard_msgs::ControllerFeedback feedback_msg_;
size_t current_goal_hash_;

void js_callback(const sensor_msgs::JointState::ConstPtr& msg)
{
  // TODO: turn this into a map!
  // is there a more efficient way?
  for (unsigned int i=0; i < joint_names_.size(); i++)
  {
    for (unsigned int j=0; j < msg->name.size(); j++)
    {
      if (msg->name[j].compare(joint_names_[i]) == 0)
      {
        state_[i] = msg->position[j];
      }
    }
  }

  // TODO: add watchdog
  if (!controller_started_)
    return;

  if (controller_.update(state_, nWSR_))
  {
    for (unsigned int i=0; i < vel_controllers_.size(); i++)
    {
      std_msgs::Float64 command;
      command.data = controller_.get_command()[i];
      vel_controllers_[i].publish(command);
    }
 
    for(size_t i=0; i<feedback_msg_.commands.size(); ++i)
      feedback_msg_.commands[i].value = controller_.get_command()[i];
    for(size_t i=0; i<feedback_msg_.slacks.size(); ++i)
      feedback_msg_.slacks[i].value = controller_.get_slack()[i];
    feedback_pub_.publish(feedback_msg_);
  }
  else
  {
    ROS_WARN("Update failed.");
    ROS_DEBUG_STREAM("Update failed. State: " << state_);
  }

  // TODO: publish diagnostics
}

void print_eigen(const Eigen::VectorXd& command)
{
  std::string cmd_str = " ";
  for(size_t i=0; i<command.rows(); ++i)
    cmd_str += boost::lexical_cast<std::string>(command[i]) + " ";
  ROS_DEBUG("Command: (%s)", cmd_str.c_str());
}

void goal_callback(const giskard_msgs::WholeBodyCommand::ConstPtr& msg)
{
  size_t new_goal_hash = giskard_examples::calculateHash<giskard_msgs::WholeBodyCommand>(*msg);
  if(current_goal_hash_ == new_goal_hash)
    return;
  else
    current_goal_hash_ = new_goal_hash;

  if(msg->left_ee_goal.header.frame_id.compare(frame_id_) != 0)
  {
    ROS_WARN("frame_id of left EE goal did not match expected '%s'. Ignoring goal", 
        frame_id_.c_str());
    return;
  }

  if(msg->right_ee_goal.header.frame_id.compare(frame_id_) != 0)
  {
    ROS_WARN("frame_id of right EE goal did not match expected '%s'. Ignoring goal", 
        frame_id_.c_str());
    return;
  }

  // copying over left goal
  state_[joint_names_.size() + 0] = msg->left_ee_goal.pose.position.x;
  state_[joint_names_.size() + 1] = msg->left_ee_goal.pose.position.y;
  state_[joint_names_.size() + 2] = msg->left_ee_goal.pose.position.z;

  KDL::Rotation rot;
  tf::quaternionMsgToKDL(msg->left_ee_goal.pose.orientation, rot);
  rot.GetEulerZYX(state_[joint_names_.size() + 3], state_[joint_names_.size() + 4], 
      state_[joint_names_.size() + 5]);

  // copying over right goal
  state_[joint_names_.size() + 6] = msg->right_ee_goal.pose.position.x;
  state_[joint_names_.size() + 7] = msg->right_ee_goal.pose.position.y;
  state_[joint_names_.size() + 8] = msg->right_ee_goal.pose.position.z;

  tf::quaternionMsgToKDL(msg->right_ee_goal.pose.orientation, rot);
  rot.GetEulerZYX(state_[joint_names_.size() + 9], state_[joint_names_.size() + 10], 
      state_[joint_names_.size() + 11]);

  current_goal_pub_.publish(*msg);

  // TODO: check that joint-state contains all necessary joints

  if (!controller_started_)
  {
    if (controller_.start(state_, nWSR_))
    {
      ROS_DEBUG("Controller started.");
      controller_started_ = true;
    }
    else
    {
      ROS_ERROR("Couldn't start controller.");
      print_eigen(state_);
    }
  }
}

giskard_msgs::ControllerFeedback initFeedbackMsg(const giskard::QPController& controller)
{
  giskard_msgs::ControllerFeedback msg;

  msg.commands.resize(controller.get_controllable_names().size());
  for(size_t i=0; i<controller.get_controllable_names().size(); ++i)
    msg.commands[i].semantics = controller.get_controllable_names()[i];

  msg.slacks.resize(controller.get_soft_constraint_names().size());
  for(size_t i=0; i<controller.get_soft_constraint_names().size(); ++i)
    msg.slacks[i].semantics = controller.get_soft_constraint_names()[i];

  return msg;
}

int main(int argc, char **argv)
{
  ros::init(argc, argv, "pr2_controller");
  ros::NodeHandle nh("~");

  nh.param("nWSR", nWSR_, 10);

  std::string controller_description;
  if (!nh.getParam("controller_description", controller_description))
  {
    ROS_ERROR("Parameter 'controller_description' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  // TODO: extract joint_names from controller description
  if (!nh.getParam("joint_names", joint_names_))
  {
    ROS_ERROR("Parameter 'joint_names' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  if (!nh.getParam("frame_id", frame_id_))
  {
    ROS_ERROR("Parameter 'frame_id' not found in namespace '%s'.", nh.getNamespace().c_str());
    return 0;
  }

  YAML::Node node = YAML::Load(controller_description);
  giskard::QPControllerSpec spec = node.as< giskard::QPControllerSpec >();
  controller_ = giskard::generate(spec);
  state_ = Eigen::VectorXd::Zero(joint_names_.size() + 2*6);
  controller_started_ = false;

  for (std::vector<std::string>::iterator it = joint_names_.begin(); it != joint_names_.end(); ++it)
    vel_controllers_.push_back(nh.advertise<std_msgs::Float64>("/" + it->substr(0, it->size() - 6) + "_velocity_controller/command", 1));

  feedback_pub_ = nh.advertise<giskard_msgs::ControllerFeedback>("feedback", 1);
  current_goal_pub_ = nh.advertise<giskard_msgs::WholeBodyCommand>("current_goal", 1, true);
  feedback_msg_ = initFeedbackMsg(controller_);

  ROS_DEBUG("Waiting for goal.");
  ros::Subscriber goal_sub = nh.subscribe("goal", 0, goal_callback);
  js_sub_ = nh.subscribe("joint_states", 0, js_callback);
  ros::spin();

  return 0;
}
