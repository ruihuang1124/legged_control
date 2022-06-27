//
// Created by qiayuan on 2022/6/24.
//

#include <pinocchio/fwd.hpp>  // forward declarations must be included first.

#include <pinocchio/algorithm/frames.hpp>
#include <pinocchio/algorithm/jacobian.hpp>

#include "quad_controllers/ocs2_controller.h"

#include <pluginlib/class_list_macros.hpp>
#include <ocs2_core/thread_support/ExecuteAndSleep.h>
#include <ocs2_core/thread_support/SetThreadPriority.h>
#include <ocs2_centroidal_model/CentroidalModelPinocchioMapping.h>
#include <ocs2_centroidal_model/AccessHelperFunctions.h>
#include <ocs2_pinocchio_interface/PinocchioEndEffectorKinematics.h>
#include <ocs2_legged_robot_ros/gait/GaitReceiver.h>
#include <ocs2_ros_interfaces/synchronized_module/RosReferenceManager.h>
#include <ocs2_ros_interfaces/common/RosMsgConversions.h>
#include <ocs2_msgs/mpc_observation.h>

namespace quad_ros
{
bool Ocs2Controller::init(hardware_interface::RobotHW* robot_hw, ros::NodeHandle& controller_nh)
{
  HybridJointInterface* hybrid_joint_interface = robot_hw->get<HybridJointInterface>();
  std::vector<std::string> joint_names{ "LF_HAA", "LF_HFE", "LF_KFE", "LH_HAA", "LH_HFE", "LH_KFE",
                                        "RF_HAA", "RF_HFE", "RF_KFE", "RH_HAA", "RH_HFE", "RH_KFE" };
  for (const auto& joint_name : joint_names)
    hybrid_joint_handles_.push_back(hybrid_joint_interface->getHandle(joint_name));

  // Initialize OCS2
  std::string task_file, urdf_file, reference_file;
  controller_nh.getParam("/task_file", task_file);
  controller_nh.getParam("/urdf_file", urdf_file);
  controller_nh.getParam("/referenceFile", reference_file);

  // Robot interface
  legged_interface_ = std::make_shared<LeggedRobotInterface>(task_file, urdf_file, reference_file);
  mpc_ = std::make_shared<MultipleShootingMpc>(legged_interface_->mpcSettings(), legged_interface_->sqpSettings(),
                                               legged_interface_->getOptimalControlProblem(),
                                               legged_interface_->getInitializer());
  const std::string robot_name = "legged_robot";
  ros::NodeHandle nh;
  // Gait receiver
  auto gait_receiver_ptr = std::make_shared<GaitReceiver>(
      nh, legged_interface_->getSwitchedModelReferenceManagerPtr()->getGaitSchedule(), robot_name);
  // ROS ReferenceManager
  auto ros_reference_manager_ptr =
      std::make_shared<RosReferenceManager>(robot_name, legged_interface_->getReferenceManagerPtr());
  ros_reference_manager_ptr->subscribe(nh);
  mpc_->getSolverPtr()->addSynchronizedModule(gait_receiver_ptr);
  mpc_->getSolverPtr()->setReferenceManager(ros_reference_manager_ptr);
  observation_publisher_ = nh.advertise<ocs2_msgs::mpc_observation>(robot_name + "_mpc_observation", 1);

  // State Estimate
  state_estimate_ =
      std::make_shared<FromTopicStateEstimate>(nh, legged_interface_->getPinocchioInterface(),
                                               legged_interface_->getCentroidalModelInfo(), hybrid_joint_handles_);

  // Visualization
  CentroidalModelPinocchioMapping pinocchio_mapping(legged_interface_->getCentroidalModelInfo());
  PinocchioEndEffectorKinematics ee_kinematics(legged_interface_->getPinocchioInterface(), pinocchio_mapping,
                                               legged_interface_->modelSettings().contactNames3DoF);
  visualizer_ = std::make_shared<LeggedRobotVisualizer>(legged_interface_->getPinocchioInterface(),
                                                        legged_interface_->getCentroidalModelInfo(), ee_kinematics, nh);

  // Create the MPC MRT Interface
  mpc_mrt_interface_ = std::make_shared<MPC_MRT_Interface>(*mpc_);
  mpc_mrt_interface_->initRollout(&legged_interface_->getRollout());

  controller_running_ = true;
  mpc_thread_ = std::thread([&]() {
    while (controller_running_)
    {
      try
      {
        ocs2::executeAndSleep(
            [&]() {
              if (mpc_running_)
                mpc_mrt_interface_->advanceMpc();
            },
            legged_interface_->mpcSettings().mpcDesiredFrequency_);
      }
      catch (const std::exception& e)
      {
        controller_running_ = false;
        ROS_ERROR_STREAM("[Ocs2 MPC thread] Error : " << e.what());
      }
    }
  });
  ocs2::setThreadPriority(legged_interface_->ddpSettings().threadPriority_, mpc_thread_);

  return true;
}

void Ocs2Controller::starting(const ros::Time& time)
{
  // Initial state
  current_observation_.mode = ModeNumber::STANCE;
  current_observation_.time = time.toSec();
  current_observation_.state = state_estimate_->update();
  current_observation_.input.setZero(legged_interface_->getCentroidalModelInfo().inputDim);

  TargetTrajectories target_trajectories({ current_observation_.time }, { current_observation_.state },
                                         { current_observation_.input });

  // Set the first observation and command and wait for optimization to finish
  ROS_INFO_STREAM("Waiting for the initial policy ...");
  mpc_mrt_interface_->setCurrentObservation(current_observation_);
  mpc_mrt_interface_->getReferenceManager().setTargetTrajectories(target_trajectories);
  while (!mpc_mrt_interface_->initialPolicyReceived() && ros::ok() && ros::master::check())
  {
    mpc_mrt_interface_->advanceMpc();
    ros::WallRate(legged_interface_->mpcSettings().mrtDesiredFrequency_).sleep();
  }
  ROS_INFO_STREAM("Initial policy has been received.");

  mpc_running_ = true;
}

void Ocs2Controller::update(const ros::Time& time, const ros::Duration& period)
{
  // Simulation
  //  mpc_mrt_interface_->setCurrentObservation(current_observation_);
  //  mpc_mrt_interface_->updatePolicy();
  //  const auto dt = period.toSec();
  //  ocs2::SystemObservation next_observation;
  //  next_observation.time = current_observation_.time + dt;
  //  mpc_mrt_interface_->rolloutPolicy(current_observation_.time, current_observation_.state, dt, next_observation.state,
  //                                    next_observation.input, next_observation.mode);
  //  current_observation_ = next_observation;

  // State Estimate
  current_observation_.time = time.toSec();
  current_observation_.state = state_estimate_->update();

  // Update the current state of the system
  mpc_mrt_interface_->setCurrentObservation(current_observation_);

  // Load the latest MPC policy
  //  if (!mpc_mrt_interface_->updatePolicy())
  //    return;
  //  ROS_INFO_STREAM(time);
  mpc_mrt_interface_->updatePolicy();

  // Evaluate the current policy
  ocs2::vector_t optimized_state;  // Evaluation of the optimized state trajectory.
  ocs2::vector_t optimized_input;  // Evaluation of the optimized input trajectory.
  size_t planned_mode;             // The mode that is active at the time the policy is
                                   // evaluated at.
  mpc_mrt_interface_->evaluatePolicy(current_observation_.time, current_observation_.state, optimized_state,
                                     optimized_input, planned_mode);
  current_observation_.mode = planned_mode;
  // Set joint command
  // TODO: Whole Body Control
  for (size_t i = 0; i < legged_interface_->getCentroidalModelInfo().numThreeDofContacts; i++)
    current_observation_.input.segment<3>(i * 3) =
        centroidal_model::getContactForces(optimized_input, i, legged_interface_->getCentroidalModelInfo());

  CentroidalModelRbdConversions rbd_conversions(legged_interface_->getPinocchioInterface(),
                                                legged_interface_->getCentroidalModelInfo());

  vector_t kp = 150. * vector_t::Ones(legged_interface_->getCentroidalModelInfo().generalizedCoordinatesNum);
  vector_t kd = 25 * vector_t::Ones(legged_interface_->getCentroidalModelInfo().actuatedDofNum);
  kp.segment<6>(0) = vector_t::Zero(6);
  kd.segment<6>(0) = vector_t::Zero(6);

  vector_t torque = rbd_conversions.computeRbdTorqueFromCentroidalModelPD(
      current_observation_.state, optimized_input,
      vector_t::Zero(legged_interface_->getCentroidalModelInfo().actuatedDofNum),
      rbd_conversions.computeRbdStateFromCentroidalModel(current_observation_.state, optimized_input), kp, kd);

  for (size_t j = 0; j < legged_interface_->getCentroidalModelInfo().actuatedDofNum; ++j)
    if (std::abs(torque(6 + j)) < 100)
      hybrid_joint_handles_[j].setFeedforward(torque(6 + j));

  // Visualization
  visualizer_->update(current_observation_, mpc_mrt_interface_->getPolicy(), mpc_mrt_interface_->getCommand());

  // Publish the observation. Only needed for the command interface
  observation_publisher_.publish(ocs2::ros_msg_conversions::createObservationMsg(current_observation_));
}

Ocs2Controller::~Ocs2Controller()
{
  controller_running_ = false;
  if (mpc_thread_.joinable())
    mpc_thread_.join();
}

}  // namespace quad_ros

PLUGINLIB_EXPORT_CLASS(quad_ros::Ocs2Controller, controller_interface::ControllerBase)